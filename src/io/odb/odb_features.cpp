#include "io/odb/odb_features.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <numbers>
#include <sstream>

#include "geom/layer_art.h"
#include "io/shapes.h"

using namespace Clipper2Lib;

namespace pcbview::odb {
namespace {

constexpr int kCircleSegs = 64;

Point64 toClip(double xMm, double yMm) {
    return Point64{geom::toInt(xMm), geom::toInt(yMm)};
}

Path64 circle(double cx, double cy, double r) {
    Path64 p;
    p.reserve(kCircleSegs);
    for (int i = 0; i < kCircleSegs; ++i) {
        const double t = 2.0 * std::numbers::pi * i / kCircleSegs;
        p.push_back(toClip(cx + r * std::cos(t), cy + r * std::sin(t)));
    }
    return p;
}

Path64 rectPath(double w, double h) {
    const double hw = w * 0.5, hh = h * 0.5;
    return Path64{toClip(-hw, -hh), toClip(hw, -hh), toClip(hw, hh),
                  toClip(-hw, hh)};
}

Paths64 stadium(double w, double h) {
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

// Append points along the arc from (sx,sy) to (ex,ey) about (cx,cy),
// EXCLUDING the start point. start == end means a full circle (ODB++ uses
// that convention for round cutouts).
void arcTo(Path64& path, double sx, double sy, double ex, double ey, double cx,
           double cy, bool cw) {
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

// Numeric parts split on 'x' ("900x160" -> {900,160}), plus the rect corner
// modifiers: "...xr40" radius, "...xc40" chamfer. A trailing corner list
// ("xr40x1.2" corner selectors) stops the scan harmlessly.
struct Dims {
    std::vector<double> v;
    double round = -1, chamfer = -1;
};
Dims splitDims(const std::string& s) {
    Dims d;
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == 'x') {
            ++i;
            continue;
        }
        double* slot = nullptr;
        if (s[i] == 'r') slot = &d.round;
        else if (s[i] == 'c') slot = &d.chamfer;
        if (slot) ++i;
        char* end = nullptr;
        const double val = std::strtod(s.c_str() + i, &end);
        if (end == s.c_str() + i) break;
        if (slot) *slot = val;
        else d.v.push_back(val);
        i = end - s.c_str();
    }
    return d;
}

Paths64 translated(Paths64 p, double xMm, double yMm, double rotDegCw,
                   bool mirror) {
    const double th = -rotDegCw * std::numbers::pi / 180.0;  // CW positive
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
        // Mirroring flips winding; keep polygons positive.
        if (mirror && Area(path) < 0)
            std::reverse(path.begin(), path.end());
    }
    return p;
}

}  // namespace

// ---- standard symbols ------------------------------------------------------

Paths64 standardSymbol(const std::string& name, bool metricDims) {
    // Standard symbol dimensions are MILS in imperial product models and
    // MICROMETERS in metric ones (spec: the M suffix -- and a metric file --
    // mean microns). KiCad writes "r600.0" for a 0.6mm drill in a UNITS=MM
    // file; misreading that as mils inflates every pad ~39x.
    const double unit = metricDims ? 0.001 : 0.0254;
    const auto starts = [&](const char* p) { return name.rfind(p, 0) == 0; };
    const auto dims = [&](size_t prefixLen) {
        Dims d = splitDims(name.substr(prefixLen));
        for (double& x : d.v) x *= unit;
        if (d.round > 0) d.round *= unit;
        if (d.chamfer > 0) d.chamfer *= unit;
        return d;
    };

    // Longest prefixes first, so "rect"/"oval" never match "r"'s branch.
    if (starts("rect")) {
        const Dims d = dims(4);
        if (d.v.size() < 2) return {};
        if (d.round > 0 && d.round < std::min(d.v[0], d.v[1]) * 0.5) {
            ClipperOffset co;
            co.ArcTolerance(geom::kScale * 0.001);
            co.AddPath(rectPath(d.v[0] - 2 * d.round, d.v[1] - 2 * d.round),
                       JoinType::Round, EndType::Polygon);
            Paths64 out;
            co.Execute(d.round * geom::kScale, out);
            return out;
        }
        // Chamfered corners rendered square: sub-width detail.
        return {rectPath(d.v[0], d.v[1])};
    }
    if (starts("oval")) {
        const Dims d = dims(4);
        return d.v.size() >= 2 ? stadium(d.v[0], d.v[1]) : Paths64{};
    }
    if (starts("oct")) {
        const Dims d = dims(3);
        if (d.v.size() < 3) return {};
        const double hw = d.v[0] * 0.5, hh = d.v[1] * 0.5, r = d.v[2];
        return {Path64{toClip(-hw + r, -hh), toClip(hw - r, -hh),
                       toClip(hw, -hh + r), toClip(hw, hh - r),
                       toClip(hw - r, hh), toClip(-hw + r, hh),
                       toClip(-hw, hh - r), toClip(-hw, -hh + r)}};
    }
    if (starts("donut_r")) {
        const Dims d = dims(7);
        if (d.v.size() < 2) return {};
        Path64 hole = circle(0, 0, d.v[1] * 0.5);
        std::reverse(hole.begin(), hole.end());
        return {circle(0, 0, d.v[0] * 0.5), std::move(hole)};
    }
    if (starts("di")) {
        const Dims d = dims(2);
        if (d.v.size() < 2) return {};
        const double hw = d.v[0] * 0.5, hh = d.v[1] * 0.5;
        return {Path64{toClip(0, -hh), toClip(hw, 0), toClip(0, hh),
                       toClip(-hw, 0)}};
    }
    if (starts("tri")) {
        const Dims d = dims(3);
        if (d.v.size() < 2) return {};
        const double hw = d.v[0] * 0.5, hh = d.v[1] * 0.5;
        return {Path64{toClip(-hw, -hh), toClip(hw, -hh), toClip(0, hh)}};
    }
    if (starts("el")) {
        const Dims d = dims(2);
        if (d.v.size() < 2) return {};
        Path64 p;
        p.reserve(kCircleSegs);
        for (int i = 0; i < kCircleSegs; ++i) {
            const double t = 2.0 * std::numbers::pi * i / kCircleSegs;
            p.push_back(toClip(d.v[0] * 0.5 * std::cos(t),
                               d.v[1] * 0.5 * std::sin(t)));
        }
        return {std::move(p)};
    }
    if (starts("r")) {
        const Dims d = dims(1);
        return d.v.empty() ? Paths64{} : Paths64{circle(0, 0, d.v[0] * 0.5)};
    }
    if (starts("s")) {
        const Dims d = dims(1);
        return d.v.empty() ? Paths64{} : Paths64{rectPath(d.v[0], d.v[0])};
    }
    return {};
}

// ---- realization -----------------------------------------------------------

const Paths64& FeatureRealizer::symbolPolys(int idx) {
    const auto it = polyCache_.find(idx);
    if (it != polyCache_.end()) return it->second;

    Paths64 polys;
    if (idx >= 0 && idx < static_cast<int>(ff_.symbols.size())) {
        const FeaturesFile::Symbol& sym = ff_.symbols[idx];
        polys = standardSymbol(sym.name, sym.metricDims);
        if (polys.empty() && user_ && depth_ < 4) {
            // A user symbol: its features file is the same language,
            // realized recursively at the origin.
            const std::string text = user_(sym.name);
            if (!text.empty()) {
                const FeaturesFile ff = parseFeatures(text, sym.metricDims);
                FeatureRealizer inner(ff, user_, warnings_, depth_ + 1);
                Paths64 acc;
                for (const Feature& f : ff.features) {
                    Paths64 fp = inner.realize(f);
                    if (fp.empty()) continue;
                    if (f.dark)
                        acc.insert(acc.end(), fp.begin(), fp.end());
                    else
                        acc = Difference(acc, fp, FillRule::NonZero);
                }
                polys = Union(acc, FillRule::NonZero);
            }
        }
        if (polys.empty() && !warned_[sym.name]) {
            warned_[sym.name] = true;
            warnings_.push_back("unsupported ODB++ symbol '" + sym.name +
                                "' -- features using it were skipped");
        }
    }
    return polyCache_.emplace(idx, std::move(polys)).first->second;
}

double FeatureRealizer::symbolWidthMm(int idx) {
    const auto it = widthCache_.find(idx);
    if (it != widthCache_.end()) return it->second;
    double width = 0.0;
    if (idx >= 0 && idx < static_cast<int>(ff_.symbols.size())) {
        const FeaturesFile::Symbol& sym = ff_.symbols[idx];
        // The common brushes carry their size in the name: r80, s10.5.
        if (!sym.name.empty() && (sym.name[0] == 'r' || sym.name[0] == 's') &&
            sym.name.rfind("rect", 0) != 0) {
            const Dims d = splitDims(sym.name.substr(1));
            if (!d.v.empty())
                width = d.v[0] * (sym.metricDims ? 0.001 : 0.0254);
        }
        if (width <= 0.0) {
            // Fall back to the realized geometry's larger extent.
            const Paths64& polys = symbolPolys(idx);
            if (!polys.empty()) {
                const Rect64 r = GetBounds(polys);
                width = std::max(r.Width(), r.Height()) / geom::kScale;
            }
        }
    }
    widthCache_[idx] = width;
    return width;
}

bool FeatureRealizer::symbolIsSquare(int idx) {
    if (idx < 0 || idx >= static_cast<int>(ff_.symbols.size())) return false;
    const std::string& n = ff_.symbols[idx].name;
    return !n.empty() && n[0] == 's' && n.rfind("s", 0) == 0 &&
           n.find('x') == std::string::npos && n.rfind("rect", 0) != 0;
}

Paths64 FeatureRealizer::realize(const Feature& f) {
    switch (f.kind) {
        case Feature::Kind::Text: {
            if (f.text.empty() || f.ye <= 0) return {};
            // font_width semantics vary by writer: a value under half the
            // char height is taken as the pen width; anything else falls
            // back to a typical 12% pen.
            const double thickness =
                (f.fontWidth > 1e-6 && f.fontWidth < f.ye * 0.5)
                    ? f.fontWidth
                    : f.ye * 0.12;
            return io::strokedText(f.text, f.xs, f.ys, f.ye, thickness,
                                   f.rotDeg, f.mirror);
        }
        case Feature::Kind::Surface:
            return f.surface;
        case Feature::Kind::Pad:
            return translated(symbolPolys(f.symbol), f.xs, f.ys, f.rotDeg,
                              f.mirror);
        case Feature::Kind::Line:
        case Feature::Kind::Arc: {
            const double width = symbolWidthMm(f.symbol);
            if (width <= 0.0) return {};
            Path64 spine;
            spine.push_back(toClip(f.xs, f.ys));
            if (f.kind == Feature::Kind::Arc)
                arcTo(spine, f.xs, f.ys, f.xe, f.ye, f.xc, f.yc, f.cw);
            else
                spine.push_back(toClip(f.xe, f.ye));
            ClipperOffset co;
            co.ArcTolerance(geom::kScale * 0.001);
            const bool square = symbolIsSquare(f.symbol);
            co.AddPath(spine, square ? JoinType::Miter : JoinType::Round,
                       square ? EndType::Square : EndType::Round);
            Paths64 out;
            co.Execute(width * 0.5 * geom::kScale, out);
            return out;
        }
    }
    return {};
}

// ---- features file ---------------------------------------------------------

FeaturesFile parseFeatures(const std::string& text, bool metricFile) {
    FeaturesFile out;
    double unit = metricFile ? 1.0 : 25.4;  // coordinate units -> mm

    std::istringstream in(text);
    std::string line;
    int surfaceIdx = -1;  // index of the open S record, -1 when none
    Path64 contour;       // open OB..OE polygon
    bool contourHole = false;
    Paths64 islands, holes;  // per-surface accumulation
    double lastX = 0, lastY = 0;

    const auto closeSurface = [&]() {
        if (surfaceIdx < 0) return;
        // Islands minus holes, resolved now so downstream code can treat a
        // surface as plain solid polygons.
        out.features[surfaceIdx].surface =
            holes.empty() ? Union(islands, FillRule::NonZero)
                          : Difference(islands, holes, FillRule::NonZero);
        islands.clear();
        holes.clear();
        surfaceIdx = -1;
    };

    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.pop_back();
        if (line.empty() || line[0] == '#') continue;

        // Attributes ride after ';' on any record -- geometry never needs
        // them.
        const size_t semi = line.find(';');
        if (semi != std::string::npos) line.resize(semi);

        if (line.rfind("UNITS=", 0) == 0) {
            metricFile = line.find("MM") != std::string::npos;
            unit = metricFile ? 1.0 : 25.4;
            continue;
        }
        if (line[0] == '$') {  // symbol table: $<n> <name> [M]
            std::istringstream ls(line);
            std::string idx, sym, m;
            ls >> idx >> sym >> m;
            FeaturesFile::Symbol s;
            s.name = sym;
            // Micron dimensions: flagged per symbol with M, or implied for
            // the whole table by a metric file (what KiCad writes).
            s.metricDims = (m == "M") || metricFile;
            const int n = std::atoi(idx.c_str() + 1);
            if (n >= 0) {
                if (n >= static_cast<int>(out.symbols.size()))
                    out.symbols.resize(n + 1);
                out.symbols[n] = std::move(s);
            }
            continue;
        }

        std::istringstream ls(line);
        std::string tag;
        ls >> tag;

        if (tag == "OB" && surfaceIdx >= 0) {
            std::string ih;
            double x = 0, y = 0;
            ls >> x >> y >> ih;
            contour.clear();
            contour.push_back(toClip(x * unit, y * unit));
            contourHole = (ih == "H");
            lastX = x * unit;
            lastY = y * unit;
        } else if (tag == "OS" && surfaceIdx >= 0) {
            double x = 0, y = 0;
            ls >> x >> y;
            contour.push_back(toClip(x * unit, y * unit));
            lastX = x * unit;
            lastY = y * unit;
        } else if (tag == "OC" && surfaceIdx >= 0) {
            double xe = 0, ye = 0, xc = 0, yc = 0;
            std::string cw;
            ls >> xe >> ye >> xc >> yc >> cw;
            arcTo(contour, lastX, lastY, xe * unit, ye * unit, xc * unit,
                  yc * unit, cw == "Y");
            lastX = xe * unit;
            lastY = ye * unit;
        } else if (tag == "OE" && surfaceIdx >= 0) {
            if (contour.size() >= 3) {
                // Winding no longer matters: islands and holes are separated.
                if (Area(contour) < 0)
                    std::reverse(contour.begin(), contour.end());
                (contourHole ? holes : islands).push_back(contour);
            }
            contour.clear();
        } else if (tag == "SE") {
            closeSurface();
        } else if (tag == "L") {
            closeSurface();
            Feature f;
            f.kind = Feature::Kind::Line;
            std::string pol;
            ls >> f.xs >> f.ys >> f.xe >> f.ye >> f.symbol >> pol;
            f.xs *= unit;
            f.ys *= unit;
            f.xe *= unit;
            f.ye *= unit;
            f.dark = (pol != "N");
            out.features.push_back(std::move(f));
        } else if (tag == "P") {
            closeSurface();
            Feature f;
            f.kind = Feature::Kind::Pad;
            std::string pol;
            int dcode = 0, orient = 0;
            ls >> f.xs >> f.ys >> f.symbol >> pol >> dcode >> orient;
            f.xs *= unit;
            f.ys *= unit;
            f.dark = (pol != "N");
            if (orient >= 8) {  // 8/9: arbitrary angle in the next field
                ls >> f.rotDeg;
                f.mirror = (orient == 9);
            } else {
                f.rotDeg = 90.0 * (orient & 3);
                f.mirror = orient >= 4;
            }
            out.features.push_back(std::move(f));
        } else if (tag == "A") {
            closeSurface();
            Feature f;
            f.kind = Feature::Kind::Arc;
            std::string pol;
            ls >> f.xs >> f.ys >> f.xe >> f.ye >> f.xc >> f.yc >> f.symbol >>
                pol;
            // dcode then the cw flag; tolerate the dcode being absent.
            std::string t1, t2;
            ls >> t1 >> t2;
            const std::string cw = t2.empty() ? t1 : t2;
            f.xs *= unit;
            f.ys *= unit;
            f.xe *= unit;
            f.ye *= unit;
            f.xc *= unit;
            f.yc *= unit;
            f.dark = (pol != "N");
            f.cw = (cw == "Y");
            out.features.push_back(std::move(f));
        } else if (tag == "S") {
            closeSurface();
            Feature f;
            f.kind = Feature::Kind::Surface;
            std::string pol;
            ls >> pol;
            f.dark = (pol != "N");
            out.features.push_back(std::move(f));
            surfaceIdx = static_cast<int>(out.features.size()) - 1;
        } else if (tag == "T") {
            closeSurface();
            // T <x> <y> <font> <polarity> <orient> <xsize> <ysize>
            //   <font_width> <text...> [version]
            Feature f;
            f.kind = Feature::Kind::Text;
            std::string font, pol;
            int orient = 0;
            double xs = 0, ys = 0, w = 0, h = 0;
            ls >> xs >> ys >> font >> pol >> orient >> w >> h >> f.fontWidth;
            f.xs = xs * unit;
            f.ys = ys * unit;
            f.xe = w * unit;
            f.ye = h * unit;
            f.fontWidth *= unit;
            f.dark = (pol != "N");
            if (orient >= 8) {
                ls >> f.rotDeg;  // CCW, native
                f.mirror = (orient == 9);
            } else {
                f.rotDeg = 90.0 * (orient & 3);
                f.mirror = orient >= 4;
            }
            std::string rest;
            std::getline(ls, rest);
            // Trim, then drop a trailing lone version token ("1").
            size_t a = rest.find_first_not_of(" \t");
            size_t b = rest.find_last_not_of(" \t");
            if (a != std::string::npos) rest = rest.substr(a, b - a + 1);
            else rest.clear();
            if (rest.size() > 2 &&
                rest.compare(rest.size() - 2, 2, " 1") == 0)
                rest.resize(rest.size() - 2);
            f.text = rest;
            if (f.text.empty()) ++out.textCount;
            out.features.push_back(std::move(f));
        } else if (tag == "B") {
            closeSurface();
            Feature f;
            f.kind = Feature::Kind::Text;  // barcode: placeholder, unrendered
            out.features.push_back(std::move(f));
            ++out.textCount;
        }
    }
    closeSurface();
    return out;
}

}  // namespace pcbview::odb
