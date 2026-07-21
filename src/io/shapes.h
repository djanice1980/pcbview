#pragma once

// Shape builders shared by the ODB++ and IPC-2581 importers: pad primitives
// at the origin, arc tessellation, and the place-with-rotation transform.
// All take mm and produce Clipper integer paths via geom::kScale.

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

#include "geom/layer_art.h"
#include "text/stroke_text.h"

namespace pcbview::io {

inline constexpr int kCircleSegs = 64;

inline Clipper2Lib::Point64 toClip(double xMm, double yMm) {
    return Clipper2Lib::Point64{geom::toInt(xMm), geom::toInt(yMm)};
}

inline Clipper2Lib::Path64 circlePath(double cx, double cy, double r) {
    Clipper2Lib::Path64 p;
    p.reserve(kCircleSegs);
    for (int i = 0; i < kCircleSegs; ++i) {
        const double t = 2.0 * std::numbers::pi * i / kCircleSegs;
        p.push_back(toClip(cx + r * std::cos(t), cy + r * std::sin(t)));
    }
    return p;
}

inline Clipper2Lib::Path64 rectPath(double w, double h) {
    const double hw = w * 0.5, hh = h * 0.5;
    return Clipper2Lib::Path64{toClip(-hw, -hh), toClip(hw, -hh),
                               toClip(hw, hh), toClip(-hw, hh)};
}

// Ellipse, full width/height.
inline Clipper2Lib::Path64 ellipsePath(double w, double h) {
    Clipper2Lib::Path64 p;
    p.reserve(kCircleSegs);
    for (int i = 0; i < kCircleSegs; ++i) {
        const double t = 2.0 * std::numbers::pi * i / kCircleSegs;
        p.push_back(toClip(w * 0.5 * std::cos(t), h * 0.5 * std::sin(t)));
    }
    return p;
}

// Stadium: a segment of length |w-h| inflated to the minor radius.
inline Clipper2Lib::Paths64 stadium(double w, double h) {
    using namespace Clipper2Lib;
    const double r = std::min(w, h) * 0.5;
    const double span = std::max(w, h) * 0.5 - r;
    Path64 spine = (w >= h) ? Path64{toClip(-span, 0), toClip(span, 0)}
                            : Path64{toClip(0, -span), toClip(0, span)};
    ClipperOffset co;
    co.ArcTolerance(geom::kScale * 0.001);
    co.AddPath(spine, JoinType::Round, EndType::Round);
    Paths64 out;
    co.Execute(r * geom::kScale, out);
    return out;
}

// Rectangle with rounded corners (radius sanity-checked by the caller).
inline Clipper2Lib::Paths64 roundedRect(double w, double h, double rad) {
    using namespace Clipper2Lib;
    ClipperOffset co;
    co.ArcTolerance(geom::kScale * 0.001);
    co.AddPath(rectPath(w - 2 * rad, h - 2 * rad), JoinType::Round,
               EndType::Polygon);
    Paths64 out;
    co.Execute(rad * geom::kScale, out);
    return out;
}

// Append points along the arc from (sx,sy) to (ex,ey) about (cx,cy),
// EXCLUDING the start point. start == end means a full circle.
inline void arcAppend(Clipper2Lib::Path64& path, double sx, double sy,
                      double ex, double ey, double cx, double cy, bool cw) {
    const double r0 = std::hypot(sx - cx, sy - cy);
    const double r1 = std::hypot(ex - cx, ey - cy);
    const double r = (r0 + r1) * 0.5;
    if (r < 1e-9) {
        path.push_back(toClip(ex, ey));
        return;
    }
    const double a0 = std::atan2(sy - cy, sx - cx);
    const double a1 = std::atan2(ey - cy, ex - cx);
    double sweep = a1 - a0;
    if (cw) {
        while (sweep >= -1e-12) sweep -= 2.0 * std::numbers::pi;
    } else {
        while (sweep <= 1e-12) sweep += 2.0 * std::numbers::pi;
    }
    const int segs = std::max(
        4, static_cast<int>(std::ceil(
               std::abs(sweep) / (2 * std::numbers::pi) * kCircleSegs)));
    for (int i = 1; i <= segs; ++i) {
        const double t = a0 + sweep * i / segs;
        path.push_back(toClip(cx + r * std::cos(t), cy + r * std::sin(t)));
    }
}

// Mirror (x negated) first, then rotate CLOCKWISE by rotDegCw, then
// translate. Winding is re-normalised after a mirror.
inline Clipper2Lib::Paths64 placed(Clipper2Lib::Paths64 p, double xMm,
                                   double yMm, double rotDegCw, bool mirror) {
    using namespace Clipper2Lib;
    const double th = -rotDegCw * std::numbers::pi / 180.0;
    const double c = std::cos(th), s = std::sin(th);
    const int64_t tx = geom::toInt(xMm), ty = geom::toInt(yMm);
    for (Path64& path : p) {
        for (Point64& pt : path) {
            double x = static_cast<double>(pt.x);
            double y = static_cast<double>(pt.y);
            if (mirror) x = -x;
            const double rx = x * c - y * s;
            const double ry = x * s + y * c;
            pt.x = static_cast<int64_t>(std::llround(rx)) + tx;
            pt.y = static_cast<int64_t>(std::llround(ry)) + ty;
        }
        if (mirror && Area(path) < 0)
            std::reverse(path.begin(), path.end());
    }
    return p;
}

// Text as stroked polygons via the Newstroke engine, in the importers'
// Y-UP space (text::layout emits KiCad's Y-down; the flip happens here).
// The anchor is the string's LEFT-BOTTOM corner (what ODB++ and Altium
// use) unless `centred`; rotation is degrees CCW; mirror flips about the
// anchor for bottom-side text.
inline Clipper2Lib::Paths64 strokedText(const std::string& utf8, double xMm,
                                        double yMm, double sizeMm,
                                        double thicknessMm, double rotDegCcw,
                                        bool mirror, bool centred = false) {
    using namespace Clipper2Lib;
    if (utf8.empty() || sizeMm <= 1e-6 || thicknessMm <= 1e-6) return {};
    text::TextStyle style;
    style.size = {sizeMm, sizeMm};
    style.thickness = thicknessMm;
    const auto lines = text::layout(utf8, {0.0, 0.0}, style);
    double dx = 0, dy = 0;
    if (!centred) {
        dx = text::measure(utf8, style) * 0.5;
        dy = sizeMm * 0.5;
    }
    const double th = rotDegCcw * std::numbers::pi / 180.0;
    const double c = std::cos(th), s = std::sin(th);
    ClipperOffset co;
    co.ArcTolerance(geom::kScale * 0.001);
    bool any = false;
    for (const text::Polyline& line : lines) {
        Path64 spine;
        for (const Vec2& p : line) {
            double lx = p.x + dx;
            double ly = -p.y + dy;  // Y-down layout -> Y-up board
            if (mirror) lx = -lx;
            const double rx = lx * c - ly * s;
            const double ry = lx * s + ly * c;
            spine.push_back(toClip(xMm + rx, yMm + ry));
        }
        if (spine.size() < 2) continue;
        co.AddPath(spine, JoinType::Round, EndType::Round);
        any = true;
    }
    if (!any) return {};
    Paths64 out;
    co.Execute(thicknessMm * 0.5 * geom::kScale, out);
    return out;
}

// Sequential dark/clear compositor, batching consecutive darks into one
// union: per-feature booleans on a 100k-feature layer would take minutes.
struct DarkClearAcc {
    Clipper2Lib::Paths64 acc;
    Clipper2Lib::Paths64 pending;
    bool sawClear = false;

    void dark(const Clipper2Lib::Paths64& p) {
        pending.insert(pending.end(), p.begin(), p.end());
    }
    void flush() {
        using namespace Clipper2Lib;
        if (pending.empty()) return;
        if (!acc.empty()) pending.insert(pending.end(), acc.begin(), acc.end());
        acc = Union(pending, FillRule::NonZero);
        pending.clear();
    }
    void clear(const Clipper2Lib::Paths64& p) {
        using namespace Clipper2Lib;
        if (p.empty()) return;
        flush();
        acc = Difference(acc, p, FillRule::NonZero);
        sawClear = true;
    }
    Clipper2Lib::Paths64 take() {
        flush();
        return std::move(acc);
    }
};

}  // namespace pcbview::io
