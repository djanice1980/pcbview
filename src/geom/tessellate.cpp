#include "geom/tessellate.h"
#include <cstdlib>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numbers>
#include <queue>

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
// Chord a three-point arc (start, mid, end) into a polyline, mapping each
// point through `place` so the caller controls the frame. Collinear points
// degrade to the straight chord rather than dividing by ~0 -- the same
// treatment KiCad arc TRACKS get.
template <typename Place>
Path64 arcThrough(Vec2 a, Vec2 m, Vec2 b, Place place) {
    const double d = 2.0 * (a.x * (m.y - b.y) + m.x * (b.y - a.y) +
                            b.x * (a.y - m.y));
    if (std::abs(d) < 1e-12) return Path64{place(a), place(b)};

    const double a2 = a.x * a.x + a.y * a.y, m2 = m.x * m.x + m.y * m.y;
    const double b2 = b.x * b.x + b.y * b.y;
    const double ox = (a2 * (m.y - b.y) + m2 * (b.y - a.y) + b2 * (a.y - m.y)) / d;
    const double oy = (a2 * (b.x - m.x) + m2 * (a.x - b.x) + b2 * (m.x - a.x)) / d;
    const double r = std::hypot(a.x - ox, a.y - oy);

    const double twoPi = 2.0 * std::numbers::pi;
    const auto norm = [twoPi](double v) {
        while (v < 0) v += twoPi;
        while (v >= twoPi) v -= twoPi;
        return v;
    };
    const double a0 = std::atan2(a.y - oy, a.x - ox);
    const double am = std::atan2(m.y - oy, m.x - ox);
    const double a1 = std::atan2(b.y - oy, b.x - ox);
    const bool ccw = norm(am - a0) < norm(a1 - a0);
    const double sweep = ccw ? norm(a1 - a0) : -norm(a0 - a1);

    const int segs = std::max(
        2, static_cast<int>(std::ceil(std::abs(sweep) / twoPi * 48)));
    Path64 out;
    for (int k = 0; k <= segs; ++k) {
        const double t = a0 + sweep * k / segs;
        out.push_back(place(Vec2{ox + r * std::cos(t), oy + r * std::sin(t)}));
    }
    return out;
}

// A trapezoid pad. KiCad skews the rect's opposite edges by half the delta;
// this reproduces its corner formula exactly rather than approximating with
// the bounding rect, which over-reported copper on every tapered pad.
Path64 trapezoidPath(const Pad& pad) {
    const double dx = pad.size.x * 0.5, dy = pad.size.y * 0.5;
    const double ddx = pad.rectDelta.x * 0.5, ddy = pad.rectDelta.y * 0.5;
    // Signs verified against KiCad's own plotter, not derived: for
    // `(rect_delta 0 1)` KiCad emits (-2,-0.5)(2,-0.5)(1,0.5)(-1,0.5) in
    // APERTURE space, i.e. wide at world y = -dy. This code builds in KiCad's
    // Y-DOWN space and is flipped on the way out, so the wide edge must sit at
    // kicad +dy -- the opposite sign to the naive reading. The x terms (ddx)
    // need no flip because x is not mirrored.
    //
    // A rect or oval pad is symmetric in y, so this error was invisible on
    // every other pad shape, and invisible to area and bounding-box checks on
    // a trapezoid too: mirroring changes neither.
    const Vec2 local[4] = {{-dx + ddy, -dy - ddx},
                           {-dx - ddy, dy + ddx},
                           {dx + ddy, dy - ddx},
                           {dx - ddy, -dy + ddx}};
    Path64 out;
    for (const Vec2& v : local) {
        const Vec2 r = rotateKicad(v, pad.rotation);
        out.push_back(toClipper(Vec2{pad.at.x + r.x, pad.at.y + r.y}));
    }
    return out;
}

// A custom pad: the anchor shape unioned with every drawn primitive.
//
// Primitives are authored in the pad's own un-rotated frame, so each is built
// locally and then rotated with the pad. Strokes are offset by half their
// width with round caps -- the same treatment tracks get, because that is what
// KiCad draws.
Paths64 customPadPaths(const Pad& pad, int segments) {
    // The anchor. KiCad's custom pads carry a circle or rect anchor sized by
    // `size`; without it a pad made only of strokes would lose its centre.
    Paths64 out;
    if (pad.size.x > 0.0 && pad.size.y > 0.0) {
        if (std::abs(pad.size.x - pad.size.y) < 1e-9)
            out.push_back(circlePath(pad.at, pad.size.x * 0.5, segments));
        else
            out.push_back(
                roundRectPath(pad.at, pad.size, 0.0, pad.rotation, segments));
    }

    const auto place = [&](Vec2 v) {
        const Vec2 r = rotateKicad(v, pad.rotation);
        return toClipper(Vec2{pad.at.x + r.x, pad.at.y + r.y});
    };
    const auto stroke = [&](const Path64& spine, double width, bool closed) {
        ClipperOffset co;
        co.ArcTolerance(kScale * 0.001);
        co.AddPath(spine, JoinType::Round,
                   closed ? EndType::Joined : EndType::Round);
        Paths64 r;
        co.Execute(std::max(width, 1e-6) * 0.5 * kScale, r);
        return r;
    };

    for (const Pad::Primitive& p : pad.primitives) {
        Paths64 shape;
        switch (p.kind) {
            case Pad::Primitive::Kind::Poly: {
                if (p.pts.size() < 3) break;
                Path64 poly;
                for (const Vec2& v : p.pts) poly.push_back(place(v));
                if (p.filled) {
                    shape.push_back(poly);
                    // A filled poly with a stroke width is fill PLUS outline.
                    if (p.width > 0.0) {
                        Paths64 e = stroke(poly, p.width, true);
                        shape.insert(shape.end(), e.begin(), e.end());
                    }
                } else {
                    shape = stroke(poly, p.width, true);
                }
                break;
            }
            case Pad::Primitive::Kind::Line: {
                if (p.pts.size() < 2) break;
                shape = stroke(Path64{place(p.pts[0]), place(p.pts[1])}, p.width,
                               false);
                break;
            }
            case Pad::Primitive::Kind::Rect: {
                if (p.pts.size() < 2) break;
                const Vec2 a = p.pts[0], b = p.pts[1];
                Path64 r{place(a), place(Vec2{b.x, a.y}), place(b),
                         place(Vec2{a.x, b.y})};
                if (p.filled) {
                    shape.push_back(r);
                    if (p.width > 0.0) {
                        Paths64 e = stroke(r, p.width, true);
                        shape.insert(shape.end(), e.begin(), e.end());
                    }
                } else {
                    shape = stroke(r, p.width, true);
                }
                break;
            }
            case Pad::Primitive::Kind::Circle: {
                if (p.pts.size() < 2) break;
                const double rad = std::hypot(p.pts[1].x - p.pts[0].x,
                                              p.pts[1].y - p.pts[0].y);
                const Vec2 c = rotateKicad(p.pts[0], pad.rotation);
                const Vec2 world{pad.at.x + c.x, pad.at.y + c.y};
                if (p.filled) {
                    shape.push_back(circlePath(world, rad + p.width * 0.5,
                                               segments));
                } else {
                    // A ring: outer minus inner.
                    Paths64 outer{circlePath(world, rad + p.width * 0.5, segments)};
                    Paths64 inner{circlePath(world, std::max(rad - p.width * 0.5, 0.0),
                                             segments)};
                    shape = BooleanOp(ClipType::Difference, FillRule::NonZero,
                                      outer, inner);
                }
                break;
            }
            case Pad::Primitive::Kind::Arc: {
                if (p.pts.size() < 2) break;
                // Chorded through the recorded mid point, then stroked. Three
                // points define the arc exactly; see the KiCad arc-track note.
                Path64 spine = arcThrough(p.pts[0], p.arcMid, p.pts[1], place);
                if (spine.size() >= 2) shape = stroke(spine, p.width, false);
                break;
            }
        }
        out.insert(out.end(), std::make_move_iterator(shape.begin()),
                   std::make_move_iterator(shape.end()));
    }

    if (out.empty())
        return Paths64{roundRectPath(pad.at, pad.size, 0.0, pad.rotation, segments)};
    // One region, not a pile of overlapping stamps.
    return BooleanOp(ClipType::Union, FillRule::NonZero, out, Paths64{});
}

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
        case PadShape::Trapezoid:
            return Paths64{trapezoidPath(pad)};
        case PadShape::Custom:
            return customPadPaths(pad, segments);
        default:
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

// One hole's outline: a circle, or a stadium for an oval drill.
//
// An oval drill is a SLOT -- the fab routes it rather than drilling it -- and
// approximating it with a circle on the larger axis removes copper the board
// actually has, in the one place (a mounting slot, a locating pin) where the
// size is load-bearing. `ovalPath` is the same helper oval PADS already use,
// so a slot and the pad around it are built from identical geometry.
Paths64 drillShape(const Drill& drill, int segments) {
    if (!drill.isSlot())
        return {circlePath(drill.at, drill.diameter * 0.5, segments)};
    return ovalPath(drill.at, drill.size, drill.rotation);
}

Paths64 drillPaths(const BoardModel& board, int segments) {
    Paths64 holes;
    for (const Drill& drill : board.drills) {
        Paths64 shape = drillShape(drill, segments);
        holes.insert(holes.end(), std::make_move_iterator(shape.begin()),
                     std::make_move_iterator(shape.end()));
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
    art.copperFinish = board.copperFinish;
    art.silkThickness = board.silkThickness;
    art.warnings = board.warnings;
    art.drills = drillPaths(board, opts.circleSegments);
    for (const Drill& drill : board.drills) {
        if (!drill.plated) continue;
        // A plated slot gets a plated WALL, following the same stadium.
        Paths64 shape = drillShape(drill, opts.circleSegments);
        art.barrels.insert(art.barrels.end(),
                           std::make_move_iterator(shape.begin()),
                           std::make_move_iterator(shape.end()));
    }
    normalizeWinding(art.barrels);

    // Blind/buried vias: the importer keeps them OUT of board.drills (their
    // bore does not reach the outer faces -- see the via-span note there), so
    // they surface here as partial-depth bores instead. Same span judgement as
    // the importer: layer span, never the `(via buried)` type token.
    {
        int lastCopperStack = 0;
        for (int idx : board.copperLayers()) {
            lastCopperStack =
                std::max(lastCopperStack, board.layers[idx].stackIndex);
        }
        for (const Via& via : board.vias) {
            if (via.drill <= 0.0 || via.fromLayer < 0 || via.toLayer < 0)
                continue;
            const int lo = std::min(board.layers[via.fromLayer].stackIndex,
                                    board.layers[via.toLayer].stackIndex);
            const int hi = std::max(board.layers[via.fromLayer].stackIndex,
                                    board.layers[via.toLayer].stackIndex);
            if (lo == 0 && hi == lastCopperStack) continue;  // full-stack via
            LayerArt::PartialBore bore;
            bore.path =
                circlePath(via.at, via.drill * 0.5, opts.circleSegments);
            if (Area(bore.path) < 0)
                std::reverse(bore.path.begin(), bore.path.end());
            bore.fromLayer = board.layers[via.fromLayer].name;
            bore.toLayer = board.layers[via.toLayer].name;
            art.partialBores.push_back(std::move(bore));
        }
    }

    // Net table + net-aware snap targets (pads and vias, same Y flip as
    // toClipper). Routed length = the sum of a net's track segments; vias are
    // counted, not measured (their barrel height is stackup-dependent and
    // tiny against trace lengths).
    //
    // netOf lives at function scope because the copper loop below needs the
    // same name -> index mapping to split each layer by net.
    std::map<std::string, int> netIdx;
    const auto netOf = [&](const std::string& n) -> int {
        if (n.empty()) return -1;
        const auto it = netIdx.find(n);
        if (it != netIdx.end()) return it->second;
        const int i = static_cast<int>(art.nets.size());
        netIdx.emplace(n, i);
        art.nets.push_back({n, 0.0, 0});
        return i;
    };
    {
        for (const Track& t : board.tracks) {
            const int i = netOf(t.net);
            if (i < 0) continue;
            art.nets[i].routedMm +=
                std::hypot(t.end.x - t.start.x, t.end.y - t.start.y);
            art.netSegments.push_back(
                {t.start.x, -t.start.y, t.end.x, -t.end.y, i});
        }
        for (const Via& via : board.vias) {
            const int i = netOf(via.net);
            if (i >= 0) ++art.nets[i].viaCount;
            art.netPoints.push_back(
                {{via.at.x, -via.at.y, board.thickness}, i});
            art.netPoints.push_back({{via.at.x, -via.at.y, 0.0}, i});
        }
        for (const Pad& pad : board.pads) {
            bool front = false;
            for (int li : pad.layers) {
                if (li >= 0 && board.layers[li].name == "F.Cu") {
                    front = true;
                    break;
                }
            }
            art.netPoints.push_back(
                {{pad.at.x, -pad.at.y, front ? board.thickness : 0.0},
                 netOf(pad.net)});
        }
    }

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

        // Copper is accumulated PER NET as well as in bulk: `copper` is what
        // everything downstream already consumed, `byNet` is what net
        // highlighting needs. Both describe the same area.
        Paths64 copper;
        std::map<int, Paths64> byNet;
        const auto addCopper = [&](Paths64 p, const std::string& net) {
            Paths64& bucket = byNet[netOf(net)];
            bucket.insert(bucket.end(), p.begin(), p.end());
            copper.insert(copper.end(), std::make_move_iterator(p.begin()),
                          std::make_move_iterator(p.end()));
        };

        for (const Track& track : board.tracks) {
            if (track.layer != layerIndex) continue;
            addCopper(trackPaths(track), track.net);
        }
        for (const Pad& pad : board.pads) {
            if (std::find(pad.layers.begin(), pad.layers.end(), layerIndex) ==
                pad.layers.end()) {
                continue;
            }
            addCopper(padPaths(pad, opts.circleSegments), pad.net);
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
            addCopper({circlePath(via.at, via.size * 0.5, opts.circleSegments)},
                      via.net);
        }
        for (const ZoneFill& zone : board.zones) {
            if (zone.layer != layerIndex) continue;
            Paths64 fill;
            for (const Polygon& poly : zone.polygons) {
                Path64 path;
                for (const Vec2& p : poly.outer) path.push_back(toClipper(p));
                if (path.size() >= 3) fill.push_back(std::move(path));
            }
            addCopper(std::move(fill), zone.net);
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
        for (auto& [net, paths] : byNet) {
            if (paths.empty()) continue;
            normalizeWinding(paths);
            al.netArt.push_back({net, std::move(paths)});
        }
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

    // --- Blind/buried bores, resolved from end-layer names to Z ranges ---
    //
    // A partial bore spans from the BOTTOM face of its lower end foil to the
    // TOP face of its upper end foil (the drill goes through both end foils'
    // copper). Resolved here, after the stackup is final, so a thickness
    // override has already moved the layers. A bore whose end layer is absent
    // (inner layers disabled, or a name mismatch) is dropped -- boring blindly
    // to a guessed depth would be worse than not boring at all.
    struct ResolvedBore {
        const Clipper2Lib::Path64* path;
        double z0, z1;
        std::string spanKey;  // groups barrels: one part per distinct span
    };
    std::vector<ResolvedBore> bores;
    for (const LayerArt::PartialBore& pb : art.partialBores) {
        const ArtLayer* a = nullptr;
        const ArtLayer* b = nullptr;
        for (const ArtLayer& al : art.layers) {
            if (al.kind != LayerKind::Copper) continue;
            if (al.name == pb.fromLayer) a = &al;
            if (al.name == pb.toLayer) b = &al;
        }
        if (!a || !b || a == b) continue;
        ResolvedBore rb;
        rb.path = &pb.path;
        rb.z0 = std::min(a->z, b->z);
        rb.z1 = std::max(a->z + a->thickness, b->z + b->thickness);
        // Key ordered by Z, so (F.Cu,In1.Cu) and (In1.Cu,F.Cu) share a part.
        const ArtLayer* lo = (a->z <= b->z) ? a : b;
        rb.spanKey = lo->name + "/" + ((lo == a) ? b : a)->name;
        bores.push_back(rb);
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
            // Blind/buried bores punch only the slabs inside their span; the
            // full-stack drills are already out of boardArea. Slabs a bore
            // crosses get their own boolean; untouched slabs reuse `shapes`.
            Paths64 slabBores;
            for (const ResolvedBore& rb : bores) {
                if (rb.z0 < span.second - 1e-6 && rb.z1 > span.first + 1e-6)
                    slabBores.push_back(*rb.path);
            }
            const std::vector<Shape>* slabShapes = &shapes;
            std::vector<Shape> bored;
            if (!slabBores.empty()) {
                Clipper64 c;
                c.AddSubject(boardArea);
                c.AddClip(slabBores);
                PolyTree64 t;
                c.Execute(ClipType::Difference, FillRule::NonZero, t);
                collectShapes(t, bored);
                slabShapes = &bored;
            }

            Part part;
            part.material = Material::Substrate;
            part.name = "substrate";
            for (const Shape& shape : *slabShapes) {
                extrude(shape, span.first, span.second, part.mesh);
            }
            if (!part.mesh.indices.empty()) out.parts.push_back(std::move(part));
        }
    }

    // --- One part per art layer ---
    for (const ArtLayer& al : art.layers) {
        ClipType op = ClipType::Union;
        Material material = Material::Copper;
        // The subject and clip for this layer's one boolean. Held as pointers
        // so copper can swap the subject per net while everything else about
        // the operation stays identical.
        const Paths64* subject = &al.art;
        const Paths64* clip = nullptr;
        // Blind/buried bores crossing this layer, subtracted after the main
        // boolean (they are not in art.drills, so boardArea leaves them be).
        Paths64 layerBores;

        switch (al.kind) {
            case LayerKind::Copper:
                // Clip to the board: trims copper at the profile (edge-connector
                // fingers are drawn overhanging and routed off during fab) and
                // punches the drills in one pass.
                material = Material::Copper;
                if (!boardArea.empty()) {
                    clip = &boardArea;
                    op = ClipType::Intersection;
                } else if (!art.drills.empty()) {
                    clip = &art.drills;
                    op = ClipType::Difference;
                }
                // A bore spans this foil if the foil's centre lies inside it
                // (its end foils' centres always do -- the bore reaches their
                // outer faces).
                {
                    const double c = al.z + al.thickness * 0.5;
                    for (const ResolvedBore& rb : bores) {
                        if (c > rb.z0 - 1e-6 && c < rb.z1 + 1e-6)
                            layerBores.push_back(*rb.path);
                    }
                }
                break;

            case LayerKind::Soldermask:
                // Film = OUTLINE minus openings. Note outline, not boardArea, and
                // no drill subtraction: mask tents ACROSS a hole, which is the
                // whole meaning of tenting. Subtracting drills here would make
                // tenting impossible and render every via as a punched dot.
                material = Material::Soldermask;
                if (art.outline.empty()) continue;
                subject = &art.outline;
                if (!al.art.empty()) {
                    clip = &al.art;
                    op = ClipType::Difference;
                }
                break;

            case LayerKind::Silkscreen:
                // Ink is clipped to the profile but NOT to the mask openings --
                // KiCad's (subtractmaskfromsilk no), which is what the fab does.
                material = Material::Silkscreen;
                if (art.outline.empty()) continue;
                clip = &art.outline;
                op = ClipType::Intersection;
                break;

            default:
                continue;
        }

        // Runs one subject through the layer's clip (and the blind/buried
        // bores) and extrudes the result, tagging every triangle produced
        // with `net`. Copper calls this once per net; everything else once
        // with net -1.
        Part part;
        part.material = material;
        part.name = al.name;
        part.maskOpenings = al.openings;

        const auto clipAndExtrude = [&](const Paths64& subj, int net) {
            if (subj.empty()) return;
            Clipper64 c;
            c.AddSubject(subj);
            if (clip) c.AddClip(*clip);

            PolyTree64 tree;
            if (layerBores.empty()) {
                c.Execute(op, FillRule::NonZero, tree);
            } else {
                Paths64 clipped;
                c.Execute(op, FillRule::NonZero, clipped);
                Clipper64 boring;
                boring.AddSubject(clipped);
                boring.AddClip(layerBores);
                boring.Execute(ClipType::Difference, FillRule::NonZero, tree);
            }

            std::vector<Shape> shapes;
            collectShapes(tree, shapes);
            const size_t before = part.mesh.indices.size();
            for (const Shape& shape : shapes)
                extrude(shape, al.z, al.z + al.thickness, part.mesh);
            const size_t added = (part.mesh.indices.size() - before) / 3;
            part.triNet.insert(part.triNet.end(), added, net);
        };

        if (al.kind == LayerKind::Copper && !al.netArt.empty()) {
            // Per net, so the triangles carry net identity. The union of the
            // parts equals the union of the whole -- different nets do not
            // overlap -- so the geometry is unchanged.
            for (const ArtLayer::NetRegion& r : al.netArt)
                clipAndExtrude(r.paths, r.net);
        } else {
            // Mask, silkscreen, and any copper with no net breakdown.
            clipAndExtrude(*subject, -1);
        }
        if (!part.mesh.indices.empty()) out.parts.push_back(std::move(part));
    }

    // --- Via barrels: each plated hole lined with a copper tube. Full-stack
    // drills collect into ONE part ("vias") spanning the whole copper stack,
    // pinned during the exploded peel. Blind/buried bores get one part per
    // span (also named "vias" -- one stackup toggle drives them all), each a
    // tube over just its own Z range, ranked by the renderers to travel with
    // the layers it spans. ---
    {
        // The barrel sits JUST INSIDE the drilled wall (radial `gap`), not on
        // it: the copper and substrate hole edges are exactly at the drill
        // radius, so a barrel of that radius shares a wall with every layer at
        // every via and z-fights into heavy speckle (bad on via-dense boards).
        // Insetting puts both the outer and inner barrel walls inside the bore,
        // where nothing else lives. A tube = inset(gap) minus inset(gap+wall);
        // holes too small for the void just render as a solid plug.
        const double gap = 0.03;   // clearance from the drilled wall (mm)
        const double wall = 0.15;  // plating thickness shown (mm)

        const auto makeBarrelPart = [&](const Paths64& holes, double z0,
                                        double z1, bool partial) {
            if (holes.empty() || z0 >= z1) return;

            // Castellated holes -- drills the routed board edge cuts through --
            // get NO barrel at all: only drills FULLY inside the outline keep
            // their plating tube. (Half-barrels looked wrong; the user wants
            // the milled half-holes bare.)
            Paths64 interior;
            interior.reserve(holes.size());
            for (const Path64& b : holes) {
                Clipper64 t;
                t.AddSubject(Paths64{b});
                t.AddClip(art.outline);
                Paths64 inside;
                t.Execute(ClipType::Intersection, FillRule::NonZero, inside);
                double insideArea = 0.0;
                for (const Path64& p : inside) insideArea += Area(p);
                if (insideArea >= std::abs(Area(b)) * 0.999)
                    interior.push_back(b);
            }
            if (interior.empty()) return;

            const auto inset = [&](double d) {
                ClipperOffset co;
                co.ArcTolerance(kScale * 0.001);
                co.AddPaths(interior, JoinType::Round, EndType::Polygon);
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
            part.partialBarrel = partial;
            for (const Shape& shape : shapes) extrude(shape, z0, z1, part.mesh);
            if (!part.mesh.indices.empty()) out.parts.push_back(std::move(part));
        };

        double botZ = 1e18, topZ = -1e18;
        for (const ArtLayer& al : art.layers) {
            if (al.kind != LayerKind::Copper) continue;
            botZ = std::min(botZ, al.z);
            topZ = std::max(topZ, al.z + al.thickness);
        }
        if (!art.barrels.empty()) makeBarrelPart(art.barrels, botZ, topZ, false);

        // Blind/buried barrels, one part per distinct span.
        std::map<std::string, std::pair<Paths64, std::pair<double, double>>>
            bySpan;
        for (const ResolvedBore& rb : bores) {
            auto& entry = bySpan[rb.spanKey];
            entry.first.push_back(*rb.path);
            entry.second = {rb.z0, rb.z1};
        }
        for (const auto& [key, entry] : bySpan) {
            makeBarrelPart(entry.first, entry.second.first, entry.second.second,
                           /*partial=*/true);
        }
    }

    // --- Measurement snap targets ---
    //
    // Fab-exact points the measure tool can lock onto: every drill/bore
    // centre (top and bottom face, so either viewing side snaps), every pad
    // centre, every outline vertex. Free clicks fall back to the board-top
    // plane, recorded here too.
    out.boardTopZ = art.thickness;
    out.copperFinish = art.copperFinish;
    out.nets = art.nets;
    out.netSegments = art.netSegments;
    {
        const auto centre = [](const Path64& p) {
            double sx = 0.0, sy = 0.0;
            for (const Point64& q : p) {
                sx += static_cast<double>(q.x);
                sy += static_cast<double>(q.y);
            }
            const double n = static_cast<double>(std::max<size_t>(p.size(), 1));
            return std::array<double, 2>{sx / n / kScale, sy / n / kScale};
        };
        const auto add = [&](double x, double y, double z, int net = -1) {
            SnapPoint sp;
            sp.pos[0] = static_cast<float>(x);
            sp.pos[1] = static_cast<float>(y);
            sp.pos[2] = static_cast<float>(z);
            sp.net = net;
            out.snapPoints.push_back(sp);
        };
        // Net-carrying points go FIRST: the snap search keeps the first of
        // equally-near candidates, so a via centre with a net beats the
        // identical netless point its drill also emits below.
        for (const LayerArt::NetPoint& np : art.netPoints) {
            add(np.pos[0], np.pos[1], np.pos[2], np.net);
        }
        for (const Path64& d : art.drills) {
            const auto c = centre(d);
            add(c[0], c[1], art.thickness);
            add(c[0], c[1], 0.0);
        }
        for (const ResolvedBore& rb : bores) {
            const auto c = centre(*rb.path);
            add(c[0], c[1], rb.z1);
            add(c[0], c[1], rb.z0);
        }
        for (const Path64& loop : art.outline) {
            for (const Point64& q : loop) {
                add(static_cast<double>(q.x) / kScale,
                    static_cast<double>(q.y) / kScale, art.thickness);
                add(static_cast<double>(q.x) / kScale,
                    static_cast<double>(q.y) / kScale, 0.0);
            }
        }

        // Outline bounding box, for the dimension callouts.
        for (const Path64& loop : art.outline) {
            for (const Point64& q : loop) {
                const double x = static_cast<double>(q.x) / kScale;
                const double y = static_cast<double>(q.y) / kScale;
                if (!out.outlineValid) {
                    out.outlineMin[0] = out.outlineMax[0] = x;
                    out.outlineMin[1] = out.outlineMax[1] = y;
                    out.outlineValid = true;
                } else {
                    out.outlineMin[0] = std::min(out.outlineMin[0], x);
                    out.outlineMin[1] = std::min(out.outlineMin[1], y);
                    out.outlineMax[0] = std::max(out.outlineMax[0], x);
                    out.outlineMax[1] = std::max(out.outlineMax[1], y);
                }
            }
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

// Dijkstra over one net's segment graph. Nodes are segment endpoints
// quantised to 1 um (KiCad shares exact coordinates between connected
// segments, and a via joins layers at one exact position, so 2D endpoint
// identity IS the connectivity). The two query points attach to every node
// within 1 mm (weighted by the gap) -- a track normally lands on the pad/via
// centre the measure tool snapped to, but not always exactly.
double netPathLength(const BoardMesh& mesh, int net, double ax, double ay,
                     double bx, double by) {
    struct Node {
        double x, y;
        std::vector<std::pair<int, double>> edges;  // (node, length)
    };
    std::vector<Node> nodes;
    std::map<std::pair<int64_t, int64_t>, int> byPos;
    const auto nodeAt = [&](double x, double y) {
        const std::pair<int64_t, int64_t> key{
            static_cast<int64_t>(std::llround(x * 1000.0)),
            static_cast<int64_t>(std::llround(y * 1000.0))};
        const auto it = byPos.find(key);
        if (it != byPos.end()) return it->second;
        const int id = static_cast<int>(nodes.size());
        byPos.emplace(key, id);
        nodes.push_back({x, y, {}});
        return id;
    };

    for (const LayerArt::NetSeg& s : mesh.netSegments) {
        if (s.net != net) continue;
        const int na = nodeAt(s.ax, s.ay);
        const int nb = nodeAt(s.bx, s.by);
        const double len = std::hypot(s.bx - s.ax, s.by - s.ay);
        nodes[na].edges.emplace_back(nb, len);
        nodes[nb].edges.emplace_back(na, len);
    }
    if (nodes.empty()) return -1.0;

    // Attach the query points. Source gets a virtual node; the target keeps a
    // per-node landing cost added at pop time.
    const int src = static_cast<int>(nodes.size());
    nodes.push_back({ax, ay, {}});
    constexpr double kAttach = 1.0;  // mm
    std::vector<double> landing(nodes.size(),
                                std::numeric_limits<double>::infinity());
    bool anyLanding = false;
    for (int i = 0; i < src; ++i) {
        const double da = std::hypot(nodes[i].x - ax, nodes[i].y - ay);
        if (da <= kAttach) nodes[src].edges.emplace_back(i, da);
        const double db = std::hypot(nodes[i].x - bx, nodes[i].y - by);
        if (db <= kAttach) {
            landing[i] = db;
            anyLanding = true;
        }
    }
    if (nodes[src].edges.empty() || !anyLanding) return -1.0;

    std::vector<double> dist(nodes.size(),
                             std::numeric_limits<double>::infinity());
    using QE = std::pair<double, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    dist[src] = 0.0;
    q.push({0.0, src});
    double best = std::numeric_limits<double>::infinity();
    while (!q.empty()) {
        const auto [d, n] = q.top();
        q.pop();
        if (d > dist[n]) continue;
        if (d >= best) break;  // everything further can only be worse
        if (n < static_cast<int>(landing.size()) &&
            std::isfinite(landing[n])) {
            best = std::min(best, d + landing[n]);
        }
        for (const auto& [m, w] : nodes[n].edges) {
            if (d + w < dist[m]) {
                dist[m] = d + w;
                q.push({d + w, m});
            }
        }
    }
    return std::isfinite(best) ? best : -1.0;
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
