#include "text/stroke_text.h"

#include <cmath>
#include <numbers>

#include "text/newstroke_data.h"

namespace pcbview::text {
namespace {

constexpr char kHersheyOrigin = 'R';  // 0x52; all coordinates are offset by this

struct Glyph {
    double left = 0.0;   // in Hershey units
    double right = 0.0;
    std::vector<std::vector<Vec2>> strokes;  // Hershey units, Y down
};

// Decode one Hershey glyph string.
//
//   [0], [1]  left and right bounds
//   then      (x, y) pairs
//   x == ' '  PEN UP -- end this stroke and start another
Glyph decode(const char* s) {
    Glyph g;
    if (!s || !s[0] || !s[1]) return g;

    g.left = static_cast<double>(s[0] - kHersheyOrigin);
    g.right = static_cast<double>(s[1] - kHersheyOrigin);

    std::vector<Vec2> run;
    for (const char* p = s + 2; p[0] && p[1]; p += 2) {
        if (p[0] == ' ') {  // pen up
            if (run.size() >= 2) g.strokes.push_back(run);
            run.clear();
            continue;
        }
        run.push_back(Vec2{static_cast<double>(p[0] - kHersheyOrigin),
                           static_cast<double>(p[1] - kHersheyOrigin)});
    }
    // A single point is a dot in Hershey, but a zero-length run is noise.
    if (run.size() >= 2) g.strokes.push_back(run);
    return g;
}

const Glyph& glyphFor(char c) {
    static const std::vector<Glyph> table = [] {
        std::vector<Glyph> t;
        t.reserve(std::size(font::kNewstrokeBasicLatin));
        for (const char* s : font::kNewstrokeBasicLatin) t.push_back(decode(s));
        return t;
    }();
    static const Glyph empty;

    const int code = static_cast<unsigned char>(c);
    if (code < font::kNewstrokeFirstChar || code > font::kNewstrokeLastChar) {
        return empty;
    }
    const size_t index = static_cast<size_t>(code - font::kNewstrokeFirstChar);
    return index < table.size() ? table[index] : empty;
}

// Matches the importer's convention: KiCad's angle is clockwise in raw file
// coordinates. Text must use the same one as pads or it lands rotated wrong.
Vec2 rotateKicad(Vec2 p, double degrees) {
    const double rad = -degrees * std::numbers::pi / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    return Vec2{p.x * c - p.y * s, p.x * s + p.y * c};
}

}  // namespace

double measure(const std::string& utf8, const TextStyle& style) {
    const double scale = style.size.x / font::kNewstrokeUnitsPerEm;
    double width = 0.0;
    for (char c : utf8) {
        const Glyph& g = glyphFor(c);
        width += (g.right - g.left) * scale;
    }
    return width;
}

std::vector<Polyline> layout(const std::string& utf8, Vec2 origin,
                             const TextStyle& style) {
    std::vector<Polyline> out;

    const double scaleX = style.size.x / font::kNewstrokeUnitsPerEm;
    const double scaleY = style.size.y / font::kNewstrokeUnitsPerEm;

    // KiCad centres reference/value text on the item position, so start half a
    // string-width to the left.
    double penX = -measure(utf8, style) * 0.5;

    for (char c : utf8) {
        const Glyph& g = glyphFor(c);

        for (const std::vector<Vec2>& stroke : g.strokes) {
            Polyline line;
            line.reserve(stroke.size());
            for (const Vec2& p : stroke) {
                // Glyph space -> text space. `left` is subtracted so each glyph
                // starts at the pen rather than at its own bearing.
                double x = (p.x - g.left) * scaleX + penX;
                double y = p.y * scaleY;

                // Italic shears with height. KiCad's ITALIC_TILT is 1/8, and the
                // shear is negative because Y runs downward.
                if (style.italic) x -= y * 0.125;

                // Bottom-side text is mirrored so it reads correctly when the
                // board is viewed from below.
                if (style.mirror) x = -x;

                Vec2 q = rotateKicad(Vec2{x, y}, style.rotation);
                line.push_back(Vec2{origin.x + q.x, origin.y + q.y});
            }
            if (line.size() >= 2) out.push_back(std::move(line));
        }
        penX += (g.right - g.left) * scaleX;
    }
    return out;
}

}  // namespace pcbview::text
