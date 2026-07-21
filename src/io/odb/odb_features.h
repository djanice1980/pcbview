#pragma once

// ODB++ features file parser: the per-layer geometry language (L/P/A/S/T
// records over a $-indexed symbol table). Parsing and geometry realization
// are split: parseFeatures() gives the records verbatim (their file order is
// load-bearing -- eda/data names features by index), and FeatureRealizer
// turns records into polygons, caching each symbol's geometry so a board with
// one pad shape flashed 100k times realizes it once.

#include <clipper2/clipper.h>

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace pcbview::odb {

struct Feature {
    enum class Kind { Line, Pad, Arc, Surface, Text } kind = Kind::Pad;
    bool dark = true;  // polarity P; N erases
    int symbol = -1;   // index into FeaturesFile::symbols (Line/Pad/Arc)
    // Coordinates in mm. Line/Arc: xs..ye endpoints (+ centre for Arc);
    // Pad: xs,ys is the flash position.
    double xs = 0, ys = 0, xe = 0, ye = 0, xc = 0, yc = 0;
    bool cw = false;       // Arc direction
    double rotDeg = 0;     // Pad: clockwise rotation
    bool mirror = false;   // Pad: mirrored in x before rotation
    // Surface: resolved solid polygons (islands minus holes), mm scaled by
    // geom::kScale, ready to union.
    Clipper2Lib::Paths64 surface;
};

struct FeaturesFile {
    struct Symbol {
        std::string name;
        bool metricDims = false;  // "$n name M" -> dimensions in mm, not mils
    };
    std::vector<Symbol> symbols;
    std::vector<Feature> features;  // file order == eda FID feature index
    int textCount = 0;              // T records met (not rendered)
};

// `metricFile`: fallback when the file has no UNITS line (older jobs are
// imperial by default).
FeaturesFile parseFeatures(const std::string& text, bool metricFile = false);

// A standard symbol's geometry at the origin, unrotated: r, s, rect (round
// corners honoured, chamfers squared), oval, donut_r, oct, di, tri, el.
// Returns empty paths for anything it does not recognise.
Clipper2Lib::Paths64 standardSymbol(const std::string& name, bool metricDims);

// Resolves user-defined symbols: returns the features text of
// symbols/<name>/features, or empty when absent.
using UserSymbolFn = std::function<std::string(const std::string& name)>;

class FeatureRealizer {
public:
    FeatureRealizer(const FeaturesFile& ff, UserSymbolFn userSymbol,
                    std::vector<std::string>& warnings, int depth = 0)
        : ff_(ff), user_(std::move(userSymbol)), warnings_(warnings),
          depth_(depth) {}

    // The feature's solid polygons, positioned. Empty for text and for
    // features whose symbol could not be resolved (warned once per name).
    Clipper2Lib::Paths64 realize(const Feature& f);

private:
    const Clipper2Lib::Paths64& symbolPolys(int idx);
    double symbolWidthMm(int idx);  // brush width for L/A strokes
    bool symbolIsSquare(int idx);

    const FeaturesFile& ff_;
    UserSymbolFn user_;
    std::vector<std::string>& warnings_;
    int depth_ = 0;
    std::map<int, Clipper2Lib::Paths64> polyCache_;
    std::map<int, double> widthCache_;
    std::map<std::string, bool> warned_;
};

}  // namespace pcbview::odb
