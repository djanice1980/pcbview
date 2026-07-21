#include "io/ipc2581/ipc2581.h"

#include <pugixml.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <stdexcept>

#include "io/shapes.h"

using namespace Clipper2Lib;
namespace px = pugi;

namespace pcbview::ipc2581 {
namespace {

std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

double unitsScale(const char* units) {
    const std::string u = units ? units : "";
    if (u == "INCH") return 25.4;
    if (u == "MICRON") return 0.001;
    return 1.0;  // MILLIMETER, and the sane default
}

// A LineDesc: brush width and end style for strokes.
struct LineStyle {
    double widthMm = 0.0;
    bool square = false;  // lineEnd="SQUARE"; ROUND/NONE stroke round
};

// ---- primitive realization -------------------------------------------------

// One dictionary entry (standard or user), realized lazily and cached: a
// board flashes the same pad shape thousands of times.
class Shapes {
public:
    Shapes(std::vector<std::string>& warnings) : warnings_(warnings) {}

    void addStandard(const px::xml_node& entry, double scale) {
        standard_[entry.attribute("id").value()] = {entry, scale};
    }
    void addUser(const px::xml_node& entry, double scale) {
        user_[entry.attribute("id").value()] = {entry, scale};
    }
    void addLine(const std::string& id, LineStyle st) { lines_[id] = st; }

    const LineStyle* line(const std::string& id) const {
        const auto it = lines_.find(id);
        return it == lines_.end() ? nullptr : &it->second;
    }

    // Geometry of primitive `id` at the origin. Empty + one-time warning for
    // anything unsupported.
    const Paths64& get(const std::string& id) {
        const auto it = cache_.find(id);
        if (it != cache_.end()) return it->second;
        Paths64 polys;
        if (const auto st = standard_.find(id); st != standard_.end()) {
            polys = realizeStandard(st->second.first, st->second.second);
        } else if (const auto us = user_.find(id); us != user_.end()) {
            polys = realizeUser(us->second.first, us->second.second);
        }
        if (polys.empty() && !warned_[id]) {
            warned_[id] = true;
            warnings_.push_back("unsupported IPC-2581 primitive '" + id +
                                "' -- features using it were skipped");
        }
        return cache_.emplace(id, std::move(polys)).first->second;
    }

    // The polygon/contour builders need to be shared with layer parsing;
    // exposed as statics.
    static Path64 polygonPath(const px::xml_node& poly, double scale) {
        Path64 out;
        double lx = 0, ly = 0;
        for (const px::xml_node& n : poly.children()) {
            const std::string name = n.name();
            const double x = n.attribute("x").as_double() * scale;
            const double y = n.attribute("y").as_double() * scale;
            if (name == "PolyBegin") {
                out.push_back(io::toClip(x, y));
                lx = x;
                ly = y;
            } else if (name == "PolyStepSegment") {
                out.push_back(io::toClip(x, y));
                lx = x;
                ly = y;
            } else if (name == "PolyStepCurve") {
                const double cx = n.attribute("centerX").as_double() * scale;
                const double cy = n.attribute("centerY").as_double() * scale;
                io::arcAppend(out, lx, ly, x, y, cx, cy,
                              n.attribute("clockwise").as_bool());
                lx = x;
                ly = y;
            }
        }
        return out;
    }

    // Contour: a Polygon plus Cutout children -> solid polygons.
    static Paths64 contourPaths(const px::xml_node& contour, double scale) {
        Paths64 islands, holes;
        for (const px::xml_node& n : contour.children()) {
            const std::string name = n.name();
            Path64 p = polygonPath(n, scale);
            if (p.size() < 3) continue;
            if (Area(p) < 0) std::reverse(p.begin(), p.end());
            if (name == "Polygon") islands.push_back(std::move(p));
            else if (name == "Cutout") holes.push_back(std::move(p));
        }
        if (islands.empty()) return {};
        return holes.empty() ? Union(islands, FillRule::NonZero)
                             : Difference(islands, holes, FillRule::NonZero);
    }

private:
    Paths64 realizeStandard(const px::xml_node& entry, double scale) {
        // The entry wraps exactly one shape element.
        for (const px::xml_node& s : entry.children()) {
            const std::string name = s.name();
            const auto attr = [&](const char* a) {
                return s.attribute(a).as_double() * scale;
            };
            if (name == "Circle")
                return {io::circlePath(0, 0, attr("diameter") * 0.5)};
            if (name == "RectCenter" || name == "RectCham")
                // Chamfer corners rendered square: sub-width detail.
                return {io::rectPath(attr("width"), attr("height"))};
            if (name == "RectRound") {
                const double w = attr("width"), h = attr("height");
                const double r = attr("radius");
                if (r > 0 && r < std::min(w, h) * 0.5)
                    return io::roundedRect(w, h, r);
                return {io::rectPath(w, h)};
            }
            if (name == "Oval") return io::stadium(attr("width"), attr("height"));
            if (name == "Ellipse")
                return {io::ellipsePath(attr("width"), attr("height"))};
            if (name == "Donut") {
                // outerDia/innerDia in rev B; shape="ROUND" assumed.
                const double od = s.attribute("outerDia").as_double() * scale;
                const double id = s.attribute("innerDia").as_double() * scale;
                if (od <= 0) break;
                Path64 hole = io::circlePath(0, 0, id * 0.5);
                std::reverse(hole.begin(), hole.end());
                return {io::circlePath(0, 0, od * 0.5), std::move(hole)};
            }
            if (name == "Octagon") {
                const double w = attr("width"), h = attr("height");
                const double r = attr("cornerSize");
                const double hw = w * 0.5, hh = h * 0.5;
                return {Path64{io::toClip(-hw + r, -hh), io::toClip(hw - r, -hh),
                               io::toClip(hw, -hh + r), io::toClip(hw, hh - r),
                               io::toClip(hw - r, hh), io::toClip(-hw + r, hh),
                               io::toClip(-hw, hh - r), io::toClip(-hw, -hh + r)}};
            }
            if (name == "Diamond") {
                const double hw = attr("width") * 0.5, hh = attr("height") * 0.5;
                return {Path64{io::toClip(0, -hh), io::toClip(hw, 0),
                               io::toClip(0, hh), io::toClip(-hw, 0)}};
            }
            if (name == "Triangle") {
                const double hw = attr("base") * 0.5, hh = attr("height") * 0.5;
                return {Path64{io::toClip(-hw, -hh), io::toClip(hw, -hh),
                               io::toClip(0, hh)}};
            }
            if (name == "Contour") return contourPaths(s, scale);
        }
        return {};
    }

    Paths64 realizeUser(const px::xml_node& entry, double scale) {
        // UserSpecial / UserPrimitive: a bag of ordinary shape features at
        // the origin, composited like a tiny layer.
        io::DarkClearAcc acc;
        for (const px::xml_node& wrap : entry.children())
            realizeShapesInto(wrap, scale, acc);
        return acc.take();
    }

    void realizeShapesInto(const px::xml_node& parent, double scale,
                           io::DarkClearAcc& acc) {
        for (const px::xml_node& n : parent.children()) {
            const std::string name = n.name();
            if (name == "Contour") {
                acc.dark(contourPaths(n, scale));
            } else if (name == "Line" || name == "Polyline" || name == "Arc") {
                acc.dark(strokePaths(n, scale, *this));
            } else if (name == "UserSpecial" || name == "Features") {
                realizeShapesInto(n, scale, acc);
            }
        }
    }

public:
    // Stroke geometry for Line/Polyline/Arc nodes: spine from the node,
    // width and end style from an inline LineDesc or a LineDescRef.
    static Paths64 strokePaths(const px::xml_node& n, double scale,
                               Shapes& shapes) {
        LineStyle st;
        if (const px::xml_node d = n.child("LineDesc")) {
            st.widthMm = d.attribute("lineWidth").as_double() * scale;
            st.square = std::string(d.attribute("lineEnd").value()) == "SQUARE";
        } else if (const px::xml_node r = n.child("LineDescRef")) {
            if (const LineStyle* ref = shapes.line(r.attribute("id").value()))
                st = *ref;
        }
        if (st.widthMm <= 0) return {};

        Path64 spine;
        const std::string name = n.name();
        if (name == "Line") {
            spine.push_back(
                io::toClip(n.attribute("startX").as_double() * scale,
                           n.attribute("startY").as_double() * scale));
            spine.push_back(io::toClip(n.attribute("endX").as_double() * scale,
                                       n.attribute("endY").as_double() * scale));
        } else if (name == "Arc") {
            const double sx = n.attribute("startX").as_double() * scale;
            const double sy = n.attribute("startY").as_double() * scale;
            spine.push_back(io::toClip(sx, sy));
            io::arcAppend(spine, sx, sy,
                          n.attribute("endX").as_double() * scale,
                          n.attribute("endY").as_double() * scale,
                          n.attribute("centerX").as_double() * scale,
                          n.attribute("centerY").as_double() * scale,
                          n.attribute("clockwise").as_bool());
        } else {  // Polyline: PolyBegin + steps
            spine = polygonPath(n, scale);
        }
        if (spine.size() < 2) return {};

        ClipperOffset co;
        co.ArcTolerance(geom::kScale * 0.001);
        co.AddPath(spine, st.square ? JoinType::Miter : JoinType::Round,
                   st.square ? EndType::Square : EndType::Round);
        Paths64 out;
        co.Execute(st.widthMm * 0.5 * geom::kScale, out);
        return out;
    }

private:
    std::vector<std::string>& warnings_;
    std::map<std::string, std::pair<px::xml_node, double>> standard_, user_;
    std::map<std::string, LineStyle> lines_;
    std::map<std::string, Paths64> cache_;
    std::map<std::string, bool> warned_;
};

struct LayerDef {
    std::string name;
    std::string function;  // CONDUCTOR, SOLDERMASK, ...
    std::string side;      // TOP / BOTTOM / INTERNAL
    std::string spanFrom, spanTo;  // DRILL layers
};

bool isCopperFn(const std::string& f) {
    return f == "CONDUCTOR" || f == "SIGNAL" || f == "PLANE" || f == "MIXED" ||
           f == "CONDUCTIVE";
}

}  // namespace

bool isIpc2581(const std::string& path) {
    const std::string p = toLower(path);
    const bool xmlish = p.size() > 4 && (p.compare(p.size() - 4, 4, ".xml") == 0 ||
                                         p.compare(p.size() - 4, 4, ".cvg") == 0);
    if (!xmlish) return false;
    std::ifstream in(path, std::ios::binary);
    char head[4096] = {};
    in.read(head, sizeof head - 1);
    return std::strstr(head, "<IPC-2581") != nullptr;
}

geom::LayerArt importFile(const std::string& path) {
    geom::LayerArt art;
    art.sourcePath = path;

    px::xml_document doc;
    const px::xml_parse_result res = doc.load_file(path.c_str());
    if (!res)
        throw std::runtime_error(std::string("cannot parse XML: ") +
                                 res.description());
    const px::xml_node root = doc.child("IPC-2581");
    if (!root) throw std::runtime_error("not an IPC-2581 file: " + path);

    // ---- dictionaries ------------------------------------------------------
    Shapes shapes(art.warnings);
    const px::xml_node content = root.child("Content");
    if (const px::xml_node d = content.child("DictionaryLineDesc")) {
        const double scale = unitsScale(d.attribute("units").value());
        for (const px::xml_node& e : d.children("EntryLineDesc")) {
            const px::xml_node ld = e.child("LineDesc");
            LineStyle st;
            st.widthMm = ld.attribute("lineWidth").as_double() * scale;
            st.square = std::string(ld.attribute("lineEnd").value()) == "SQUARE";
            shapes.addLine(e.attribute("id").value(), st);
        }
    }
    if (const px::xml_node d = content.child("DictionaryStandard")) {
        const double scale = unitsScale(d.attribute("units").value());
        for (const px::xml_node& e : d.children("EntryStandard"))
            shapes.addStandard(e, scale);
    }
    if (const px::xml_node d = content.child("DictionaryUser")) {
        const double scale = unitsScale(d.attribute("units").value());
        for (const px::xml_node& e : d.children("EntryUser"))
            shapes.addUser(e, scale);
    }

    // ---- cad data ----------------------------------------------------------
    const px::xml_node ecad = root.child("Ecad");
    const double scale =
        unitsScale(ecad.child("CadHeader").attribute("units").value());
    const px::xml_node cad = ecad.child("CadData");
    if (!cad) throw std::runtime_error("IPC-2581 file has no CadData: " + path);

    std::vector<LayerDef> defs;
    for (const px::xml_node& l : cad.children("Layer")) {
        LayerDef d;
        d.name = l.attribute("name").value();
        d.function = l.attribute("layerFunction").value();
        d.side = l.attribute("side").value();
        if (const px::xml_node span = l.child("Span")) {
            d.spanFrom = span.attribute("fromLayer").value();
            d.spanTo = span.attribute("toLayer").value();
        }
        defs.push_back(std::move(d));
    }
    const auto layerDef = [&](const std::string& name) -> const LayerDef* {
        for (const LayerDef& d : defs)
            if (d.name == name) return &d;
        return nullptr;
    };

    // ---- stackup: real thicknesses, top-first sequence --------------------
    // Missing entries (or no stackup at all) fall back to the same derived
    // numbers the other importers use.
    std::map<std::string, double> thickOf;
    std::vector<std::string> stackOrder;
    for (const px::xml_node& su : cad.children("Stackup")) {
        for (const px::xml_node& g : su.children("StackupGroup")) {
            for (const px::xml_node& sl : g.children("StackupLayer")) {
                const std::string ref = sl.attribute("layerOrGroupRef").value();
                thickOf[ref] = sl.attribute("thickness").as_double() * scale;
                stackOrder.push_back(ref);
            }
        }
    }

    // ---- the step ----------------------------------------------------------
    const px::xml_node step = cad.child("Step");
    if (!step) throw std::runtime_error("IPC-2581 file has no Step: " + path);

    // Profile: Polygon + Cutouts, already the board area.
    if (const px::xml_node prof = step.child("Profile")) {
        art.outline = Shapes::contourPaths(prof, scale);
    }

    // Layer features grouped by layerRef (drill spans have their own refs).
    // A Set's polarity is honoured (CLEAR/NEGATIVE erase), though writers in
    // practice emit positive-only.
    struct Bucket {
        io::DarkClearAcc comp;
        std::map<int, Paths64> netDark;
        std::vector<std::pair<Paths64, bool>> holes;  // (paths, plated)
        int netless = 0;
    };
    std::map<std::string, Bucket> buckets;
    std::map<std::string, int> netIndex;
    const auto netFor = [&](const std::string& name) -> int {
        if (name.empty()) return -1;
        const auto [it, inserted] =
            netIndex.emplace(name, static_cast<int>(art.nets.size()));
        if (inserted) art.nets.push_back({name, 0.0, 0});
        return it->second;
    };

    // Recursive walk of a Set's geometry (Features/UserSpecial wrappers are
    // transparent). Returns the realized dark polygons while logging nets,
    // segments and snap points.
    int textSkipped = 0;
    std::function<void(const px::xml_node&, Bucket&, int, const LayerDef&,
                       bool)>
        walk = [&](const px::xml_node& parent, Bucket& b, int net,
                   const LayerDef& def, bool dark) {
            for (const px::xml_node& n : parent.children()) {
                const std::string name = n.name();
                Paths64 polys;
                if (name == "Pad") {
                    const px::xml_node loc = n.child("Location");
                    const double x = loc.attribute("x").as_double() * scale;
                    const double y = loc.attribute("y").as_double() * scale;
                    double rot = 0;
                    bool mirror = false;
                    if (const px::xml_node xf = n.child("Xform")) {
                        // IPC-2581 rotation runs the OPPOSITE way from
                        // ODB++'s (KiCad writes 90 there, 270 here for the
                        // same pad); negate so both formats land identically.
                        rot = -xf.attribute("rotation").as_double();
                        mirror = xf.attribute("mirror").as_bool();
                    }
                    std::string prim =
                        n.child("StandardPrimitiveRef").attribute("id").value();
                    if (prim.empty())
                        prim = n.child("UserPrimitiveRef").attribute("id").value();
                    polys = io::placed(shapes.get(prim), x, y, rot, mirror);
                    if (!polys.empty() && isCopperFn(def.function)) {
                        geom::LayerArt::NetPoint np;
                        np.pos[0] = x;
                        np.pos[1] = y;
                        // Marker on the pad's outward face; z rewritten to
                        // the real stack height after the stackup is read.
                        np.pos[2] = (def.side == "BOTTOM") ? -1.0 : 1.0;
                        np.net = net;
                        art.netPoints.push_back(np);
                    }
                } else if (name == "Line" || name == "Polyline" ||
                           name == "Arc") {
                    polys = Shapes::strokePaths(n, scale, shapes);
                    if (name == "Line" && net >= 0 &&
                        isCopperFn(def.function)) {
                        const double ax = n.attribute("startX").as_double() * scale;
                        const double ay = n.attribute("startY").as_double() * scale;
                        const double bx = n.attribute("endX").as_double() * scale;
                        const double by = n.attribute("endY").as_double() * scale;
                        art.netSegments.push_back({ax, ay, bx, by, net});
                        art.nets[net].routedMm += std::hypot(bx - ax, by - ay);
                    }
                } else if (name == "Contour") {
                    polys = Shapes::contourPaths(n, scale);
                    if (net >= 0 && isCopperFn(def.function))
                        art.nets[net].hasPlane = true;
                } else if (name == "Hole") {
                    const double d = n.attribute("diameter").as_double() * scale;
                    const double x = n.attribute("x").as_double() * scale;
                    const double y = n.attribute("y").as_double() * scale;
                    const std::string ps = n.attribute("platingStatus").value();
                    const bool plated = ps != "NONPLATED";
                    if (d > 0)
                        b.holes.push_back(
                            {Paths64{io::circlePath(x, y, d * 0.5)}, plated});
                    continue;
                } else if (name == "Features" || name == "UserSpecial" ||
                           name == "Set") {
                    walk(n, b, net, def, dark);
                    continue;
                } else if (name == "Text") {
                    ++textSkipped;
                    continue;
                } else {
                    continue;  // Xform/Location handled inside Pad; PinRef etc.
                }
                if (polys.empty()) continue;
                if (!dark) {
                    b.comp.clear(polys);
                    continue;
                }
                b.comp.dark(polys);
                Paths64& bucket = b.netDark[net];
                bucket.insert(bucket.end(), polys.begin(), polys.end());
            }
        };

    for (const px::xml_node& lf : step.children("LayerFeature")) {
        const std::string ref = lf.attribute("layerRef").value();
        const LayerDef* def = layerDef(ref);
        if (!def) continue;
        Bucket& b = buckets[ref];
        for (const px::xml_node& set : lf.children("Set")) {
            const int net = netFor(set.attribute("net").value());
            const std::string pol = set.attribute("polarity").value();
            const bool dark = pol != "CLEAR" && pol != "NEGATIVE";
            walk(set, b, net, *def, dark);
        }
    }

    // ---- stack the layers --------------------------------------------------
    std::vector<const LayerDef*> copper;
    for (const LayerDef& d : defs)
        if (isCopperFn(d.function) && buckets.count(d.name)) copper.push_back(&d);
    if (copper.empty())
        throw std::runtime_error("IPC-2581 file has no copper layers: " + path);

    // Z from the stackup when present; derived defaults otherwise.
    double total = 0.0;
    for (const std::string& ref : stackOrder) total += thickOf[ref];
    const bool haveStack = total > 0.1;
    double copperT = 0.035, maskT = 0.010;
    std::map<std::string, double> zOf;
    if (haveStack) {
        double zTop = total;
        for (const std::string& ref : stackOrder) {
            zTop -= thickOf[ref];
            zOf[ref] = zTop;  // bottom of that film
        }
        if (const LayerDef* first = copper.front(); thickOf.count(first->name))
            copperT = std::max(thickOf[first->name], 0.005);
        for (const LayerDef& d : defs)
            if (d.function == "SOLDERMASK" && thickOf.count(d.name) &&
                thickOf[d.name] > 0)
                maskT = thickOf[d.name];
        art.notes.push_back("IPC-2581 stackup: real thicknesses, board " +
                            std::to_string(total) + "mm");
    } else {
        total = 1.6;
        const int n = static_cast<int>(copper.size());
        const double dielT =
            n > 1 ? (total - n * copperT - 2 * maskT) / (n - 1) : 0.0;
        for (int i = 0; i < n; ++i)
            zOf[copper[i]->name] =
                total - maskT - i * (copperT + dielT) - copperT;
        art.warnings.push_back(
            "IPC-2581 file carries no stackup; using derived thicknesses");
    }
    art.thickness = total;
    art.copperThickness = copperT;
    art.maskThickness = maskT;
    art.silkThickness = 0.010;

    const auto buildLayer = [&](const LayerDef& d, LayerKind kind) {
        Bucket& b = buckets[d.name];
        geom::ArtLayer al;
        al.name = d.name;
        al.kind = kind;
        const bool top = d.side != "BOTTOM";
        if (kind == LayerKind::Copper) {
            al.thickness = copperT;
            al.z = zOf.count(d.name) ? zOf[d.name] : 0.0;
        } else if (kind == LayerKind::Soldermask) {
            al.thickness = maskT;
            al.z = top ? total - maskT : 0.0;
        } else {
            al.thickness = art.silkThickness;
            al.z = top ? total : -art.silkThickness;
        }
        const bool hadClears = b.comp.sawClear;
        al.art = b.comp.take();
        if (kind == LayerKind::Soldermask)
            al.openings = static_cast<int>(al.art.size());
        if (kind == LayerKind::Copper) {
            for (auto& [net, darks] : b.netDark) {
                geom::ArtLayer::NetRegion nr;
                nr.net = net;
                nr.paths = hadClears
                               ? Intersect(darks, al.art, FillRule::NonZero)
                               : Union(darks, FillRule::NonZero);
                if (!nr.paths.empty()) al.netArt.push_back(std::move(nr));
            }
        }
        art.layers.push_back(std::move(al));
    };

    for (const LayerDef* d : copper) buildLayer(*d, LayerKind::Copper);
    for (const LayerDef& d : defs) {
        if (!buckets.count(d.name)) continue;
        if (d.function == "SOLDERMASK") buildLayer(d, LayerKind::Soldermask);
        else if (d.function == "SILKSCREEN" || d.function == "LEGEND")
            buildLayer(d, LayerKind::Silkscreen);
    }

    // Snap points: the walk marked sides as +-1; resolve to real faces now
    // that the total thickness is known.
    for (geom::LayerArt::NetPoint& np : art.netPoints)
        np.pos[2] = np.pos[2] < 0 ? 0.0 : total;

    // ---- drills ------------------------------------------------------------
    const std::string firstCu = copper.front()->name;
    const std::string lastCu = copper.back()->name;
    for (const LayerDef& d : defs) {
        if (d.function != "DRILL" || !buckets.count(d.name)) continue;
        const bool fullSpan =
            d.spanFrom.empty() || d.spanTo.empty() ||
            (d.spanFrom == firstCu && d.spanTo == lastCu) ||
            (d.spanFrom == lastCu && d.spanTo == firstCu);
        for (auto& [paths, plated] : buckets[d.name].holes) {
            for (Path64& p : paths) {
                if (fullSpan) {
                    art.drills.push_back(p);
                    if (plated) art.barrels.push_back(p);
                } else {
                    geom::LayerArt::PartialBore bore;
                    bore.path = p;
                    bore.fromLayer = d.spanFrom;
                    bore.toLayer = d.spanTo;
                    art.partialBores.push_back(std::move(bore));
                }
            }
        }
    }
    geom::normalizeWinding(art.drills);
    geom::normalizeWinding(art.barrels);

    if (art.outline.empty()) {
        Paths64 all;
        for (const geom::ArtLayer& al : art.layers)
            if (al.kind == LayerKind::Copper)
                all.insert(all.end(), al.art.begin(), al.art.end());
        const Rect64 bb = GetBounds(all);
        if (bb.Width() <= 0)
            throw std::runtime_error(
                "IPC-2581 file has no profile and no copper: " + path);
        art.outline = {bb.AsPath()};
        art.warnings.push_back(
            "step has no profile; using the copper bounding box");
    }

    if (textSkipped > 0)
        art.notes.push_back(std::to_string(textSkipped) +
                            " text feature(s) not rendered");
    if (!art.nets.empty())
        art.notes.push_back("IPC-2581 netlist: " +
                            std::to_string(art.nets.size()) +
                            " nets (net highlighting and routed length "
                            "available)");
    art.notes.push_back("IPC-2581 step '" +
                        std::string(step.attribute("name").value()) + "', " +
                        std::to_string(art.layers.size()) + " layers");
    return art;
}

}  // namespace pcbview::ipc2581
