#include "io/gerber/gerber_parser.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <numbers>
#include <sstream>
#include <string_view>

#include "geom/layer_art.h"  // kScale, toInt

namespace pcbview::gerber {
namespace {

using namespace Clipper2Lib;

constexpr int kArcSegments = 48;   // per full circle
constexpr int kFlashSegments = 48;

// Gerber Y already matches our world Y (both up-positive), unlike KiCad. So
// there is NO flip here -- this is the whole reason gerber has its own toClipper.
Point64 toClip(double x, double y) {
    return Point64{geom::toInt(x), geom::toInt(y)};
}

// ---- primitive shape builders (mm) --------------------------------------

Path64 circle(double cx, double cy, double r, int segs = kFlashSegments) {
    Path64 p;
    p.reserve(segs);
    for (int i = 0; i < segs; ++i) {
        const double t = 2.0 * std::numbers::pi * i / segs;
        p.push_back(toClip(cx + r * std::cos(t), cy + r * std::sin(t)));
    }
    return p;
}

Path64 rect(double cx, double cy, double w, double h) {
    const double hw = w * 0.5, hh = h * 0.5;
    return Path64{toClip(cx - hw, cy - hh), toClip(cx + hw, cy - hh),
                  toClip(cx + hw, cy + hh), toClip(cx - hw, cy + hh)};
}

// Stadium: a segment of length (major-minor) inflated to the minor radius.
Paths64 obround(double cx, double cy, double w, double h) {
    const double r = std::min(w, h) * 0.5;
    const double span = std::max(w, h) * 0.5 - r;
    Path64 spine = (w >= h)
                       ? Path64{toClip(cx - span, cy), toClip(cx + span, cy)}
                       : Path64{toClip(cx, cy - span), toClip(cx, cy + span)};
    ClipperOffset co;
    co.ArcTolerance(geom::kScale * 0.001);
    co.AddPath(spine, JoinType::Round, EndType::Round);
    Paths64 out;
    co.Execute(r * geom::kScale, out);
    return out;
}

Path64 regularPolygon(double cx, double cy, double diameter, int verts,
                      double rotDeg) {
    Path64 p;
    const double r = diameter * 0.5;
    const double rot = rotDeg * std::numbers::pi / 180.0;
    for (int i = 0; i < verts; ++i) {
        const double t = rot + 2.0 * std::numbers::pi * i / verts;
        p.push_back(toClip(cx + r * std::cos(t), cy + r * std::sin(t)));
    }
    return p;
}

// ---- aperture macro expression evaluator --------------------------------
//
// A tiny recursive-descent evaluator over + - x / and parens, with $n variable
// substitution. The RoundRect macro only needs `$1+$1`, but the full grammar is
// cheap and guards against surprises in other tools' macros.
class Expr {
public:
    Expr(std::string_view s, const std::vector<double>& vars)
        : s_(s), vars_(vars) {}

    double eval() {
        double v = parseSum();
        return v;
    }

private:
    std::string_view s_;
    const std::vector<double>& vars_;
    size_t i_ = 0;

    void skip() { while (i_ < s_.size() && s_[i_] == ' ') ++i_; }

    double parseSum() {
        double v = parseProduct();
        for (;;) {
            skip();
            if (i_ >= s_.size()) break;
            const char c = s_[i_];
            if (c == '+') { ++i_; v += parseProduct(); }
            else if (c == '-') { ++i_; v -= parseProduct(); }
            else break;
        }
        return v;
    }
    double parseProduct() {
        double v = parseAtom();
        for (;;) {
            skip();
            if (i_ >= s_.size()) break;
            const char c = s_[i_];
            // Gerber uses 'x' or 'X' for multiply, '/' for divide.
            if (c == 'x' || c == 'X') { ++i_; v *= parseAtom(); }
            else if (c == '/') { ++i_; v /= parseAtom(); }
            else break;
        }
        return v;
    }
    double parseAtom() {
        skip();
        if (i_ >= s_.size()) return 0.0;
        if (s_[i_] == '(') {
            ++i_;
            double v = parseSum();
            skip();
            if (i_ < s_.size() && s_[i_] == ')') ++i_;
            return v;
        }
        if (s_[i_] == '-') { ++i_; return -parseAtom(); }
        if (s_[i_] == '+') { ++i_; return parseAtom(); }
        if (s_[i_] == '$') {
            ++i_;
            int idx = 0;
            while (i_ < s_.size() && std::isdigit(s_[i_])) {
                idx = idx * 10 + (s_[i_] - '0');
                ++i_;
            }
            // $1 is the first parameter.
            return (idx >= 1 && idx <= static_cast<int>(vars_.size()))
                       ? vars_[idx - 1]
                       : 0.0;
        }
        // number
        const size_t start = i_;
        while (i_ < s_.size() &&
               (std::isdigit(s_[i_]) || s_[i_] == '.' || s_[i_] == 'e' ||
                s_[i_] == 'E')) {
            ++i_;
        }
        return std::atof(std::string(s_.substr(start, i_ - start)).c_str());
    }
};

struct Macro {
    // Each primitive is one raw line, e.g. "1,1,$1+$1,$2,$3". Evaluated per
    // instance against the aperture's parameters.
    std::vector<std::string> primitives;
};

// Evaluate a macro into dark polygons at (ox, oy), given instance params.
Paths64 evalMacro(const Macro& m, const std::vector<double>& params, double ox,
                  double oy, std::vector<std::string>& warnings) {
    Paths64 image;
    bool pendingDark = true;
    Paths64 pending;

    const auto flush = [&] {
        if (pending.empty()) return;
        Clipper64 c;
        c.AddSubject(image);
        c.AddClip(pending);
        image = BooleanOp(pendingDark ? ClipType::Union : ClipType::Difference,
                          FillRule::NonZero, image, pending);
        pending.clear();
    };
    const auto add = [&](Paths64 shape, bool dark) {
        if (dark != pendingDark && !pending.empty()) flush();
        pendingDark = dark;
        for (Path64& p : shape) pending.push_back(std::move(p));
    };

    for (const std::string& prim : m.primitives) {
        // Comment primitive.
        if (!prim.empty() && prim[0] == '0') continue;

        std::vector<double> a;
        std::stringstream ss(prim);
        std::string tok;
        while (std::getline(ss, tok, ',')) {
            a.push_back(Expr(tok, params).eval());
        }
        if (a.empty()) continue;

        const int code = static_cast<int>(a[0]);
        switch (code) {
            case 1: {  // circle: code, exposure, diameter, x, y[, rot]
                if (a.size() < 5) break;
                const bool dark = a[1] != 0.0;
                add({circle(ox + a[3], oy + a[4], a[2] * 0.5)}, dark);
                break;
            }
            case 20: {  // vector line: code, exp, width, x1,y1,x2,y2[, rot]
                if (a.size() < 7) break;
                const bool dark = a[1] != 0.0;
                Path64 spine{toClip(ox + a[3], oy + a[4]),
                             toClip(ox + a[5], oy + a[6])};
                ClipperOffset co;
                co.ArcTolerance(geom::kScale * 0.001);
                co.AddPath(spine, JoinType::Round, EndType::Butt);
                Paths64 out;
                co.Execute(a[2] * 0.5 * geom::kScale, out);
                add(std::move(out), dark);
                break;
            }
            case 21: {  // center line rect: code, exp, w, h, x, y[, rot]
                if (a.size() < 6) break;
                const bool dark = a[1] != 0.0;
                add({rect(ox + a[4], oy + a[5], a[2], a[3])}, dark);
                break;
            }
            case 4: {  // outline: code, exp, N, x0,y0,...,xN,yN[, rot]
                if (a.size() < 5) break;
                const bool dark = a[1] != 0.0;
                const int n = static_cast<int>(a[2]);
                Path64 poly;
                for (int k = 0; k <= n; ++k) {
                    const size_t xi = 3 + 2 * k, yi = 4 + 2 * k;
                    if (yi >= a.size()) break;
                    poly.push_back(toClip(ox + a[xi], oy + a[yi]));
                }
                if (poly.size() >= 3) add({std::move(poly)}, dark);
                break;
            }
            case 5: {  // regular polygon: code, exp, verts, x, y, dia[, rot]
                if (a.size() < 6) break;
                const bool dark = a[1] != 0.0;
                add({regularPolygon(ox + a[3], oy + a[4], a[5],
                                    static_cast<int>(a[2]),
                                    a.size() > 6 ? a[6] : 0.0)},
                    dark);
                break;
            }
            default:
                warnings.push_back("aperture macro primitive " +
                                   std::to_string(code) + " not implemented");
                break;
        }
    }
    flush();
    return image;
}

// ---- aperture ------------------------------------------------------------

struct Aperture {
    enum class Type { Circle, Rect, Obround, Polygon, Macro } type = Type::Circle;
    std::vector<double> params;  // template params, or macro instance params
    std::string macroName;
    double diameter = 0.0;  // circle: for stroking draws
};

// ---- file function -------------------------------------------------------

FileFunction parseFileFunction(std::string_view v) {
    FileFunction f;
    // e.g. "Copper,L1,Top" / "Soldermask,Top" / "Legend,Bot" / "Profile,NP"
    std::vector<std::string> parts;
    std::stringstream ss{std::string(v)};
    std::string p;
    while (std::getline(ss, p, ',')) parts.push_back(p);
    if (parts.empty()) return f;

    // Case-insensitive: KiCad itself is inconsistent -- a file's X2 attribute
    // says "Soldermask" while the SAME board's .gbrjob says "SolderMask".
    std::string k = parts[0];
    std::transform(k.begin(), k.end(), k.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (k == "copper") f.kind = FileFunction::Kind::Copper;
    else if (k == "soldermask") f.kind = FileFunction::Kind::Soldermask;
    else if (k == "legend") f.kind = FileFunction::Kind::Silkscreen;
    // "Paste" in a file's own X2 attribute, "SolderPaste" in a .gbrjob
    // manifest -- the same inconsistency as Soldermask/SolderMask, which only
    // passes because case-folding happens to collapse it. The manifest wins
    // when present, so a package with a .gbrjob reported its paste layers as
    // unidentified until both spellings were accepted.
    else if (k == "paste" || k == "solderpaste")
        f.kind = FileFunction::Kind::Paste;
    else if (k == "profile") f.kind = FileFunction::Kind::Profile;
    // Documentation: drawings ABOUT the board rather than layers OF it. All
    // of these are real X2 FileFunction values, so a file carrying one has
    // told us exactly what it is -- reporting it as unidentified would be
    // simply untrue.
    else if (k == "drillmap" || k == "fabricationdrawing" ||
             k == "vcutmap" || k == "assemblydrawing" ||
             k == "arraydrawing" || k == "otherdrawing")
        f.kind = FileFunction::Kind::Documentation;

    for (const std::string& part : parts) {
        if (part == "Top") f.top = true;
        else if (part == "Bot") f.bottom = true;
        else if (part.size() >= 2 && part[0] == 'L' && std::isdigit(part[1])) {
            f.copperIndex = std::atoi(part.c_str() + 1);
        }
    }
    return f;
}

// ---- the parser ----------------------------------------------------------

class Parser {
public:
    explicit Parser(const std::string& name) : name_(name) {}

    GerberImage run(const std::string& text) {
        // Split into commands. Extended commands are delimited by '%'; ordinary
        // ones end with '*'. This tokeniser walks the whole file once.
        size_t i = 0;
        while (i < text.size()) {
            const char c = text[i];
            if (c == '%') {
                const size_t end = text.find('%', i + 1);
                if (end == std::string::npos) break;
                // An extended block may hold several *-separated statements.
                std::string block = text.substr(i + 1, end - i - 1);
                size_t s = 0;
                while (s < block.size()) {
                    size_t star = block.find('*', s);
                    if (star == std::string::npos) star = block.size();
                    extended(block.substr(s, star - s));
                    s = star + 1;
                }
                i = end + 1;
            } else if (c == '*') {
                word(current_);
                current_.clear();
                ++i;
            } else if (c == '\n' || c == '\r' || c == ' ') {
                ++i;
            } else {
                current_.push_back(c);
                ++i;
            }
        }
        flush();

        GerberImage img;
        img.dark = std::move(image_);
        // Union each net's own geometry: the objects overlap heavily (every
        // trace corner is two draws sharing a stroke cap), and the consumer
        // wants one region per net, not thousands of stamped shapes.
        for (auto& [name, paths] : netPaths_) {
            NetArea na;
            na.name = name;   // empty = the no-net bucket
            na.area = BooleanOp(ClipType::Union, FillRule::NonZero, paths,
                                Paths64{});
            na.routedMm = netLen_[name];
            na.segments = netSegs_[name];
            img.nets.push_back(std::move(na));
        }
        // Do NOT normalize winding here. image_ is the result of Clipper boolean
        // ops, so it is already correctly wound -- outer contours positive, holes
        // (antipad clearances in a plane, ring interiors) negative. Forcing every
        // path positive would fill every hole. normalizeWinding is for the
        // KiCad path's soup of overlapping same-sign primitives, not for a
        // finished polygon-with-holes.
        img.function = function_;
        img.hasFunction = hasFunction_;
        img.warnings = std::move(warnings_);
        img.ok = true;
        return img;
    }

private:
    std::string name_;
    std::string current_;

    // coordinate format
    int xInt_ = 3, xDec_ = 6, yInt_ = 3, yDec_ = 6;
    double unitScale_ = 1.0;  // to mm

    std::map<int, Aperture> apertures_;
    std::map<std::string, Macro> macros_;
    int currentAperture_ = -1;

    double curX_ = 0, curY_ = 0;
    int interp_ = 1;         // 1 linear, 2 CW, 3 CCW
    bool multiQuadrant_ = true;
    bool regionMode_ = false;
    bool polarityDark_ = true;

    Path64 regionContour_;
    Paths64 regionContours_;

    // accumulation with batched polarity
    Paths64 image_;
    Paths64 pending_;
    std::string currentNet_;                       // active %TO.N%, "" if none
    std::map<std::string, Paths64> netPaths_;
    std::map<std::string, double> netLen_;
    std::map<std::string, std::vector<NetArea::Seg>> netSegs_;
    bool pendingDark_ = true;

    FileFunction function_;
    bool hasFunction_ = false;
    std::vector<std::string> warnings_;

    void warn(const std::string& w) {
        for (const std::string& e : warnings_) if (e == w) return;
        warnings_.push_back(w);
    }

    void flush() {
        if (pending_.empty()) return;
        image_ = BooleanOp(pendingDark_ ? ClipType::Union : ClipType::Difference,
                           FillRule::NonZero, image_, pending_);
        pending_.clear();
    }
    void addDark(Paths64 shape) {
        if (!polarityDark_ != !pendingDark_ && !pending_.empty()) flush();
        pendingDark_ = polarityDark_;
        // Tag dark geometry with the current net BEFORE it is composited --
        // `image_` is one merged union in which per-object identity is gone.
        // Clear polarity is deliberately not recorded: a clear region belongs
        // to no net, and subtracting it from the net area would misreport a
        // thermal relief as a break in the net.
        // EVERY dark object must land in a bucket, including the ones with no
        // net -- an untagged pad, a fiducial, anything marked N/C. assemble()
        // extrudes the per-net regions INSTEAD OF the bulk art once any net is
        // present, so copper that reaches no bucket is not merely unhighlighted,
        // it is never built at all. The empty key is the no-net bucket and the
        // project layer maps it to net -1.
        if (polarityDark_) {
            Paths64& bucket = netPaths_[currentNet_];
            bucket.insert(bucket.end(), shape.begin(), shape.end());
        }
        for (Path64& p : shape) pending_.push_back(std::move(p));
    }

    double num(std::string_view s, int decimals) {
        // Signed integer whose last `decimals` digits are fractional.
        bool neg = false;
        size_t k = 0;
        if (k < s.size() && (s[k] == '+' || s[k] == '-')) { neg = s[k] == '-'; ++k; }
        long long v = 0;
        for (; k < s.size(); ++k) {
            if (!std::isdigit(s[k])) break;
            v = v * 10 + (s[k] - '0');
        }
        double d = static_cast<double>(v) / std::pow(10.0, decimals) * unitScale_;
        return neg ? -d : d;
    }

    // ---- extended (%...%) commands ----
    void extended(std::string cmd) {
        // Statements inside a %...% block are split on '*' but keep the newlines
        // that separated the source lines, so each arrives with leading
        // whitespace. Trim it: otherwise a macro primitive line ("4,1,...")
        // presents as "\n4,1,..." and its first char is not a digit, so the
        // macro-primitive collector never fires and every macro aperture
        // silently produces nothing.
        size_t b = 0;
        while (b < cmd.size() && std::isspace(static_cast<unsigned char>(cmd[b]))) ++b;
        size_t e = cmd.size();
        while (e > b && std::isspace(static_cast<unsigned char>(cmd[e - 1]))) --e;
        cmd = cmd.substr(b, e - b);
        if (cmd.empty()) return;

        // A macro definition spans several *-separated statements inside one
        // %...% block: "AMName", then primitive lines that start with a digit.
        // The tokeniser hands each statement here separately, so once an AM is
        // open, digit-leading statements are its primitives.
        if (collectingMacro_) {
            if (std::isdigit(static_cast<unsigned char>(cmd[0]))) {
                macros_[openMacroName_].primitives.push_back(cmd);
                return;
            }
            collectingMacro_ = false;  // a non-primitive ends the macro
        }

        if (cmd.rfind("FS", 0) == 0) {  // %FSLAX46Y46
            const size_t xp = cmd.find('X');
            const size_t yp = cmd.find('Y');
            if (xp != std::string::npos && xp + 2 < cmd.size()) {
                xInt_ = cmd[xp + 1] - '0';
                xDec_ = cmd[xp + 2] - '0';
            }
            if (yp != std::string::npos && yp + 2 < cmd.size()) {
                yInt_ = cmd[yp + 1] - '0';
                yDec_ = cmd[yp + 2] - '0';
            }
        } else if (cmd == "MOMM") {
            unitScale_ = 1.0;
        } else if (cmd == "MOIN") {
            unitScale_ = 25.4;
        } else if (cmd == "LPD") {
            polarityDark_ = true;
        } else if (cmd == "LPC") {
            polarityDark_ = false;
        } else if (cmd.rfind("ADD", 0) == 0) {
            defineAperture(cmd.substr(3));
        } else if (cmd.rfind("AM", 0) == 0) {
            defineMacro(cmd.substr(2));
        } else if (cmd.rfind("TF.FileFunction,", 0) == 0) {
            function_ = parseFileFunction(cmd.substr(16));
            hasFunction_ = true;
        } else if (cmd.rfind("TO.N,", 0) == 0) {
            // The object attribute that names a net. Applies to every object
            // drawn from here until it is replaced or deleted -- this is how a
            // Gerber package carries connectivity without a schematic.
            currentNet_ = cmd.substr(5);
            // Multi-valued attributes are comma-separated; the net name is the
            // first field. "N/C" marks a deliberately unconnected object.
            const size_t comma = currentNet_.find(',');
            if (comma != std::string::npos) currentNet_.resize(comma);
            if (currentNet_ == "N/C" || currentNet_.empty()) currentNet_.clear();
        } else if (cmd.rfind("TD", 0) == 0) {
            // %TD*% deletes ALL object attributes; %TD.N*% just the net one.
            const std::string which = cmd.substr(2);
            if (which.empty() || which == ".N") currentNet_.clear();
        } else if (cmd.rfind("TF", 0) == 0 || cmd.rfind("TA", 0) == 0 ||
                   cmd.rfind("TO", 0) == 0 ||
                   cmd.rfind("IP", 0) == 0 || cmd.rfind("IN", 0) == 0 ||
                   cmd.rfind("LN", 0) == 0) {
            // other attributes / image name / polarity -- no geometry
        } else if (cmd.rfind("SR", 0) == 0 || cmd.rfind("AB", 0) == 0) {
            warn("step-and-repeat / aperture blocks not implemented (" + cmd + ")");
        } else if (cmd.rfind("MI", 0) == 0 || cmd.rfind("OF", 0) == 0 ||
                   cmd.rfind("SF", 0) == 0 || cmd.rfind("AS", 0) == 0) {
            warn("deprecated transform command ignored (" + cmd + ")");
        }
    }

    void defineMacro(const std::string& body) {
        // The statement is just the macro name ("AMRoundRect" -> "RoundRect").
        // Its primitive lines are the following statements in the same block and
        // are gathered by extended()'s collectingMacro_ path.
        openMacroName_ = body;
        macros_[openMacroName_] = Macro{};
        collectingMacro_ = true;
    }

    std::string openMacroName_;
    bool collectingMacro_ = false;

    void defineAperture(const std::string& s) {
        // "10C,1.5"  "10RoundRect,0.225X..."  "11O,0.9X1.7"
        size_t k = 0;
        int code = 0;
        while (k < s.size() && std::isdigit(s[k])) { code = code * 10 + (s[k] - '0'); ++k; }
        const size_t comma = s.find(',', k);
        const std::string tmpl = s.substr(k, (comma == std::string::npos ? s.size() : comma) - k);

        std::vector<double> params;
        if (comma != std::string::npos) {
            std::stringstream ss(s.substr(comma + 1));
            std::string p;
            while (std::getline(ss, p, 'X')) params.push_back(std::atof(p.c_str()) * unitScale_);
        }

        Aperture ap;
        ap.params = params;
        if (tmpl == "C") {
            ap.type = Aperture::Type::Circle;
            ap.diameter = params.empty() ? 0.0 : params[0];
        } else if (tmpl == "R") {
            ap.type = Aperture::Type::Rect;
        } else if (tmpl == "O") {
            ap.type = Aperture::Type::Obround;
        } else if (tmpl == "P") {
            ap.type = Aperture::Type::Polygon;
        } else {
            ap.type = Aperture::Type::Macro;
            ap.macroName = tmpl;
        }
        apertures_[code] = std::move(ap);
    }

    Paths64 flashAperture(const Aperture& ap, double x, double y) {
        switch (ap.type) {
            case Aperture::Type::Circle:
                return {circle(x, y, (ap.params.empty() ? 0.0 : ap.params[0]) * 0.5)};
            case Aperture::Type::Rect:
                if (ap.params.size() >= 2) return {rect(x, y, ap.params[0], ap.params[1])};
                return {};
            case Aperture::Type::Obround:
                if (ap.params.size() >= 2) return obround(x, y, ap.params[0], ap.params[1]);
                return {};
            case Aperture::Type::Polygon:
                if (ap.params.size() >= 2)
                    return {regularPolygon(x, y, ap.params[0],
                                           static_cast<int>(ap.params[1]),
                                           ap.params.size() > 2 ? ap.params[2] : 0.0)};
                return {};
            case Aperture::Type::Macro: {
                auto it = macros_.find(ap.macroName);
                if (it == macros_.end()) {
                    warn("flash of undefined macro " + ap.macroName);
                    return {};
                }
                return evalMacro(it->second, ap.params, x, y, warnings_);
            }
        }
        return {};
    }

    // ---- ordinary (*-terminated) words ----
    void word(const std::string& w) {
        if (w.empty()) return;

        // KiCad embeds X2 attributes as G04 comments -- "G04 #@! TF.FileFunction,
        // Profile,NP*" -- when "Use extended X2 format" is OFF (the %TF...*% form
        // is only written when it is ON). Route the payload after the "#@!" marker
        // to the same handler as a real %TF block, so file classification (and
        // thus finding the board outline) works for either export style. Without
        // this, an X2-off export has no recognised Profile and cannot be placed.
        const size_t marker = w.find("#@!");
        if (marker != std::string::npos) {
            extended(w.substr(marker + 3));
            return;
        }

        // Parse leading G / D codes and coordinates.
        // A line can be like: G01X123Y456D01  or  D10  or  G36
        size_t i = 0;
        bool haveX = false, haveY = false, haveI = false, haveJ = false;
        double nx = curX_, ny = curY_, ni = 0, nj = 0;
        int dcode = -1;

        while (i < w.size()) {
            const char c = w[i];
            if (c == 'G') {
                int g = 0; ++i;
                while (i < w.size() && std::isdigit(w[i])) { g = g * 10 + (w[i] - '0'); ++i; }
                applyG(g);
            } else if (c == 'D') {
                int d = 0; ++i;
                while (i < w.size() && std::isdigit(w[i])) { d = d * 10 + (w[i] - '0'); ++i; }
                dcode = d;
            } else if (c == 'X' || c == 'Y' || c == 'I' || c == 'J') {
                ++i;
                const size_t start = i;
                if (i < w.size() && (w[i] == '+' || w[i] == '-')) ++i;
                while (i < w.size() && std::isdigit(w[i])) ++i;
                const std::string_view v(w.data() + start, i - start);
                if (c == 'X') { nx = num(v, xDec_); haveX = true; }
                else if (c == 'Y') { ny = num(v, yDec_); haveY = true; }
                else if (c == 'I') { ni = num(v, xDec_); haveI = true; }
                else { nj = num(v, yDec_); haveJ = true; }
            } else {
                ++i;  // skip unknown
            }
        }
        (void)haveX; (void)haveY;

        if (dcode == 1) operationDraw(nx, ny, ni, nj, haveI || haveJ);
        else if (dcode == 2) operationMove(nx, ny);
        else if (dcode == 3) operationFlash(nx, ny);
        else if (dcode >= 10) currentAperture_ = dcode;
        else if (dcode < 0 && (haveI || haveJ || nx != curX_ || ny != curY_)) {
            // A bare coordinate with no D-code: modal, repeats the last op. Rare
            // from KiCad; treat as a move to keep position sane.
            curX_ = nx; curY_ = ny;
        }
    }

    void applyG(int g) {
        switch (g) {
            case 1: interp_ = 1; break;
            case 2: interp_ = 2; break;
            case 3: interp_ = 3; break;
            case 74: multiQuadrant_ = false; break;
            case 75: multiQuadrant_ = true; break;
            case 36:
                regionMode_ = true;
                regionContour_.clear();
                regionContours_.clear();
                break;
            case 37:
                endRegion();
                regionMode_ = false;
                break;
            case 4: break;   // comment
            case 54: break;  // deprecated aperture select prefix
            case 70: unitScale_ = 25.4; break;  // deprecated inch
            case 71: unitScale_ = 1.0; break;   // deprecated mm
            default: break;
        }
    }

    // Arc chording in mm. The Clipper version below wraps this rather than
    // repeating the maths, so the net graph is chorded EXACTLY like the copper
    // it describes -- a separate approximation could disagree with what is
    // drawn, and a length that does not match the visible trace is worse than
    // no length at all.
    //
    // Returns points k = 1..segs: the start is the caller's current point and
    // is deliberately excluded, the end point is included.
    std::vector<std::pair<double, double>> arcPointsMm(double sx, double sy,
                                                       double ex, double ey,
                                                       double cx, double cy,
                                                       bool cw) {
        std::vector<std::pair<double, double>> pts;
        const double r = std::hypot(sx - cx, sy - cy);
        double a0 = std::atan2(sy - cy, sx - cx);
        double a1 = std::atan2(ey - cy, ex - cx);
        // Sweep direction. Gerber G02 is clockwise.
        if (cw) { if (a1 >= a0) a1 -= 2 * std::numbers::pi; }
        else { if (a1 <= a0) a1 += 2 * std::numbers::pi; }
        // Full circle if start==end (common for isolated circular pours).
        if (std::abs(ex - sx) < 1e-9 && std::abs(ey - sy) < 1e-9) {
            a1 = a0 + (cw ? -2 : 2) * std::numbers::pi;
        }
        const int segs = std::max(2, static_cast<int>(
                                         std::ceil(std::abs(a1 - a0) /
                                                   (2 * std::numbers::pi) *
                                                   kArcSegments)));
        for (int k = 1; k <= segs; ++k) {
            const double t = a0 + (a1 - a0) * k / segs;
            pts.emplace_back(cx + r * std::cos(t), cy + r * std::sin(t));
        }
        return pts;
    }

    std::vector<Point64> arcPoints(double sx, double sy, double ex, double ey,
                                   double cx, double cy, bool cw) {
        std::vector<Point64> out;
        for (const auto& [px, py] : arcPointsMm(sx, sy, ex, ey, cx, cy, cw))
            out.push_back(toClip(px, py));
        return out;
    }

    void operationMove(double x, double y) {
        if (regionMode_ && regionContour_.size() >= 3) {
            regionContours_.push_back(regionContour_);
        }
        if (regionMode_) {
            regionContour_.clear();
            regionContour_.push_back(toClip(x, y));
        }
        curX_ = x; curY_ = y;
    }

    void operationDraw(double x, double y, double i, double j, bool hasIJ) {
        // Record draws on a net as graph segments: the same track-endpoint
        // graph the KiCad path builds, and what lets the measure tool report
        // distance ALONG a net rather than through the air. Region fills are
        // excluded -- a zone outline is not a route.
        //
        // Arcs are chorded with the same subdivision as the copper they draw,
        // so a net routed in arcs reports its real length instead of silently
        // under-reporting it. Chord length runs a hair under true arc length
        // (~0.02% at 48 segments per full circle); that is deliberate, because
        // the alternative -- an analytic arc length that disagrees with the
        // walked graph -- would make the total and the between-two-points
        // distance contradict each other.
        if (!regionMode_ && !currentNet_.empty()) {
            const auto edge = [&](double ax, double ay, double bx, double by) {
                const double len = std::hypot(bx - ax, by - ay);
                if (len <= 1e-9) return;
                netLen_[currentNet_] += len;
                netSegs_[currentNet_].push_back({ax, ay, bx, by});
            };
            if (interp_ == 1) {
                edge(curX_, curY_, x, y);
            } else if (hasIJ) {
                double px = curX_, py = curY_;
                for (const auto& [qx, qy] : arcPointsMm(curX_, curY_, x, y,
                                                        curX_ + i, curY_ + j,
                                                        interp_ == 2)) {
                    edge(px, py, qx, qy);
                    px = qx; py = qy;
                }
            }
        }
        if (regionMode_) {
            if (regionContour_.empty()) regionContour_.push_back(toClip(curX_, curY_));
            if (interp_ == 1) {
                regionContour_.push_back(toClip(x, y));
            } else if (hasIJ) {
                const auto pts = arcPoints(curX_, curY_, x, y, curX_ + i,
                                           curY_ + j, interp_ == 2);
                for (const Point64& p : pts) regionContour_.push_back(p);
            }
            curX_ = x; curY_ = y;
            return;
        }

        // A track: stroke the path swept by the aperture. Only circular
        // apertures stroke cleanly; a rectangular draw aperture is a rare
        // "smear" we approximate with a round pen and warn about.
        auto it = apertures_.find(currentAperture_);
        double width = 0.0;
        if (it != apertures_.end()) {
            if (it->second.type == Aperture::Type::Circle) width = it->second.diameter;
            else {
                width = it->second.params.empty() ? 0.0 : it->second.params[0];
                warn("draw with non-circular aperture approximated as round");
            }
        }

        Path64 spine;
        spine.push_back(toClip(curX_, curY_));
        if (interp_ == 1 || !hasIJ) {
            spine.push_back(toClip(x, y));
        } else {
            const auto pts = arcPoints(curX_, curY_, x, y, curX_ + i, curY_ + j,
                                       interp_ == 2);
            for (const Point64& p : pts) spine.push_back(p);
        }
        if (width > 0.0 && spine.size() >= 2) {
            ClipperOffset co;
            co.ArcTolerance(geom::kScale * 0.001);
            co.AddPath(spine, JoinType::Round, EndType::Round);
            Paths64 out;
            co.Execute(width * 0.5 * geom::kScale, out);
            addDark(std::move(out));
        }
        curX_ = x; curY_ = y;
    }

    void operationFlash(double x, double y) {
        auto it = apertures_.find(currentAperture_);
        if (it != apertures_.end()) addDark(flashAperture(it->second, x, y));
        curX_ = x; curY_ = y;
    }

    void endRegion() {
        if (regionContour_.size() >= 3) regionContours_.push_back(regionContour_);
        regionContour_.clear();
        if (!regionContours_.empty()) addDark(regionContours_);
        regionContours_.clear();
    }
};

}  // namespace

FileFunction parseFileFunctionPublic(const std::string& value) {
    return parseFileFunction(value);
}

GerberImage parseGerber(const std::string& text, const std::string& name) {
    Parser parser(name);
    GerberImage img = parser.run(text);
    if (img.dark.empty() && img.ok) {
        img.warnings.push_back(name + ": parsed but produced no geometry");
    }
    return img;
}

}  // namespace pcbview::gerber
