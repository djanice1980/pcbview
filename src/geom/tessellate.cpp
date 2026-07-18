#include "geom/tessellate.h"
#include <cstdlib>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include "clipper2/clipper.h"
#include "mapbox/earcut.hpp"
#include "text/stroke_text.h"

namespace pcbview::geom {
namespace {

using namespace Clipper2Lib;

// kScale / toInt / toMm / toClipper now live in layer_art.h -- they are the
// shared currency of the LayerArt boundary, not private to this file.

// Matches the importer's convention: KiCad's stored angle is clockwise in raw
// file coordinates. Applied before the Y flip, same as pad positions.
Vec2 rotateKicad(Vec2 p, double degrees) {
    const double rad = -degrees * std::numbers::pi / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    return Vec2{p.x * c - p.y * s, p.x * s + p.y * c};
}

// normalizeWinding is now the shared helper in layer_art.h -- load-bearing at
// the LayerArt boundary, and used identically by the Gerber importer. (Measured
// origin of the need: toClipper() negates Y, which reverses the winding of every
// hand-built path relative to ClipperOffset output; overlapping opposite windings
// cancel to a hole under NonZero. Not fixable with FillRule::Positive, which
// would delete the negatively-wound shapes instead of merging them.)

Path64 circlePath(Vec2 center, double radius, int segments) {
    Path64 path;
    path.reserve(segments);
    for (int i = 0; i < segments; ++i) {
        const double t = 2.0 * std::numbers::pi * i / segments;
        path.push_back(toClipper(Vec2{center.x + radius * std::cos(t),
                                      center.y + radius * std::sin(t)}));
    }
    return path;
}

// A rounded rectangle in local space, rotated and translated into board space.
// corner == 0 gives a plain rectangle.
Path64 roundRectPath(Vec2 center, Vec2 size, double corner, double degrees,
                     int segments) {
    const double hx = size.x * 0.5;
    const double hy = size.y * 0.5;
    corner = std::min({corner, hx, hy});

    std::vector<Vec2> local;
    if (corner <= 0.0) {
        local = {Vec2{-hx, -hy}, Vec2{hx, -hy}, Vec2{hx, hy}, Vec2{-hx, hy}};
    } else {
        const int arc = std::max(2, segments / 4);
        // Corner centres, walked counter-clockwise starting bottom-right.
        const Vec2 pivots[4] = {Vec2{hx - corner, -(hy - corner)},
                                Vec2{hx - corner, hy - corner},
                                Vec2{-(hx - corner), hy - corner},
                                Vec2{-(hx - corner), -(hy - corner)}};
        const double startAngle[4] = {-std::numbers::pi / 2, 0.0,
                                      std::numbers::pi / 2, std::numbers::pi};
        for (int c = 0; c < 4; ++c) {
            for (int i = 0; i <= arc; ++i) {
                const double t =
                    startAngle[c] + (std::numbers::pi / 2) * i / arc;
                local.push_back(Vec2{pivots[c].x + corner * std::cos(t),
                                     pivots[c].y + corner * std::sin(t)});
            }
        }
    }

    Path64 path;
    path.reserve(local.size());
    for (const Vec2& p : local) {
        const Vec2 r = rotateKicad(p, degrees);
        path.push_back(toClipper(Vec2{center.x + r.x, center.y + r.y}));
    }
    return path;
}

// Stadium shape: a segment inflated by half the minor axis.
//
// Takes no segment count: like trackPaths, arc quality here comes from
// ClipperOffset's ArcTolerance rather than a fixed subdivision. Only the
// hand-built circle/roundrect paths use TessellateOptions::circleSegments.
Paths64 ovalPath(Vec2 center, Vec2 size, double degrees) {
    const double radius = std::min(size.x, size.y) * 0.5;
    const double span = std::max(size.x, size.y) * 0.5 - radius;

    // The long axis follows whichever dimension is larger.
    Vec2 a = (size.x >= size.y) ? Vec2{-span, 0.0} : Vec2{0.0, -span};
    Vec2 b = (size.x >= size.y) ? Vec2{span, 0.0} : Vec2{0.0, span};
    a = rotateKicad(a, degrees);
    b = rotateKicad(b, degrees);

    Path64 spine{toClipper(Vec2{center.x + a.x, center.y + a.y}),
                 toClipper(Vec2{center.x + b.x, center.y + b.y})};

    ClipperOffset co;
    co.ArcTolerance(kScale * 0.001);
    co.AddPath(spine, JoinType::Round, EndType::Round);
    Paths64 out;
    co.Execute(radius * kScale, out);
    return out;
}

Paths64 padPaths(const Pad& pad, int segments) {
    switch (pad.shape) {
        case PadShape::Circle:
            return Paths64{circlePath(pad.at, pad.size.x * 0.5, segments)};
        case PadShape::Rect:
            return Paths64{
                roundRectPath(pad.at, pad.size, 0.0, pad.rotation, segments)};
        case PadShape::RoundRect: {
            const double corner =
                std::min(pad.size.x, pad.size.y) * pad.roundrectRatio;
            return Paths64{roundRectPath(pad.at, pad.size, corner, pad.rotation,
                                         segments)};
        }
        case PadShape::Oval:
            return ovalPath(pad.at, pad.size, pad.rotation);
        default:
            // Custom pads are not implemented; fall back to the bounding rect so
            // the copper is over- rather than under-reported.
            return Paths64{
                roundRectPath(pad.at, pad.size, 0.0, pad.rotation, segments)};
    }
}

Paths64 trackPaths(const Track& track) {
    Path64 spine{toClipper(track.start), toClipper(track.end)};

    ClipperOffset co;
    co.ArcTolerance(kScale * 0.001);
    // Round caps and joins: this is how KiCad renders track ends.
    co.AddPath(spine, JoinType::Round, EndType::Round);
    Paths64 out;
    co.Execute(track.width * 0.5 * kScale, out);
    return out;
}

// --- Triangulation -------------------------------------------------------

struct Ring {
    std::vector<std::array<double, 2>> pts;
};

struct Shape {
    std::vector<Ring> rings;  // [0] outer, rest holes
};

void collectShapes(const PolyPath64& node, std::vector<Shape>& out) {
    for (size_t i = 0; i < node.Count(); ++i) {
        const PolyPath64* child = node.Child(i);
        if (child->IsHole()) {
            // Holes are attached to their parent below; a hole's own children
            // are fresh outer contours (an island inside a hole).
            collectShapes(*child, out);
            continue;
        }

        Shape shape;
        Ring outer;
        for (const Point64& p : child->Polygon()) {
            outer.pts.push_back({toMm(p.x), toMm(p.y)});
        }
        if (outer.pts.size() < 3) continue;
        shape.rings.push_back(std::move(outer));

        for (size_t h = 0; h < child->Count(); ++h) {
            const PolyPath64* hole = child->Child(h);
            if (!hole->IsHole()) continue;

            Ring ring;
            for (const Point64& p : hole->Polygon()) {
                ring.pts.push_back({toMm(p.x), toMm(p.y)});
            }
            if (ring.pts.size() >= 3) shape.rings.push_back(std::move(ring));
        }
        out.push_back(std::move(shape));

        // Islands nested inside this shape's holes.
        collectShapes(*child, out);
    }
}

std::vector<Shape> unionToShapes(const Paths64& paths) {
    Clipper64 clipper;
    clipper.AddSubject(paths);
    PolyTree64 tree;
    clipper.Execute(ClipType::Union, FillRule::NonZero, tree);

    std::vector<Shape> shapes;
    collectShapes(tree, shapes);
    return shapes;
}

// Extrude one shape between zBottom and zTop, appending to mesh.
void extrude(const Shape& shape, double zBottom, double zTop, Mesh& mesh) {
    std::vector<std::vector<std::array<double, 2>>> polygon;
    polygon.reserve(shape.rings.size());
    for (const Ring& ring : shape.rings) polygon.push_back(ring.pts);

    const std::vector<uint32_t> tris = mapbox::earcut<uint32_t>(polygon);
    if (tris.empty()) return;

    // Flatten ring points in the same order earcut indexed them.
    std::vector<std::array<double, 2>> flat;
    for (const auto& ring : polygon) {
        flat.insert(flat.end(), ring.begin(), ring.end());
    }

    const uint32_t topBase = static_cast<uint32_t>(mesh.vertices.size());
    for (const auto& p : flat) {
        mesh.vertices.push_back(Vertex{
            {static_cast<float>(p[0]), static_cast<float>(p[1]),
             static_cast<float>(zTop)},
            {0.0f, 0.0f, 1.0f}});
    }
    const uint32_t bottomBase = static_cast<uint32_t>(mesh.vertices.size());
    for (const auto& p : flat) {
        mesh.vertices.push_back(Vertex{
            {static_cast<float>(p[0]), static_cast<float>(p[1]),
             static_cast<float>(zBottom)},
            {0.0f, 0.0f, -1.0f}});
    }

    for (size_t i = 0; i + 2 < tris.size(); i += 3) {
        // Top cap, counter-clockwise seen from +Z.
        mesh.indices.push_back(topBase + tris[i]);
        mesh.indices.push_back(topBase + tris[i + 1]);
        mesh.indices.push_back(topBase + tris[i + 2]);
        // Bottom cap, wound the other way so its face points at -Z.
        mesh.indices.push_back(bottomBase + tris[i + 2]);
        mesh.indices.push_back(bottomBase + tris[i + 1]);
        mesh.indices.push_back(bottomBase + tris[i]);
    }

    // Side walls. Each ring contributes a quad per edge, with its own vertices
    // so the wall normal stays sharp instead of averaging into the caps.
    for (const auto& ring : polygon) {
        const size_t n = ring.size();
        if (n < 3) continue;

        // Signed area decides which way "outward" is for this ring: earcut's
        // outer rings and hole rings wind oppositely, and the wall normal must
        // follow suit or holes light up inside-out.
        double area = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const auto& a = ring[i];
            const auto& b = ring[(i + 1) % n];
            area += a[0] * b[1] - b[0] * a[1];
        }
        const double dir = (area >= 0.0) ? 1.0 : -1.0;

        for (size_t i = 0; i < n; ++i) {
            const auto& a = ring[i];
            const auto& b = ring[(i + 1) % n];

            const double ex = b[0] - a[0];
            const double ey = b[1] - a[1];
            const double len = std::hypot(ex, ey);
            if (len < 1e-12) continue;

            const float nx = static_cast<float>(dir * ey / len);
            const float ny = static_cast<float>(-dir * ex / len);

            const uint32_t base = static_cast<uint32_t>(mesh.vertices.size());
            const float ax = static_cast<float>(a[0]);
            const float ay = static_cast<float>(a[1]);
            const float bx = static_cast<float>(b[0]);
            const float by = static_cast<float>(b[1]);
            const float zt = static_cast<float>(zTop);
            const float zb = static_cast<float>(zBottom);

            mesh.vertices.push_back(Vertex{{ax, ay, zb}, {nx, ny, 0.0f}});
            mesh.vertices.push_back(Vertex{{bx, by, zb}, {nx, ny, 0.0f}});
            mesh.vertices.push_back(Vertex{{bx, by, zt}, {nx, ny, 0.0f}});
            mesh.vertices.push_back(Vertex{{ax, ay, zt}, {nx, ny, 0.0f}});

            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 1);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 0);
            mesh.indices.push_back(base + 2);
            mesh.indices.push_back(base + 3);
        }
    }
}

// Turn one drawn shape into filled polygons.
//
// KiCad's model: `filled` fills the interior, and `width` strokes the outline on
// top. They are independent -- an unfilled circle with width is a ring, a filled
// polygon with width is the polygon grown by half the stroke. Both are common on
// silkscreen, so both must be honoured or footprint outlines come out thin and
// courtyard rings vanish.
Paths64 graphicPaths(const Graphic& g, int segments, BoardModel* warnings) {
    Paths64 out;
    const double halfWidth = g.width * 0.5;

    const auto strokeOpenPath = [&](const Path64& spine, EndType endType) {
        if (halfWidth <= 0.0) return;
        ClipperOffset co;
        co.ArcTolerance(kScale * 0.001);
        co.AddPath(spine, JoinType::Round, endType);
        Paths64 grown;
        co.Execute(halfWidth * kScale, grown);
        out.insert(out.end(), grown.begin(), grown.end());
    };

    switch (g.kind) {
        case GraphicKind::Segment: {
            strokeOpenPath(Path64{toClipper(g.start), toClipper(g.end)},
                           EndType::Round);
            break;
        }
        case GraphicKind::Circle: {
            const double radius =
                std::hypot(g.end.x - g.start.x, g.end.y - g.start.y);
            if (radius <= 0.0) break;
            if (g.filled) {
                out.push_back(circlePath(g.start, radius + halfWidth, segments));
            } else if (halfWidth > 0.0) {
                // A ring: outer disc minus inner disc. Emitting both with
                // opposite winding lets the caller's NonZero union carve it.
                Path64 outer = circlePath(g.start, radius + halfWidth, segments);
                Path64 inner = circlePath(g.start, std::max(radius - halfWidth, 0.0),
                                          segments);
                if (Area(outer) < 0) std::reverse(outer.begin(), outer.end());
                if (Area(inner) > 0) std::reverse(inner.begin(), inner.end());
                out.push_back(std::move(outer));
                out.push_back(std::move(inner));
            }
            break;
        }
        case GraphicKind::Polygon: {
            Path64 poly;
            for (const Vec2& p : g.points) poly.push_back(toClipper(p));
            if (poly.size() < 3) break;
            if (g.filled) {
                out.push_back(poly);
            } else if (halfWidth > 0.0) {
                // An UNFILLED outline is a frame: the polygon grown by halfWidth
                // minus the polygon shrunk by halfWidth. Built explicitly as
                // outer + reversed inner (like the ring case above) so the
                // caller's NonZero union carves the hole. EndType::Joined was
                // wrong here -- it strokes a closed loop into two SAME-wound
                // contours, so the union filled the interior solid (a large
                // fill-no silk rectangle came out as a white box).
                ClipperOffset coOut;
                coOut.ArcTolerance(kScale * 0.001);
                coOut.AddPath(poly, JoinType::Miter, EndType::Polygon);
                Paths64 outer;
                coOut.Execute(halfWidth * kScale, outer);

                ClipperOffset coIn;
                coIn.ArcTolerance(kScale * 0.001);
                coIn.AddPath(poly, JoinType::Miter, EndType::Polygon);
                Paths64 inner;
                coIn.Execute(-halfWidth * kScale, inner);

                for (Path64& p : outer) {
                    if (Area(p) < 0) std::reverse(p.begin(), p.end());
                    out.push_back(std::move(p));
                }
                for (Path64& p : inner) {
                    if (Area(p) > 0) std::reverse(p.begin(), p.end());
                    out.push_back(std::move(p));
                }
            }
            break;
        }
        case GraphicKind::Arc: {
            if (warnings) {
                warnings->warnings.push_back(
                    "arc graphics are not implemented; approximated as a chord");
            }
            strokeOpenPath(Path64{toClipper(g.start), toClipper(g.mid),
                                  toClipper(g.end)},
                           EndType::Round);
            break;
        }
        default:
            break;
    }
    return out;
}

Paths64 drillPaths(const BoardModel& board, int segments) {
    Paths64 holes;
    for (const Drill& drill : board.drills) {
        holes.push_back(circlePath(drill.at, drill.diameter * 0.5, segments));
    }
    normalizeWinding(holes);
    return holes;
}

}  // namespace

size_t BoardMesh::totalTriangles() const {
    size_t n = 0;
    for (const Part& part : parts) n += part.mesh.triangleCount();
    return n;
}

size_t BoardMesh::totalVertices() const {
    size_t n = 0;
    for (const Part& part : parts) n += part.mesh.vertices.size();
    return n;
}

LayerArt buildLayerArt(const BoardModel& board, const TessellateOptions& opts) {
    LayerArt art;
    art.sourcePath = board.sourcePath;
    art.thickness = board.thickness;
    art.copperThickness = board.copperThickness;
    art.maskThickness = board.maskThickness;
    art.silkThickness = board.silkThickness;
    art.warnings = board.warnings;
    art.drills = drillPaths(board, opts.circleSegments);
    for (const Drill& drill : board.drills) {
        if (drill.plated)
            art.barrels.push_back(
                circlePath(drill.at, drill.diameter * 0.5, opts.circleSegments));
    }
    normalizeWinding(art.barrels);

    // The bare profile, drills still intact. assemble() subtracts them, because
    // soldermask must tent ACROSS a drill and so needs the un-drilled outline.
    for (const Polygon& poly : board.outline) {
        Path64 path;
        for (const Vec2& p : poly.outer) path.push_back(toClipper(p));
        if (path.size() >= 3) art.outline.push_back(std::move(path));
    }
    normalizeWinding(art.outline);

    // --- Copper, one ArtLayer per layer ---
    for (int layerIndex : board.copperLayers()) {
        const Layer& layer = board.layers[layerIndex];
        const bool isOuter = (layer.name == "F.Cu" || layer.name == "B.Cu");
        if (!isOuter && !opts.innerLayers) continue;

        Paths64 copper;
        for (const Track& track : board.tracks) {
            if (track.layer != layerIndex) continue;
            const Paths64 p = trackPaths(track);
            copper.insert(copper.end(), p.begin(), p.end());
        }
        for (const Pad& pad : board.pads) {
            if (std::find(pad.layers.begin(), pad.layers.end(), layerIndex) ==
                pad.layers.end()) {
                continue;
            }
            const Paths64 p = padPaths(pad, opts.circleSegments);
            copper.insert(copper.end(), p.begin(), p.end());
        }
        for (const Via& via : board.vias) {
            // Blind/buried vias only span their own range; -1 means the layer
            // name was unknown, so be conservative and include it.
            const bool spans =
                (via.fromLayer < 0 || via.toLayer < 0) ||
                (layer.stackIndex >=
                     board.layers[via.fromLayer].stackIndex &&
                 layer.stackIndex <= board.layers[via.toLayer].stackIndex);
            if (!spans) continue;
            copper.push_back(circlePath(via.at, via.size * 0.5, opts.circleSegments));
        }
        for (const ZoneFill& zone : board.zones) {
            if (zone.layer != layerIndex) continue;
            for (const Polygon& poly : zone.polygons) {
                Path64 path;
                for (const Vec2& p : poly.outer) path.push_back(toClipper(p));
                if (path.size() >= 3) copper.push_back(std::move(path));
            }
        }
        if (copper.empty()) continue;

        // Tracks, pads, vias and zone fills arrive with inconsistent winding.
        // They must agree before any boolean or they cancel where they overlap.
        normalizeWinding(copper);

        ArtLayer al;
        al.name = layer.name;
        al.kind = LayerKind::Copper;
        // Layer::z is the bottom of the foil, correct for every layer including
        // B.Cu -- the stackup places them, so no special case here.
        al.z = layer.z;
        al.thickness = layer.thickness;
        al.art = std::move(copper);
        art.layers.push_back(std::move(al));
    }

    // --- Soldermask openings, one ArtLayer per side ---
    //
    // ArtLayer::art for a mask is the OPENINGS, not the film. assemble() derives
    // the film as (outline - openings), which is also what makes via tenting
    // free: a tented via carries no mask layer, so it contributes no opening and
    // the film simply closes over it.
    if (opts.soldermask && !art.outline.empty()) {
        for (const char* name : {"F.Mask", "B.Mask"}) {
            const int layerIndex = board.layerIndex(name);
            if (layerIndex < 0) continue;
            const Layer& maskLayer = board.layers[layerIndex];

            Paths64 openings;
            int openingCount = 0;
            for (const Pad& pad : board.pads) {
                if (std::find(pad.maskLayers.begin(), pad.maskLayers.end(),
                              layerIndex) == pad.maskLayers.end()) {
                    continue;
                }
                ++openingCount;
                Paths64 shape = padPaths(pad, opts.circleSegments);

                // The opening is the pad grown by pad_to_mask_clearance. Zero on
                // many boards, in which case the opening is the pad exactly.
                if (board.padToMaskClearance > 0.0) {
                    normalizeWinding(shape);
                    ClipperOffset co;
                    co.ArcTolerance(kScale * 0.001);
                    co.AddPaths(shape, JoinType::Round, EndType::Polygon);
                    Paths64 grown;
                    co.Execute(board.padToMaskClearance * kScale, grown);
                    shape = std::move(grown);
                }
                openings.insert(openings.end(), shape.begin(), shape.end());
            }

            normalizeWinding(openings);

            ArtLayer al;
            al.name = name;
            al.kind = LayerKind::Soldermask;
            al.z = maskLayer.z;
            al.thickness = maskLayer.thickness;
            al.art = std::move(openings);
            al.openings = openingCount;
            art.layers.push_back(std::move(al));
        }
    }

    // --- Silkscreen, one ArtLayer per side ---
    if (opts.silkscreen && !art.outline.empty()) {
        for (const char* name : {"F.SilkS", "B.SilkS"}) {
            const int layerIndex = board.layerIndex(name);
            if (layerIndex < 0) continue;
            const Layer& silkLayer = board.layers[layerIndex];

            Paths64 ink;
            for (const Graphic& g : board.graphics) {
                if (g.layer != layerIndex) continue;
                const Paths64 p = graphicPaths(g, opts.circleSegments, nullptr);
                ink.insert(ink.end(), p.begin(), p.end());
            }

            // Text: stroke centrelines from the Newstroke font, thickened with a
            // round pen exactly like a track.
            for (const TextItem& t : board.texts) {
                if (t.layer != layerIndex) continue;
                if (t.thickness <= 0.0) continue;

                text::TextStyle style;
                style.size = t.size;
                style.thickness = t.thickness;
                style.rotation = t.rotation;
                style.mirror = t.mirror;
                style.italic = t.italic;

                for (const text::Polyline& line :
                     text::layout(t.content, t.at, style)) {
                    Path64 spine;
                    for (const Vec2& p : line) spine.push_back(toClipper(p));
                    if (spine.size() < 2) continue;

                    ClipperOffset co;
                    co.ArcTolerance(kScale * 0.001);
                    co.AddPath(spine, JoinType::Round, EndType::Round);
                    Paths64 stroked;
                    co.Execute(t.thickness * 0.5 * kScale, stroked);
                    ink.insert(ink.end(), stroked.begin(), stroked.end());
                }
            }
            if (ink.empty()) continue;

            // Union the strokes/fills with NonZero, and DO NOT normalise winding:
            // a real enclosed hole (an unfilled rectangle/polygon frame, a ring)
            // comes out of the union as a negative-wound inner contour, and
            // assemble() clips silk with NonZero -- which keeps the hole only while
            // that contour stays negative. normalizeWinding() flips it positive and
            // the NonZero fill then fills the hole solid (a `fill no` silk box came
            // out as a white block). Stroked TEXT has no enclosed contours, so it
            // was unaffected and hid the bug. The union already yields Clipper's
            // canonical winding (outers +, holes -), which is exactly what we want.
            Clipper64 unite;
            unite.AddSubject(ink);
            Paths64 united;
            unite.Execute(ClipType::Union, FillRule::NonZero, united);

            ArtLayer al;
            al.name = name;
            al.kind = LayerKind::Silkscreen;
            al.z = silkLayer.z;
            al.thickness = silkLayer.thickness;
            al.art = std::move(united);
            art.layers.push_back(std::move(al));
        }
    }

    return art;
}

BoardMesh assemble(const LayerArt& art, const TessellateOptions& opts) {
    (void)opts;
    BoardMesh out;

    // --- Board area: outline minus every drill ---
    //
    // Computed once and reused: extruded as the substrate, and used as the clip
    // for every copper layer. Copper cannot exist where there is no board, and
    // copper & (outline - drills) is exactly (copper & outline) - drills by set
    // algebra, so one intersection does both jobs.
    Paths64 boardArea;
    if (!art.outline.empty()) {
        Clipper64 clipper;
        clipper.AddSubject(art.outline);
        if (!art.drills.empty()) clipper.AddClip(art.drills);
        clipper.Execute(art.drills.empty() ? ClipType::Union : ClipType::Difference,
                        FillRule::NonZero, boardArea);
    }

    // The dielectric core. Its TOP is the underside of the topmost copper; its
    // BOTTOM is the top of the bottommost copper -- the foils and masks sit
    // outside it. Derived from the copper actually present, not by name.
    //
    // A single-copper board (single-sided or flex) breaks the "between two
    // foils" model: the topmost and bottommost copper are the SAME layer, so
    // top-of-core (its bottom face) lands BELOW bottom-of-core (its top face)
    // and the extrusion inverts -- flipped normals, swapped caps, depth-test
    // tearing. When that happens the core is instead a solid slab from the board
    // bottom up to the single foil's underside.
    double coreTop = art.thickness - art.maskThickness - art.copperThickness;
    double coreBottom = 0.0;
    double topFoil = -1e18, bottomFoilTop = 1e18;
    bool haveCopper = false;
    for (const ArtLayer& al : art.layers) {
        if (al.kind != LayerKind::Copper) continue;
        topFoil = std::max(topFoil, al.z);
        bottomFoilTop = std::min(bottomFoilTop, al.z + al.thickness);
        haveCopper = true;
    }
    if (haveCopper) {
        coreTop = topFoil;
        // Only sandwich between foils when they genuinely bracket a gap;
        // otherwise fall the core to the board bottom.
        coreBottom = (bottomFoilTop < topFoil - 1e-6) ? bottomFoilTop : 0.0;
    }

    // --- Substrate, sliced into the dielectric slabs BETWEEN copper foils ---
    //
    // A multilayer board is copper / prepreg / copper / core / copper ... : the
    // dielectric is not one block but a stack of slabs, each filling the gap
    // between two adjacent foils. Slicing it this way is what lets the exploded
    // view show an inner trace layer in its true position -- sandwiched between
    // two dielectric layers -- instead of the inner copper sliding through a
    // single monolithic slab. Each slab sorts to its own explode rank by its own
    // centre Z, so it interleaves with the foils automatically.
    if (!boardArea.empty()) {
        Clipper64 clipper;
        clipper.AddSubject(boardArea);
        PolyTree64 tree;
        clipper.Execute(ClipType::Union, FillRule::NonZero, tree);

        std::vector<Shape> shapes;
        collectShapes(tree, shapes);

        // The dielectric is cut at each INNER foil's mid-plane, not at its faces.
        // Cutting at the faces would leave a copper-thickness gap between slabs
        // that shows as a line on the collapsed board's edge; cutting at the
        // centres makes adjacent slabs ABUT (a shared, coplanar wall renders
        // seamlessly), so a collapsed board reads as one solid block while each
        // inner foil still sits exactly on a slab boundary -- so it peels out
        // cleanly between two slabs. Fewer than two cuts falls back to one slab
        // (2-layer / single-sided / flex).
        std::vector<std::pair<double, double>> spans;
        {
            std::vector<double> bounds{coreBottom, coreTop};
            for (const ArtLayer& al : art.layers) {
                if (al.kind != LayerKind::Copper) continue;
                const double centre = al.z + al.thickness * 0.5;
                // Only the foils strictly inside the core cut it; the outer foils
                // (F/B.Cu) bound it and lie on its faces.
                if (centre > coreBottom + 1e-6 && centre < coreTop - 1e-6) {
                    bounds.push_back(centre);
                }
            }
            std::sort(bounds.begin(), bounds.end());
            for (size_t i = 0; i + 1 < bounds.size(); ++i) {
                if (bounds[i + 1] > bounds[i] + 1e-6) {
                    spans.emplace_back(bounds[i], bounds[i + 1]);
                }
            }
        }
        if (spans.empty()) spans.emplace_back(coreBottom, coreTop);

        // Every slab is named "substrate": one stackup toggle drives them all,
        // and the appearance picker recolours by material, not by name.
        for (const auto& span : spans) {
            Part part;
            part.material = Material::Substrate;
            part.name = "substrate";
            for (const Shape& shape : shapes) {
                extrude(shape, span.first, span.second, part.mesh);
            }
            if (!part.mesh.indices.empty()) out.parts.push_back(std::move(part));
        }
    }

    // --- One part per art layer ---
    for (const ArtLayer& al : art.layers) {
        Clipper64 clipper;
        ClipType op = ClipType::Union;
        Material material = Material::Copper;

        switch (al.kind) {
            case LayerKind::Copper:
                // Clip to the board: trims copper at the profile (edge-connector
                // fingers are drawn overhanging and routed off during fab) and
                // punches the drills in one pass.
                material = Material::Copper;
                clipper.AddSubject(al.art);
                if (!boardArea.empty()) {
                    clipper.AddClip(boardArea);
                    op = ClipType::Intersection;
                } else if (!art.drills.empty()) {
                    clipper.AddClip(art.drills);
                    op = ClipType::Difference;
                }
                break;

            case LayerKind::Soldermask:
                // Film = OUTLINE minus openings. Note outline, not boardArea, and
                // no drill subtraction: mask tents ACROSS a hole, which is the
                // whole meaning of tenting. Subtracting drills here would make
                // tenting impossible and render every via as a punched dot.
                material = Material::Soldermask;
                if (art.outline.empty()) continue;
                clipper.AddSubject(art.outline);
                if (!al.art.empty()) {
                    clipper.AddClip(al.art);
                    op = ClipType::Difference;
                }
                break;

            case LayerKind::Silkscreen:
                // Ink is clipped to the profile but NOT to the mask openings --
                // KiCad's (subtractmaskfromsilk no), which is what the fab does.
                material = Material::Silkscreen;
                if (art.outline.empty()) continue;
                clipper.AddSubject(al.art);
                clipper.AddClip(art.outline);
                op = ClipType::Intersection;
                break;

            default:
                continue;
        }

        PolyTree64 tree;
        clipper.Execute(op, FillRule::NonZero, tree);

        std::vector<Shape> shapes;
        collectShapes(tree, shapes);

        Part part;
        part.material = material;
        part.name = al.name;
        part.maskOpenings = al.openings;
        for (const Shape& shape : shapes) {
            extrude(shape, al.z, al.z + al.thickness, part.mesh);
        }
        if (!part.mesh.indices.empty()) out.parts.push_back(std::move(part));
    }

    // --- Via barrels: each plated hole lined with a copper tube spanning the
    // whole copper stack, collected into ONE part ("vias"). A via is a single
    // plated barrel through the board; the renderer pins this part to explode
    // rank 0 so it stays intact and centred while the layers peel around it,
    // instead of being sliced like the substrate. ---
    if (!art.barrels.empty()) {
        double botZ = 1e18, topZ = -1e18;
        for (const ArtLayer& al : art.layers) {
            if (al.kind != LayerKind::Copper) continue;
            botZ = std::min(botZ, al.z);
            topZ = std::max(topZ, al.z + al.thickness);
        }
        if (botZ < topZ) {
            // The barrel sits JUST INSIDE the drilled wall (radial `gap`), not on
            // it: the copper and substrate hole edges are exactly at the drill
            // radius, so a barrel of that radius shares a wall with every layer at
            // every via and z-fights into heavy speckle (bad on via-dense boards).
            // Insetting puts both the outer and inner barrel walls inside the bore,
            // where nothing else lives. A tube = inset(gap) minus inset(gap+wall);
            // holes too small for the void just render as a solid plug.
            const double gap = 0.03;   // clearance from the drilled wall (mm)
            const double wall = 0.15;  // plating thickness shown (mm)
            const auto inset = [&](double d) {
                ClipperOffset co;
                co.ArcTolerance(kScale * 0.001);
                co.AddPaths(art.barrels, JoinType::Round, EndType::Polygon);
                Paths64 r;
                co.Execute(-d * kScale, r);
                return r;
            };
            const Paths64 outer = inset(gap);
            const Paths64 inner = inset(gap + wall);

            Clipper64 diff;
            diff.AddSubject(outer);
            if (!inner.empty()) diff.AddClip(inner);
            Paths64 rings;
            diff.Execute(inner.empty() ? ClipType::Union : ClipType::Difference,
                         FillRule::NonZero, rings);

            // Clip the barrels to the BOARD. A castellated hole sits ON the
            // outline: the routed edge cuts the plated hole in half, so the fab
            // plates a HALF-barrel hugging the edge -- the outboard half must
            // not hang in space. (art.outline carries the internal cutouts as
            // opposite-wound holes, so barrels are clipped at those edges too.)
            Clipper64 clip;
            clip.AddSubject(rings);
            clip.AddClip(art.outline);
            PolyTree64 tree;
            clip.Execute(ClipType::Intersection, FillRule::NonZero, tree);

            std::vector<Shape> shapes;
            collectShapes(tree, shapes);

            Part part;
            part.material = Material::Copper;
            part.name = "vias";
            for (const Shape& shape : shapes)
                extrude(shape, botZ, topZ, part.mesh);
            if (!part.mesh.indices.empty()) out.parts.push_back(std::move(part));
        }
    }

    // --- Bounds ---
    bool first = true;
    for (const Part& part : out.parts) {
        for (const Vertex& v : part.mesh.vertices) {
            for (int i = 0; i < 3; ++i) {
                if (first) {
                    out.bounds.min[i] = out.bounds.max[i] = v.position[i];
                } else {
                    out.bounds.min[i] = std::min(out.bounds.min[i],
                                                 static_cast<double>(v.position[i]));
                    out.bounds.max[i] = std::max(out.bounds.max[i],
                                                 static_cast<double>(v.position[i]));
                }
            }
            first = false;
        }
    }
    return out;
}

BoardMesh tessellate(const BoardModel& board, const TessellateOptions& opts) {
    return assemble(buildLayerArt(board, opts), opts);
}

void applyThickness(LayerArt& art, double finishedThicknessMm) {
    if (finishedThicknessMm <= 0.0) return;
    const double tc = art.copperThickness;
    const double tm = art.maskThickness;
    const double ts = art.silkThickness;

    // Copper, ordered top to bottom by current z. F.Cu highest, B.Cu lowest.
    std::vector<ArtLayer*> copper;
    for (ArtLayer& al : art.layers)
        if (al.kind == LayerKind::Copper) copper.push_back(&al);
    std::sort(copper.begin(), copper.end(),
              [](const ArtLayer* a, const ArtLayer* b) { return a->z > b->z; });

    const int n = static_cast<int>(copper.size());
    // Dielectric flexes; copper and mask do not. Same formula the importers use.
    double diel = (n > 1)
                      ? (finishedThicknessMm - n * tc - 2.0 * tm) / (n - 1)
                      : 0.0;
    if (diel < 0.0) diel = 0.0;
    const double pitch = tc + diel;

    for (int i = 0; i < n; ++i) {
        copper[i]->thickness = tc;
        copper[i]->z = finishedThicknessMm - tm - i * pitch - tc;
    }
    for (ArtLayer& al : art.layers) {
        const bool front = al.name.rfind("F.", 0) == 0;
        if (al.kind == LayerKind::Soldermask) {
            al.thickness = tm;
            al.z = front ? finishedThicknessMm - tm : 0.0;
        } else if (al.kind == LayerKind::Silkscreen) {
            al.thickness = ts;
            al.z = front ? finishedThicknessMm : -ts;
        }
    }
    art.thickness = finishedThicknessMm;
}

}  // namespace pcbview::geom
