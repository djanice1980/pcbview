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
#include <unordered_map>

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

// Nearest-distance queries against a set of 2D segments (the copper rim),
// through a uniform grid so a vertex asks only its own neighbourhood. The
// meniscus is a height field over distance-from-copper; every vertex of the
// fillet zone needs that distance, and the rim of a dense board is ~100k
// edges, which brute force would make quadratic.
struct SegmentGrid {
    struct Seg {
        double ax, ay, bx, by;
    };
    std::vector<Seg> segs;
    std::unordered_map<int64_t, std::vector<uint32_t>> cells;
    double cell = 0.1;

    static int64_t key(int64_t ix, int64_t iy) { return (ix << 32) ^ (iy & 0xffffffff); }

    explicit SegmentGrid(const Paths64& paths, double cellMm) : cell(cellMm) {
        for (const Path64& p : paths) {
            const size_t n = p.size();
            if (n < 2) continue;
            for (size_t i = 0; i < n; ++i) {
                const Point64& a = p[i];
                const Point64& b = p[(i + 1) % n];
                Seg s{toMm(a.x), toMm(a.y), toMm(b.x), toMm(b.y)};
                const uint32_t id = static_cast<uint32_t>(segs.size());
                segs.push_back(s);
                const int64_t x0 = static_cast<int64_t>(std::floor(std::min(s.ax, s.bx) / cell));
                const int64_t x1 = static_cast<int64_t>(std::floor(std::max(s.ax, s.bx) / cell));
                const int64_t y0 = static_cast<int64_t>(std::floor(std::min(s.ay, s.by) / cell));
                const int64_t y1 = static_cast<int64_t>(std::floor(std::max(s.ay, s.by) / cell));
                for (int64_t ix = x0; ix <= x1; ++ix)
                    for (int64_t iy = y0; iy <= y1; ++iy)
                        cells[key(ix, iy)].push_back(id);
            }
        }
    }

    // Distance from (x, y) to the nearest segment within one cell's reach,
    // and the unit direction from that nearest point to (x, y) -- the
    // "outward" the profile normal tilts along. Falls back to a straight-up
    // normal (dir = 0) when nothing is near, which the caller treats as the
    // flat foot.
    double nearest(double x, double y, double& dirx, double& diry) const {
        const int64_t cx = static_cast<int64_t>(std::floor(x / cell));
        const int64_t cy = static_cast<int64_t>(std::floor(y / cell));
        double best = 1e30, bnx = 0.0, bny = 0.0, bpx = 0.0, bpy = 0.0;
        for (int64_t ix = cx - 1; ix <= cx + 1; ++ix) {
            for (int64_t iy = cy - 1; iy <= cy + 1; ++iy) {
                const auto it = cells.find(key(ix, iy));
                if (it == cells.end()) continue;
                for (const uint32_t id : it->second) {
                    const Seg& s = segs[id];
                    const double ex = s.bx - s.ax, ey = s.by - s.ay;
                    const double l2 = ex * ex + ey * ey;
                    double t = l2 > 0.0 ? ((x - s.ax) * ex + (y - s.ay) * ey) / l2 : 0.0;
                    t = std::max(0.0, std::min(1.0, t));
                    const double px = s.ax + ex * t, py = s.ay + ey * t;
                    const double d2 = (x - px) * (x - px) + (y - py) * (y - py);
                    if (d2 < best) {
                        best = d2;
                        bpx = px;
                        bpy = py;
                        // Right-hand normal of a positively wound rim = outward.
                        const double l = std::sqrt(l2);
                        bnx = l > 0.0 ? ey / l : 0.0;
                        bny = l > 0.0 ? -ex / l : 0.0;
                    }
                }
            }
        }
        if (best >= 1e30) {
            dirx = diry = 0.0;
            return 1e30;
        }
        const double d = std::sqrt(best);
        if (d > 1e-7) {
            dirx = (x - bpx) / d;
            diry = (y - bpy) / d;
        } else {
            dirx = bnx;
            diry = bny;
        }
        return d;
    }
};

// A single-sided sheet over a shape -- top cap only, no walls, no bottom --
// with every vertex's height and normal supplied by the caller from its
// position. The mask's meniscus is one of these: a height field over the
// fillet zone, and ONE entering face to the path tracer's film lottery.
// `front` picks the winding so the face points away from the board.
// A solid slab over a shape whose TOP is a height field: every top vertex's
// height and normal come from `at`, the underside from `floorAt`, and the
// side walls close the slab along every boundary edge the caller does not
// veto. The mask's meniscus bands are these: `skipWall` vetoes the edges
// shared with the neighbouring band (the concentric rings), so from above a
// ray still meets exactly one entering face, while the film's CUT edges --
// the board outline at a castellation, a mask opening -- get a real wall and
// underside. Without them the band was a one-sided sheet, invisible from
// behind (exit faces never commit), which at a cut edge read as the mask
// turning into a clear membrane with a hairline silhouette.
// `front` picks the winding so faces point away from the board.
// `wallFloor(ax, ay, bx, by)` may give one wall a floor of its own (NaN keeps
// `floorAt`): the fillet's cliff above an exposed pad stands on the copper's
// top, not the laminate, because below the copper's top the copper's own
// flank is the wall and a second one there would z-fight it.
template <class At, class FloorAt, class SkipWall, class WallFloor>
void sheet(const Shape& shape, bool front, At&& at, FloorAt&& floorAt,
           SkipWall&& skipWall, Mesh& mesh, bool underside,
           WallFloor&& wallFloor) {
    std::vector<std::vector<std::array<double, 2>>> polygon;
    polygon.reserve(shape.rings.size());
    for (const Ring& ring : shape.rings) polygon.push_back(ring.pts);
    const std::vector<uint32_t> tris = mapbox::earcut<uint32_t>(polygon);
    if (tris.empty()) return;

    const float zs = front ? 1.0f : -1.0f;
    const auto tri = [&](uint32_t a, uint32_t b, uint32_t c) {
        if (!front) std::swap(b, c);
        mesh.indices.push_back(a);
        mesh.indices.push_back(b);
        mesh.indices.push_back(c);
    };

    // Top, per-vertex height field.
    const uint32_t top = static_cast<uint32_t>(mesh.vertices.size());
    std::vector<double> topZ;
    for (const auto& ring : polygon) {
        for (const auto& p : ring) {
            Vertex v{};
            v.position[0] = static_cast<float>(p[0]);
            v.position[1] = static_cast<float>(p[1]);
            double z = 0.0;
            float n[3] = {0.0f, 0.0f, 1.0f};
            at(p[0], p[1], z, n);
            topZ.push_back(z);
            v.position[2] = static_cast<float>(z);
            v.normal[0] = n[0];
            v.normal[1] = n[1];
            v.normal[2] = n[2];
            mesh.vertices.push_back(v);
        }
    }
    for (size_t i = 0; i + 2 < tris.size(); i += 3)
        tri(top + tris[i], top + tris[i + 1], top + tris[i + 2]);

    // Underside, facing into the board. The mask's cap over copper skips
    // it: opaque copper sits directly beneath.
    if (underside) {
        const uint32_t bot = static_cast<uint32_t>(mesh.vertices.size());
        for (const auto& ring : polygon) {
            for (const auto& p : ring) {
                Vertex v{};
                v.position[0] = static_cast<float>(p[0]);
                v.position[1] = static_cast<float>(p[1]);
                v.position[2] = static_cast<float>(floorAt(p[0], p[1]));
                v.normal[2] = -zs;
                mesh.vertices.push_back(v);
            }
        }
        for (size_t i = 0; i + 2 < tris.size(); i += 3)
            tri(bot + tris[i + 2], bot + tris[i + 1], bot + tris[i]);
    }

    // Walls along the boundary edges the caller does not veto. Outward is
    // decided per ring by its winding, as extrude() does.
    size_t offset = 0;
    for (const auto& ring : polygon) {
        const size_t n = ring.size();
        if (n < 3) {
            offset += n;
            continue;
        }
        double area = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const auto& a = ring[i];
            const auto& b = ring[(i + 1) % n];
            area += a[0] * b[1] - b[0] * a[1];
        }
        const double dir = (area >= 0.0) ? 1.0 : -1.0;
        for (size_t i = 0; i < n; ++i) {
            const size_t j = (i + 1) % n;
            const auto& a = ring[i];
            const auto& b = ring[j];
            if (skipWall(a[0], a[1], b[0], b[1])) continue;
            double ex = b[0] - a[0], ey = b[1] - a[1];
            const double len = std::sqrt(ex * ex + ey * ey);
            if (len < 1e-9) continue;
            // Outward normal of edge a->b for a CCW ring is its right-hand
            // side; a CW (hole) ring flips.
            const float nx = static_cast<float>(dir * ey / len);
            const float ny = static_cast<float>(-dir * ex / len);
            const uint32_t w = static_cast<uint32_t>(mesh.vertices.size());
            const double za = topZ[offset + i], zb = topZ[offset + j];
            double fa = floorAt(a[0], a[1]), fb = floorAt(b[0], b[1]);
            const double own = wallFloor(a[0], a[1], b[0], b[1]);
            if (!std::isnan(own)) fa = fb = own;
            const double P[4][3] = {{a[0], a[1], za}, {b[0], b[1], zb},
                                    {b[0], b[1], fb}, {a[0], a[1], fa}};
            for (const auto& p : P) {
                Vertex v{};
                v.position[0] = static_cast<float>(p[0]);
                v.position[1] = static_cast<float>(p[1]);
                v.position[2] = static_cast<float>(p[2]);
                v.normal[0] = nx;
                v.normal[1] = ny;
                mesh.vertices.push_back(v);
            }
            // CCW seen from outside (front): a-top, b-top, b-floor...
            if (dir > 0.0) {
                tri(w + 0, w + 3, w + 2);
                tri(w + 0, w + 2, w + 1);
            } else {
                tri(w + 0, w + 1, w + 2);
                tri(w + 0, w + 2, w + 3);
            }
        }
        offset += n;
    }
}

template <class At, class FloorAt, class SkipWall>
void sheet(const Shape& shape, bool front, At&& at, FloorAt&& floorAt,
           SkipWall&& skipWall, Mesh& mesh, bool underside = true) {
    sheet(shape, front, at, floorAt, skipWall, mesh, underside,
          [](double, double, double, double) {
              return std::numeric_limits<double>::quiet_NaN();
          });
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
            // A zone fill is pour copper: the net can be "joined through the
            // plane" where its track graph has no route.
            {
                const int ni = netOf(zone.net);
                if (ni >= 0) art.nets[ni].hasPlane = true;
            }
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
        const bool front = al.name.rfind("F.", 0) == 0;
        // Storage for the silk-minus-mask result, when that option is on. Must
        // outlive the switch, because `subject` points into it.
        Paths64 silkClipped;
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

            case LayerKind::Silkscreen: {
                // Ink is clipped to the profile. Whether it is ALSO clipped off
                // the mask openings is the caller's choice: KiCad's
                // (subtractmaskfromsilk) decides what is plotted and defaults
                // to off, so by default we render the ink exactly as the file
                // carries it -- including over pads.
                material = Material::Silkscreen;
                if (art.outline.empty()) continue;
                clip = &art.outline;
                op = ClipType::Intersection;
                if (opts.subtractMaskFromSilk) {
                    // Openings on THIS side only: front ink is not clipped by
                    // the back mask.
                    Paths64 openings;
                    for (const ArtLayer& other : art.layers) {
                        if (other.kind != LayerKind::Soldermask) continue;
                        if ((other.name.rfind("F.", 0) == 0) != front) continue;
                        openings.insert(openings.end(), other.art.begin(),
                                        other.art.end());
                    }
                    if (!openings.empty()) {
                        silkClipped = BooleanOp(ClipType::Difference,
                                                FillRule::NonZero,
                                                BooleanOp(ClipType::Intersection,
                                                          FillRule::NonZero,
                                                          *subject, art.outline),
                                                openings);
                        subject = &silkClipped;
                        // The subject is already fully clipped, so the
                        // downstream pass must UNION it, not intersect it:
                        // Clipper intersecting against no clip paths returns
                        // empty, which silently deletes the whole layer.
                        clip = nullptr;
                        op = ClipType::Union;
                    }
                }
                break;
            }

            default:
                continue;
        }

        // Conforming films (fab-truth gap #5). Real LPI soldermask -- and the
        // silkscreen ink printed on it -- DRAPES: over copper it rides the
        // foil, and between traces it drops onto the laminate. A film
        // extruded flat at one Z instead HOVERS a copper thickness above the
        // laminate wherever no copper supports it, with silk riding the
        // hover. Invisible at normal distance; at macro range with traced
        // shadows the 35 um slit under the film reads as a dark line and
        // every silk stroke casts a visibly displaced shadow. Split the film
        // by the outer foil's copper and extrude the unsupported region a
        // copper thickness closer to the core (a stepped drape -- the real
        // thing has a smooth shoulder, but the step puts both regions at
        // their true heights).
        const Paths64* support = nullptr;     // inflated 20 um (see below)
        const Paths64* supportRaw = nullptr;  // the copper itself
        double filmDrop = 0.0;
        if (al.kind == LayerKind::Soldermask ||
            al.kind == LayerKind::Silkscreen) {
            const ArtLayer* outer = nullptr;
            for (const ArtLayer& c : art.layers) {
                if (c.kind != LayerKind::Copper || c.art.empty()) continue;
                if (!outer || (front ? c.z > outer->z : c.z < outer->z))
                    outer = &c;
            }
            if (outer) {
                support = &outer->art;
                supportRaw = &outer->art;
                // Toward the core: down on the front, up on the back.
                filmDrop = front ? art.copperThickness : -art.copperThickness;
            }
        }
        // The split boundary is the copper edge INFLATED by a hair: splitting
        // exactly at the edge would put the dropped film's side walls in the
        // same plane as every trace's side walls, which z-fights into speckle
        // (the via barrels learnt this first -- see their inset). The 20 um
        // overhang doubles as the mask's shoulder over the trace edge.
        Paths64 supportInflated;
        if (support) {
            ClipperOffset co;
            co.ArcTolerance(kScale * 0.001);
            co.AddPaths(*support, JoinType::Round, EndType::Polygon);
            co.Execute(0.02 * kScale, supportInflated);
            support = &supportInflated;
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

            const size_t before = part.mesh.indices.size();
            if (!support) {
                std::vector<Shape> shapes;
                collectShapes(tree, shapes);
                for (const Shape& shape : shapes)
                    extrude(shape, al.z, al.z + al.thickness, part.mesh);
            } else {
                // Conformal films, built so that a ray from outside crosses
                // exactly ONE entering mask face anywhere on the board. That
                // is the invariant the path tracer's stochastic alpha needs:
                // every entering face of a translucent film is an independent
                // commit lottery (exits never commit), so two overlapping
                // solids tint twice -- darker bands along every shoulder.
                //
                // The film is PARTITIONED in 2D, by booleans, into:
                //   base   beyond the fillet foot, at the dropped level;
                //   cap    over copper to the rim (the copper edge itself), at
                //          the raised level -- for the mask a hair-thick
                //          sheet, since the copper beneath is opaque and walls
                //          would only be a second lottery;
                //   bands  the fillet zone between rim and foot, in kSteps
                //          concentric rings, each a single-sided SHEET whose
                //          vertices are lifted by the meniscus profile at
                //          their measured distance from the rim: concave
                //          quadratic, steep at the wall, flat at the foot,
                //          with normals from the profile's slope, so it shades
                //          as one continuous curve in every render mode.
                // Booleans clip the zone exactly to any outline -- a mask
                // opening, the board edge, a silkscreen stroke crossing a
                // trace obliquely -- and take corners through Clipper's own
                // round joins. (A skirt SWEPT outward from the rim was tried:
                // its rectangles stuck out past oblique silk edges as tabs,
                // and its corner fans shaded as dark wedges.)
                //
                // Silkscreen (opaque) uses the same partition at its own
                // thickness: ink is printed ON the cured mask and rides the
                // meniscus, so a stroke crossing a trace is one continuous
                // top. Reach ~1.5x the copper height, about what a 1 oz
                // fillet spans. PCBVIEW_MASK_FILLET_STEPS sets the profile
                // resolution.
                Paths64 film = PolyTreeToPaths64(tree);
                const double baseZ0 = al.z - filmDrop;
                const double reach = art.copperThickness * 1.5;
                const double rise = std::fabs(filmDrop);
                const bool frontSide = filmDrop > 0;
                const bool isMask = al.kind == LayerKind::Soldermask;
                static const int kSteps = [] {
                    const char* v = std::getenv("PCBVIEW_MASK_FILLET_STEPS");
                    const int n = v && v[0] ? std::atoi(v) : 6;
                    return std::max(1, std::min(n, 32));
                }();

                const auto inflate = [&](double d) {
                    ClipperOffset co;
                    co.ArcTolerance(kScale * 0.001);
                    co.AddPaths(*supportRaw, JoinType::Round, EndType::Polygon);
                    Paths64 r;
                    co.Execute(d * kScale, r);
                    return r;
                };
                const auto clipShapes = [&](const Paths64& subj,
                                            const Paths64& clip, ClipType ct) {
                    Clipper64 c;
                    c.AddSubject(subj);
                    c.AddClip(clip);
                    PolyTree64 t2;
                    c.Execute(ct, FillRule::NonZero, t2);
                    std::vector<Shape> shapes;
                    collectShapes(t2, shapes);
                    return shapes;
                };
                const auto clipPaths = [&](const Paths64& subj,
                                           const Paths64& clip, ClipType ct) {
                    Clipper64 c;
                    c.AddSubject(subj);
                    c.AddClip(clip);
                    Paths64 r;
                    c.Execute(ct, FillRule::NonZero, r);
                    return r;
                };

                // Close the hairline gaps between the mask and its copper: a
                // pad whose opening is drawn a couple of microns larger than
                // the pad (the copper and mask arts chord the same arc
                // differently) would otherwise get the full fillet lifted to
                // the opening's edge, then a 45 um cliff and a bare corridor
                // between it and the copper -- a sawn edge looks straight
                // down that corridor. Nothing on a real board is registered
                // to 5 um, so a gap that narrow is the mask meeting the
                // copper. Only film-to-copper gaps: the fill must be within
                // the snap of BOTH, so a real expansion is left open.
                if (isMask) {
                    constexpr double kSnap = 0.005;
                    ClipperOffset co;
                    co.ArcTolerance(kScale * 0.001);
                    co.AddPaths(film, JoinType::Round, EndType::Polygon);
                    Paths64 nearFilm;
                    co.Execute(kSnap * kScale, nearFilm);
                    const Paths64 nearCopper = inflate(kSnap);
                    Paths64 gap = clipPaths(nearFilm, nearCopper, ClipType::Intersection);
                    gap = clipPaths(gap, *supportRaw, ClipType::Difference);
                    // Both dilations overhang a sawn edge; the film does not.
                    gap = clipPaths(gap, art.outline, ClipType::Intersection);
                    film = clipPaths(film, gap, ClipType::Union);
                }

                // Concentric rings: rings[0] is the rim, rings[kSteps] the foot.
                // The rim is the copper edge ITSELF, so the meniscus starts at
                // the copper's top edge and sweeps straight down (a 20 um
                // shoulder at full height read as a squared-off ledge beside
                // every pad, edge-on; and a 2 um hair was worse -- where the
                // copper is clipped by the outline the hair's cut edge fell
                // inside the shared-ring tolerance, lost its wall, and showed
                // bare laminate). No mask wall stands on the rim any more, so
                // nothing is coplanar with the copper's flanks.
                std::vector<Paths64> rings(kSteps + 1);
                rings[0] = clipPaths(*supportRaw, Paths64{}, ClipType::Union);
                for (int k = 1; k <= kSteps; ++k)
                    rings[k] = inflate(reach * k / kSteps);
                const Paths64& rim = rings[0];
                const Paths64& foot = rings[kSteps];

                // Distance from the rim, for the meniscus profile.
                const SegmentGrid grid(rim, reach * 2.0 + 0.05);
                // An edge that lies ON a ring polygon is shared with the
                // neighbouring piece: no wall there, or a ray crossing it
                // would meet a second entering face. Tested against the ring's
                // own segments to half a micron -- the pieces are cut from the
                // same paths, so a shared edge coincides to the nanometre,
                // while a cut edge of the film can run parallel to a ring a
                // couple of microns away (a mask opening hugging its pad) and
                // must keep its wall: a distance-from-the-rim test with a
                // chord-sag tolerance vetoed those too, and the opening's
                // cliff went missing (bare laminate seen down the clearance).
                // Endpoints AND midpoint: the outline between two neighbouring
                // castellations runs from one pad's foot to the next pad's, so
                // both its ends sit on the foot polygon while the edge itself
                // is a cut edge that needs its wall.
                const auto edgeOn = [](const SegmentGrid& g, double ax, double ay,
                                       double bx, double by) {
                    constexpr double kOn = 0.0005;
                    double dx, dy;
                    return g.nearest(ax, ay, dx, dy) < kOn &&
                           g.nearest(bx, by, dx, dy) < kOn &&
                           g.nearest(0.5 * (ax + bx), 0.5 * (ay + by), dx, dy) < kOn;
                };
                std::vector<SegmentGrid> ringGrids;
                ringGrids.reserve(rings.size());
                for (const Paths64& r : rings) ringGrids.emplace_back(r, 0.05);
                const auto onRing = [&](int k, double ax, double ay, double bx,
                                        double by) {
                    return edgeOn(ringGrids[k], ax, ay, bx, by);
                };

                // Base: beyond the foot, full thickness, dropped. Silk is an
                // opaque slab; the mask's base is a sheet with walls only where
                // the film is cut -- along the foot it abuts the last band, and
                // a wall there was an internal face (a dark stripe along the
                // foot of every fillet, seen edge-on).
                if (!isMask) {
                    for (const Shape& shape : clipShapes(film, foot, ClipType::Difference))
                        extrude(shape, baseZ0, baseZ0 + al.thickness, part.mesh);
                } else {
                    const double baseTop = frontSide ? baseZ0 + al.thickness : baseZ0;
                    const auto baseAt = [&](double, double, double& z, float n[3]) {
                        z = baseTop;
                        n[0] = 0.0f;
                        n[1] = 0.0f;
                        n[2] = frontSide ? 1.0f : -1.0f;
                    };
                    const double baseFloor = frontSide ? baseZ0 : baseZ0 + al.thickness;
                    const auto baseFloorAt = [&](double, double) { return baseFloor; };
                    const auto onFoot = [&](double ax, double ay, double bx, double by) {
                        return onRing(kSteps, ax, ay, bx, by);
                    };
                    for (const Shape& shape : clipShapes(film, foot, ClipType::Difference))
                        sheet(shape, frontSide, baseAt, baseFloorAt, onFoot, part.mesh);
                }

                // Cap: over copper, at the raised level. Silk is an opaque
                // slab at its own thickness. The mask's cap is a top sheet
                // with walls down to the laminate and no underside: from
                // above a ray meets the sheet and then opaque copper (one
                // lottery), while the walls coat the copper's flanks and the
                // 20 um shoulder beside them where the film is CUT -- the
                // board outline, a mask opening. Along the rim ring itself
                // band 0 abuts the cap, and a wall there would be an internal
                // face: a ray through the fillet's cross-section at a
                // castellation crossed it into the cap for a second lottery
                // (a dark stripe beside the pad). A hair-thick cap with no
                // walls at all was tried before that: bare copper and bare
                // laminate between the pad and the fillet.
                {
                    const double top = frontSide ? baseZ0 + al.thickness + rise
                                                 : baseZ0 - rise;
                    if (!isMask) {
                        const double lo = frontSide ? top - al.thickness : top;
                        const double hi = frontSide ? top : top + al.thickness;
                        for (const Shape& shape :
                             clipShapes(film, rim, ClipType::Intersection))
                            extrude(shape, lo, hi, part.mesh);
                    } else {
                        const auto capAt = [&](double, double, double& z, float n[3]) {
                            z = top;
                            n[0] = 0.0f;
                            n[1] = 0.0f;
                            n[2] = frontSide ? 1.0f : -1.0f;
                        };
                        const double capFloor =
                            baseZ0 + (frontSide ? 0.0 : al.thickness);
                        const auto floorConst = [&](double, double) { return capFloor; };
                        const auto onRim = [&](double ax, double ay, double bx,
                                               double by) {
                            return onRing(0, ax, ay, bx, by);
                        };
                        for (const Shape& shape :
                             clipShapes(film, rim, ClipType::Intersection))
                            sheet(shape, frontSide, capAt, floorConst, onRim,
                                  part.mesh, /*underside=*/false);
                    }
                }

                // Bands: the zone, lifted by the profile at each vertex's
                // distance from the rim.
                const double zs = frontSide ? 1.0 : -1.0;
                const auto at = [&](double x, double y, double& z, float n[3]) {
                    double dx = 0.0, dy = 0.0;
                    double d = grid.nearest(x, y, dx, dy);
                    d = std::max(0.0, std::min(reach, d));
                    const double f = 1.0 - d / reach;
                    const double lift = rise * f * f;
                    // -d(lift)/dx: how steeply the surface descends outward.
                    const double sl = 2.0 * rise * f / reach;
                    z = frontSide ? baseZ0 + al.thickness + lift : baseZ0 - lift;
                    const double len = std::sqrt(sl * sl + 1.0);
                    n[0] = static_cast<float>(dx * sl / len);
                    n[1] = static_cast<float>(dy * sl / len);
                    n[2] = static_cast<float>(zs / len);
                };
                // The band's underside: for the mask, the laminate (the
                // dropped level); for silk, the mask's surface beneath it.
                const auto floorAt = [&](double x, double y) {
                    if (isMask) return baseZ0 + (frontSide ? 0.0 : al.thickness);
                    double z = 0.0;
                    float n[3];
                    at(x, y, z, n);
                    return z - zs * al.thickness;
                };
                // Copper the film does not cover (an exposed pad). Band 0's
                // inner edges there are not shared with a cap: the fillet's
                // cross-section above the copper's top is a real cliff and
                // needs a wall, or a ray skimming over the pad slips into the
                // band sideways, leaves through its free underside and lands
                // on untinted laminate. Below the copper's top the copper's own
                // flank closes the band, so the wall stands on the copper.
                const Paths64 exposed = clipPaths(rings[0], film, ClipType::Difference);
                const SegmentGrid exposedGrid(exposed, 0.05);
                const auto onExposed = [&](double ax, double ay, double bx, double by) {
                    return edgeOn(exposedGrid, ax, ay, bx, by);
                };
                const double copperFace =
                    frontSide ? baseZ0 + rise : baseZ0 + al.thickness - rise;
                for (int k = 0; k < kSteps; ++k) {
                    const Paths64 band =
                        clipPaths(rings[k + 1], rings[k], ClipType::Difference);
                    // An edge whose endpoints both sit on the same ring is
                    // shared with the neighbouring band: no wall there, or a
                    // ray from above would meet two entering faces. Anything
                    // else is a cut edge of the film and gets closed.
                    const auto cliff = [&](double ax, double ay, double bx, double by) {
                        return isMask && k == 0 && onRing(0, ax, ay, bx, by) &&
                               onExposed(ax, ay, bx, by);
                    };
                    const auto skipWall = [&](double ax, double ay, double bx,
                                              double by) {
                        if (cliff(ax, ay, bx, by)) return false;
                        return onRing(k, ax, ay, bx, by) || onRing(k + 1, ax, ay, bx, by);
                    };
                    const auto wallFloor = [&](double ax, double ay, double bx, double by) {
                        return cliff(ax, ay, bx, by)
                                   ? copperFace
                                   : std::numeric_limits<double>::quiet_NaN();
                    };
                    for (const Shape& shape : clipShapes(film, band, ClipType::Intersection))
                        sheet(shape, frontSide, at, floorAt, skipWall, part.mesh,
                              /*underside=*/true, wallFloor);
                }
            }
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
    out.netsArePseudo = art.netsArePseudo;
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

    struct SegRef {
        int na, nb;
        double ax, ay, bx, by, len;
    };
    std::vector<SegRef> segs;
    for (const LayerArt::NetSeg& s : mesh.netSegments) {
        if (s.net != net) continue;
        const int na = nodeAt(s.ax, s.ay);
        const int nb = nodeAt(s.bx, s.by);
        const double len = std::hypot(s.bx - s.ax, s.by - s.ay);
        nodes[na].edges.emplace_back(nb, len);
        nodes[nb].edges.emplace_back(na, len);
        segs.push_back({na, nb, s.ax, s.ay, s.bx, s.by, len});
    }
    if (nodes.empty()) return -1.0;

    // Attach each query point by PROJECTING it onto the nearest segment of
    // the net -- interiors included, not just endpoints. A measurement click
    // lands mid-trace far more often than on a junction, and the old
    // endpoint-only attach reported "no route" for any point over 1mm from a
    // segment END even when it sat dead-centre ON the copper. The projection
    // splits its segment virtually: the point connects to both segment ends
    // with the along-the-copper distances (plus the tiny perpendicular hop).
    struct Attach {
        int seg = -1;      // index into segs
        double t = 0.0;    // param along the segment, 0..1
        double perp = 0.0; // distance off the copper centreline
    };
    constexpr double kAttach = 1.0;  // mm
    const auto attach = [&](double px, double py) {
        Attach best;
        double bestD = kAttach;
        for (size_t i = 0; i < segs.size(); ++i) {
            const SegRef& s = segs[i];
            const double vx = s.bx - s.ax, vy = s.by - s.ay;
            const double ll = vx * vx + vy * vy;
            double t = ll > 1e-12
                           ? ((px - s.ax) * vx + (py - s.ay) * vy) / ll
                           : 0.0;
            t = std::clamp(t, 0.0, 1.0);
            const double cx = s.ax + vx * t, cy = s.ay + vy * t;
            const double d = std::hypot(px - cx, py - cy);
            if (d < bestD) {
                bestD = d;
                best = {static_cast<int>(i), t, d};
            }
        }
        return best;
    };
    const Attach atA = attach(ax, ay);
    const Attach atB = attach(bx, by);
    if (atA.seg < 0 || atB.seg < 0) return -1.0;

    // Both points on the SAME segment: the direct run along it, no junctions.
    double direct = std::numeric_limits<double>::infinity();
    if (atA.seg == atB.seg) {
        direct = atA.perp + atB.perp +
                 std::abs(atA.t - atB.t) * segs[atA.seg].len;
    }

    // Source: a virtual node joined to its segment's two ends by the
    // along-the-copper distances. Target: landing costs on ITS segment's two
    // ends, added at pop time.
    const int src = static_cast<int>(nodes.size());
    nodes.push_back({ax, ay, {}});
    {
        const SegRef& s = segs[atA.seg];
        nodes[src].edges.emplace_back(s.na, atA.perp + atA.t * s.len);
        nodes[src].edges.emplace_back(s.nb, atA.perp + (1.0 - atA.t) * s.len);
    }
    std::vector<double> landing(nodes.size(),
                                std::numeric_limits<double>::infinity());
    {
        const SegRef& s = segs[atB.seg];
        landing[s.na] = std::min(landing[s.na], atB.perp + atB.t * s.len);
        landing[s.nb] =
            std::min(landing[s.nb], atB.perp + (1.0 - atB.t) * s.len);
    }

    std::vector<double> dist(nodes.size(),
                             std::numeric_limits<double>::infinity());
    using QE = std::pair<double, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> q;
    dist[src] = 0.0;
    q.push({0.0, src});
    // Seed with the same-segment direct run (if any); the graph may still
    // find something shorter through a loop, so both compete.
    double best = direct;
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
