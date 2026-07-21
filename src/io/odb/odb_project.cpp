#include "io/odb/odb_project.h"

#include <miniz.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

#include "io/gerber/gerber_project.h"
#include "io/shapes.h"
#include "io/odb/odb_eda.h"
#include "io/odb/odb_features.h"
#include "io/odb/odb_fs.h"
#include "io/odb/odb_matrix.h"

namespace fs = std::filesystem;
using namespace Clipper2Lib;

namespace pcbview::odb {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool endsWithNoCase(const std::string& s, const char* suffix) {
    const std::string suf(suffix);
    if (s.size() < suf.size()) return false;
    return toLower(s.substr(s.size() - suf.size())) == suf;
}

bool isCopperType(const std::string& t) {
    return t == "SIGNAL" || t == "POWER_GROUND" || t == "MIXED";
}

// The board step: jobs holding a panel step-repeat the board step inside it,
// and the viewer wants the BOARD. Any step referenced by another step's
// STEP-REPEAT blocks is a child; prefer the most-referenced child, else the
// first step.
std::string chooseStep(const OdbFs& fsys, const Matrix& m,
                       std::vector<std::string>& notes) {
    std::vector<std::string> steps = m.steps;
    if (steps.empty()) steps = fsys.dirs("steps");
    if (steps.empty()) return {};
    std::map<std::string, int> referenced;
    for (const std::string& s : steps) {
        const std::string hdr = fsys.read("steps/" + s + "/stephdr");
        for (const Block& b : parseStructured(hdr)) {
            if (b.name != "STEP-REPEAT") continue;
            const std::string child = toLower(b.get("NAME"));
            if (!child.empty()) ++referenced[child];
        }
    }
    std::string best;
    int bestRefs = 0;
    for (const std::string& s : steps) {
        const auto it = referenced.find(toLower(s));
        const int refs = it == referenced.end() ? 0 : it->second;
        if (refs > bestRefs) {
            best = s;
            bestRefs = refs;
        }
    }
    if (!best.empty()) {
        notes.push_back("ODB++ step '" + best +
                        "' chosen (panelization steps skipped)");
        return best;
    }
    if (steps.size() > 1)
        notes.push_back("ODB++ job has " + std::to_string(steps.size()) +
                        " steps; using '" + steps.front() + "'");
    return steps.front();
}

using io::DarkClearAcc;

}  // namespace

bool isOdbJob(const std::string& path) {
    if (fs::is_directory(path)) {
        if (fs::exists(fs::path(path) / "matrix" / "matrix")) return true;
        for (const auto& e : fs::directory_iterator(path)) {
            if (e.is_directory() &&
                fs::exists(e.path() / "matrix" / "matrix"))
                return true;
        }
        return false;
    }
    if (endsWithNoCase(path, ".tgz") || endsWithNoCase(path, ".tar.gz") ||
        endsWithNoCase(path, ".tar"))
        return true;
    if (endsWithNoCase(path, ".zip")) {
        // Cheap central-directory scan: is there a matrix/matrix inside?
        mz_zip_archive zip{};
        if (!mz_zip_reader_init_file(&zip, path.c_str(), 0)) return false;
        bool found = false;
        const mz_uint n = mz_zip_reader_get_num_files(&zip);
        for (mz_uint i = 0; i < n && !found; ++i) {
            mz_zip_archive_file_stat st;
            if (!mz_zip_reader_file_stat(&zip, i, &st)) continue;
            const std::string name = toLower(st.m_filename);
            if (name == "matrix/matrix" ||
                endsWithNoCase(name, "/matrix/matrix"))
                found = true;
        }
        mz_zip_reader_end(&zip);
        return found;
    }
    return false;
}

geom::LayerArt importJob(const std::string& path) {
    geom::LayerArt art;
    art.sourcePath = path;

    OdbFs fsys = OdbFs::open(path);
    for (const std::string& w : fsys.warnings) art.warnings.push_back(w);
    if (!fsys.exists("matrix/matrix"))
        throw std::runtime_error(
            "no ODB++ job found (missing matrix/matrix): " + path);

    const Matrix matrix = parseMatrix(fsys.read("matrix/matrix"));
    const std::string step = chooseStep(fsys, matrix, art.notes);
    if (step.empty())
        throw std::runtime_error("ODB++ job has no steps: " + path);
    const std::string stepDir = "steps/" + step + "/";

    // The user-symbol resolver realizeSymbol falls back to. Symbols may carry
    // their own compression, which OdbFs already peeled.
    const UserSymbolFn userSymbol = [&fsys](const std::string& name) {
        return fsys.read("symbols/" + name + "/features");
    };

    // ---- nets (eda/data) --------------------------------------------------
    EdaData eda;
    const bool haveEda = fsys.exists(stepDir + "eda/data");
    if (haveEda) eda = parseEda(fsys.read(stepDir + "eda/data"));
    // eda layer name (lowered) -> LYR index.
    std::map<std::string, int> edaLayerIdx;
    for (size_t i = 0; i < eda.layers.size(); ++i)
        edaLayerIdx[toLower(eda.layers[i])] = static_cast<int>(i);
    // eda net index -> art net index ($NONE$ and friends collapse to -1).
    std::vector<int> netMap(eda.nets.size(), -1);
    for (size_t i = 0; i < eda.nets.size(); ++i) {
        const std::string& n = eda.nets[i];
        if (n.empty() || n == "$NONE$") continue;
        netMap[i] = static_cast<int>(art.nets.size());
        art.nets.push_back({n, 0.0, 0});
    }

    // ---- stackup ----------------------------------------------------------
    std::vector<const MatrixLayer*> copperLayers, maskLayers, silkLayers,
        drillLayers;
    for (const MatrixLayer& l : matrix.layers) {
        if (!l.context.empty() && l.context != "BOARD") continue;
        if (isCopperType(l.type)) copperLayers.push_back(&l);
        else if (l.type == "SOLDER_MASK") maskLayers.push_back(&l);
        else if (l.type == "SILK_SCREEN") silkLayers.push_back(&l);
        else if (l.type == "DRILL") drillLayers.push_back(&l);
    }
    if (copperLayers.empty())
        throw std::runtime_error("ODB++ job has no copper layers: " + path);

    const int nCopper = static_cast<int>(copperLayers.size());
    const double copperT = 0.035, maskT = 0.010;
    double total = 1.6;
    const double dielT =
        nCopper > 1 ? (total - nCopper * copperT - 2 * maskT) / (nCopper - 1)
                    : 0.0;
    art.warnings.push_back(
        "ODB++ matrix carries no thicknesses; using derived stackup");
    art.thickness = total;
    art.copperThickness = copperT;
    art.maskThickness = maskT;
    art.silkThickness = 0.010;
    const double pitch = copperT + dielT;

    // Copper row order in the matrix = physical order, top first.
    const auto copperZ = [&](int i) {
        return total - maskT - static_cast<double>(i) * pitch - copperT;
    };
    const int firstCopperRow = copperLayers.front()->row;

    // ---- copper layers ----------------------------------------------------
    int textSkipped = 0;
    for (int ci = 0; ci < nCopper; ++ci) {
        const MatrixLayer& ml = *copperLayers[ci];
        const std::string featPath = stepDir + "layers/" + ml.name + "/features";
        const FeaturesFile ff = parseFeatures(fsys.read(featPath));
        textSkipped += ff.textCount;
        if (!ml.positive)
            art.warnings.push_back("layer '" + ml.name +
                                   "' is NEGATIVE; rendered as positive");

        FeatureRealizer real(ff, userSymbol, art.warnings);
        const int lyrIdx = [&] {
            const auto it = edaLayerIdx.find(toLower(ml.name));
            return it == edaLayerIdx.end() ? -1 : it->second;
        }();

        DarkClearAcc comp;
        std::map<int, Paths64> netDark;  // art net (-1 = none) -> raw darks
        const bool top = (ci == 0), bottom = (ci == nCopper - 1);

        for (size_t fi = 0; fi < ff.features.size(); ++fi) {
            const Feature& f = ff.features[fi];
            Paths64 polys = real.realize(f);
            if (polys.empty()) continue;
            if (!f.dark) {
                comp.clear(polys);
                continue;
            }
            comp.dark(polys);
            int net = -1;
            if (lyrIdx >= 0) {
                const auto it =
                    eda.featureNet.find({lyrIdx, static_cast<int>(fi)});
                if (it != eda.featureNet.end() &&
                    it->second < static_cast<int>(netMap.size()))
                    net = netMap[it->second];
            }
            Paths64& bucket = netDark[net];
            bucket.insert(bucket.end(), polys.begin(), polys.end());

            if (net >= 0) {
                if (f.kind == Feature::Kind::Line) {
                    art.netSegments.push_back({f.xs, f.ys, f.xe, f.ye, net});
                    art.nets[net].routedMm +=
                        std::hypot(f.xe - f.xs, f.ye - f.ys);
                } else if (f.kind == Feature::Kind::Surface) {
                    art.nets[net].hasPlane = true;
                }
            }
            if (f.kind == Feature::Kind::Pad) {
                geom::LayerArt::NetPoint np;
                np.pos[0] = f.xs;
                np.pos[1] = f.ys;
                np.pos[2] = bottom && !top ? copperZ(ci)
                                           : copperZ(ci) + copperT;
                np.net = net;
                art.netPoints.push_back(np);
            }
        }

        geom::ArtLayer al;
        al.name = ml.name;  // real ODB++ names; drill spans reference them
        al.kind = LayerKind::Copper;
        al.thickness = copperT;
        al.z = copperZ(ci);
        const bool hadClears = comp.sawClear;
        al.art = comp.take();
        // Per-net split. Design rules keep nets apart, so per-net unions of
        // the dark features cover the same copper -- except where a clear
        // feature erased some of it, where each net must be re-clipped to the
        // final layer.
        for (auto& [net, darks] : netDark) {
            geom::ArtLayer::NetRegion nr;
            nr.net = net;
            nr.paths = hadClears
                           ? Intersect(darks, al.art, FillRule::NonZero)
                           : Union(darks, FillRule::NonZero);
            if (nr.paths.empty()) continue;
            // No real netlist? Every stroke feeds pseudo-net inference.
            al.netArt.push_back(std::move(nr));
        }
        if (art.nets.empty()) {
            for (const Feature& f : ff.features)
                if (f.kind == Feature::Kind::Line && f.dark)
                    al.looseSegments.push_back({f.xs, f.ys, f.xe, f.ye});
        }
        art.layers.push_back(std::move(al));
    }

    // ---- masks and silks ---------------------------------------------------
    // Which side a layer belongs to comes from its matrix ROW relative to the
    // copper stack (the spec's layer order is physical).
    const auto addSimpleLayer = [&](const MatrixLayer& ml, LayerKind kind) {
        const FeaturesFile ff =
            parseFeatures(fsys.read(stepDir + "layers/" + ml.name + "/features"));
        textSkipped += ff.textCount;
        FeatureRealizer real(ff, userSymbol, art.warnings);
        DarkClearAcc comp;
        for (const Feature& f : ff.features) {
            Paths64 polys = real.realize(f);
            if (polys.empty()) continue;
            if (f.dark) comp.dark(polys);
            else comp.clear(polys);
        }
        geom::ArtLayer al;
        al.name = ml.name;
        al.kind = kind;
        const bool top = ml.row < firstCopperRow;
        if (kind == LayerKind::Soldermask) {
            al.thickness = maskT;
            al.z = top ? total - maskT : 0.0;
            al.art = comp.take();
            al.openings = static_cast<int>(al.art.size());
        } else {
            al.thickness = art.silkThickness;
            al.z = top ? total : -art.silkThickness;
            al.art = comp.take();
        }
        art.layers.push_back(std::move(al));
    };
    for (const MatrixLayer* ml : maskLayers)
        addSimpleLayer(*ml, LayerKind::Soldermask);
    for (const MatrixLayer* ml : silkLayers)
        addSimpleLayer(*ml, LayerKind::Silkscreen);

    // ---- drills ------------------------------------------------------------
    for (const MatrixLayer* ml : drillLayers) {
        const FeaturesFile ff =
            parseFeatures(fsys.read(stepDir + "layers/" + ml->name + "/features"));
        FeatureRealizer real(ff, userSymbol, art.warnings);

        // Plating: some exporters split plated/non-plated into separate drill
        // LAYERS and say so in the name (KiCad: DRILL_NON-PLATED_F.CU-B.CU);
        // others tag tools in the layer's tools file. No signal = plated (the
        // safer default for vias, and what the Gerber path does too).
        const std::string upperName = [&] {
            std::string u = ml->name;
            std::transform(u.begin(), u.end(), u.begin(), ::toupper);
            return u;
        }();
        const bool nonPlatedLayer =
            upperName.find("NON-PLATED") != std::string::npos ||
            upperName.find("NON_PLATED") != std::string::npos;
        std::set<long long> nonPlatedSizes;  // in micrometres
        const std::string toolsText =
            fsys.read(stepDir + "layers/" + ml->name + "/tools");
        for (const Block& b : parseStructured(toolsText)) {
            if (b.name != "TOOLS" && b.name != "TOOL") continue;
            std::string type = b.get("TYPE");
            std::transform(type.begin(), type.end(), type.begin(), ::toupper);
            if (type != "NON_PLATED") continue;
            const std::string sz = b.get("DRILL_SIZE").empty()
                                       ? b.get("FINISH_SIZE")
                                       : b.get("DRILL_SIZE");
            if (sz.empty()) continue;
            // Tool sizes are in mils; features may disagree slightly, so
            // match at 1um resolution after rounding.
            nonPlatedSizes.insert(
                std::llround(std::atof(sz.c_str()) * 25.4));
        }

        // A drill layer spanning the full stack punches `drills` (+ barrels
        // when plated); a partial span becomes a blind/buried bore.
        const bool fullSpan =
            ml->startName.empty() || ml->endName.empty() ||
            (toLower(ml->startName) == toLower(copperLayers.front()->name) &&
             toLower(ml->endName) == toLower(copperLayers.back()->name));

        for (const Feature& f : ff.features) {
            if (!f.dark) continue;
            if (f.kind != Feature::Kind::Pad &&
                f.kind != Feature::Kind::Line)
                continue;
            const Paths64 polys = real.realize(f);
            if (polys.empty()) continue;
            const Rect64 bb = GetBounds(polys);
            const long long sizeUm = std::llround(
                std::max(bb.Width(), bb.Height()) / (geom::kScale * 1e-3));
            const bool plated =
                !nonPlatedLayer && nonPlatedSizes.count(sizeUm) == 0;
            for (const Path64& p : polys) {
                if (fullSpan) {
                    art.drills.push_back(p);
                    if (plated) art.barrels.push_back(p);
                } else {
                    geom::LayerArt::PartialBore bore;
                    bore.path = p;
                    bore.fromLayer = ml->startName;
                    bore.toLayer = ml->endName;
                    art.partialBores.push_back(std::move(bore));
                }
            }
        }
    }
    geom::normalizeWinding(art.drills);
    geom::normalizeWinding(art.barrels);

    // ---- outline -----------------------------------------------------------
    {
        const FeaturesFile ff = parseFeatures(fsys.read(stepDir + "profile"));
        FeatureRealizer real(ff, userSymbol, art.warnings);
        Paths64 surfaces, strokes;
        for (const Feature& f : ff.features) {
            if (!f.dark) continue;
            Paths64 polys = real.realize(f);
            Paths64& dst =
                f.kind == Feature::Kind::Surface ? surfaces : strokes;
            dst.insert(dst.end(), polys.begin(), polys.end());
        }
        if (!surfaces.empty()) {
            // Filled profile: already the board area, holes and all. Do NOT
            // normalize -- cutout winding is the data.
            art.outline = Union(surfaces, FillRule::NonZero);
        } else if (!strokes.empty()) {
            art.outline = gerber::boardFromProfile(strokes);
        } else {
            // No profile: bound the copper, with a warning.
            Paths64 all;
            for (const geom::ArtLayer& al : art.layers)
                if (al.kind == LayerKind::Copper)
                    all.insert(all.end(), al.art.begin(), al.art.end());
            const Rect64 bb = GetBounds(all);
            if (bb.Width() <= 0)
                throw std::runtime_error(
                    "ODB++ step has no profile and no copper: " + path);
            art.outline = {bb.AsPath()};
            art.warnings.push_back(
                "step has no profile; using the copper bounding box");
        }
    }

    if (textSkipped > 0)
        art.notes.push_back(std::to_string(textSkipped) +
                            " text feature(s) not rendered");
    if (!art.nets.empty())
        art.notes.push_back(
            "ODB++ eda/data netlist: " + std::to_string(art.nets.size()) +
            " nets (net highlighting and routed length available)");
    else if (haveEda)
        art.notes.push_back(
            "ODB++ eda/data present but carried no usable nets");
    art.notes.push_back("ODB++ product model: step '" + step + "', " +
                        std::to_string(art.layers.size()) + " layers");
    return art;
}

}  // namespace pcbview::odb
