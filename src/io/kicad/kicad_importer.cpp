#include "io/kicad/kicad_importer.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <numbers>
#include <sstream>
#include <stdexcept>

#include "io/sexpr.h"

namespace pcbview::kicad {
namespace {

using sexpr::Node;

constexpr double kStitchTolerance = 0.001;  // mm

Vec2 readXY(const Node& node, size_t first = 1) {
    return Vec2{node.num(first), node.num(first + 1)};
}

LayerKind classifyLayer(std::string_view name) {
    if (name.size() > 3 && name.substr(name.size() - 3) == ".Cu") {
        return LayerKind::Copper;
    }
    if (name == "F.Mask" || name == "B.Mask") return LayerKind::Soldermask;
    if (name == "F.SilkS" || name == "B.SilkS") return LayerKind::Silkscreen;
    if (name == "F.Paste" || name == "B.Paste") return LayerKind::Paste;
    if (name == "Edge.Cuts") return LayerKind::EdgeCuts;
    return LayerKind::Other;
}

// Physical top-to-bottom rank for a copper layer.
//
// KiCad 10 ordinals are NOT physical order: F.Cu=0, B.Cu=2, In1.Cu=4, In2.Cu=6.
// Sorting by ordinal would place B.Cu second from the top and silently corrupt
// the stackup. Rank by name instead.
int copperStackRank(std::string_view name) {
    if (name == "F.Cu") return 0;
    if (name == "B.Cu") return 1'000'000;  // always last
    if (name.size() > 5 && name.substr(0, 2) == "In") {
        const std::string digits(name.substr(2, name.size() - 5));
        return 1 + std::atoi(digits.c_str());
    }
    return 999'999;
}

// Build the physical stackup: z = 0 at the bottom of the finished board,
// z = board.thickness at the top. Layer::z is the BOTTOM of each film.
//
// The dielectric height is derived, not guessed. (general (thickness ...))
// covers the whole finished board -- both mask films and every copper foil --
// so what remains for the dielectrics is:
//
//     (thickness - N*copper - 2*mask) / (N - 1)
//
// Verified against fab output: on cx4multicart_v3 that is
// (1.6 - 8*0.035 - 2*0.01) / 7 = 0.185714, and KiCad's own gerber job file
// reports 0.1857 for all seven dielectrics. This reproduces KiCad's arithmetic
// exactly, so an even split of the full thickness (the old placeholder) was
// wrong twice over: wrong spacing, and a board 0.07mm too thick.
// Place every film from an EXPLICIT (setup (stackup ...)) block.
//
// Returns false when the board has no usable block, leaving the caller to
// derive. Walks the entries top-down accumulating z, which is the only way to
// reproduce an asymmetric stack: a single derived dielectric height spreads
// the asymmetry evenly and puts every inner foil somewhere the fab will not.
//
// The block is authoritative for POSITION. `(general (thickness ...))` still
// names the finished thickness, and the two should agree -- when they do not,
// the stackup wins (it is the per-film truth) and the disagreement is warned
// about rather than silently reconciled.
bool buildLayerStackFromBlock(BoardModel& board) {
    if (board.stackup.empty()) return false;

    // Total height of everything INSIDE the finished board. Silkscreen carries
    // no thickness in KiCad's block and sits on top of the mask, so it is
    // excluded here exactly as it is from (general (thickness ...)).
    double total = 0.0;
    int copperSeen = 0;
    for (const auto& e : board.stackup) {
        if (!e.hasThickness) continue;
        if (e.type.find("Silk") != std::string::npos) continue;
        total += e.thickness;
        if (e.type == "copper") ++copperSeen;
    }
    if (copperSeen == 0 || total <= 0.0) return false;

    if (std::abs(total - board.thickness) > 0.005) {
        board.warnings.push_back(
            "stackup films sum to " + std::to_string(total) +
            " mm but (general (thickness)) says " +
            std::to_string(board.thickness) +
            " mm; using the stackup, which is the per-film truth");
    }
    board.thickness = total;

    // Top-down: z starts at the finished surface and walks down.
    double top = total;
    int stackIndex = 0;
    double firstCopper = -1.0, lastCopper = -1.0;
    for (const auto& e : board.stackup) {
        if (e.type.find("Silk") != std::string::npos) continue;
        if (!e.hasThickness) continue;
        const double bottom = top - e.thickness;

        // Entries name a real layer only for copper and the masks; dielectric
        // entries ("dielectric 1", core/prepreg) exist to consume height, and
        // consuming it here is precisely what places the foils around them.
        for (Layer& layer : board.layers) {
            if (layer.name != e.name) continue;
            layer.z = bottom;
            layer.thickness = e.thickness;
            if (layer.kind == LayerKind::Copper) layer.stackIndex = stackIndex;
        }
        if (e.type == "copper") {
            if (firstCopper < 0.0) firstCopper = bottom;
            lastCopper = bottom;
            ++stackIndex;
            board.copperThickness = e.thickness;
        } else if (e.type.find("Solder Mask") != std::string::npos ||
                   e.type.find("solder mask") != std::string::npos) {
            board.maskThickness = e.thickness;
        }
        top = bottom;
    }

    // A representative dielectric height for the few consumers that still want
    // one scalar. Derived from the copper span so it stays meaningful on an
    // asymmetric stack rather than reporting whichever dielectric came last.
    if (stackIndex > 1 && firstCopper > lastCopper) {
        const double span = firstCopper - lastCopper;
        board.dielectricThickness =
            (span - (stackIndex - 1) * board.copperThickness) /
            static_cast<double>(stackIndex - 1);
    }

    // Silkscreen sits ON the mask, outside the finished thickness.
    for (Layer& layer : board.layers) {
        if (layer.kind != LayerKind::Silkscreen) continue;
        layer.thickness = board.silkThickness;
        layer.z = (layer.name.rfind("F.", 0) == 0) ? board.thickness
                                                   : -board.silkThickness;
    }
    return true;
}

void buildLayerStack(BoardModel& board) {
    // An explicit block beats any derivation. This is the whole point: the
    // Gerber path already reads real thicknesses from the .gbrjob, so until
    // now importing a board's gerbers was MORE stackup-accurate than importing
    // its .kicad_pcb.
    if (buildLayerStackFromBlock(board)) return;

    std::vector<int> copper = board.copperLayers();
    std::sort(copper.begin(), copper.end(), [&](int a, int b) {
        return copperStackRank(board.layers[a].name) <
               copperStackRank(board.layers[b].name);
    });

    const size_t count = copper.size();
    if (count == 0) return;

    const double tc = board.copperThickness;
    const double tm = board.maskThickness;

    board.dielectricThickness =
        (count > 1)
            ? (board.thickness - static_cast<double>(count) * tc - 2.0 * tm) /
                  static_cast<double>(count - 1)
            : 0.0;

    if (board.dielectricThickness < 0.0) {
        board.warnings.push_back(
            "board thickness is too small for the copper and mask films; "
            "dielectric height came out negative");
        board.dielectricThickness = 0.0;
    }

    const double pitch = tc + board.dielectricThickness;
    for (size_t i = 0; i < count; ++i) {
        Layer& layer = board.layers[copper[i]];
        layer.stackIndex = static_cast<int>(i);
        layer.thickness = tc;
        // F.Cu's top face sits exactly one mask film below the finished surface.
        layer.z = board.thickness - tm - static_cast<double>(i) * pitch - tc;
    }

    for (Layer& layer : board.layers) {
        if (layer.kind == LayerKind::Soldermask) {
            layer.thickness = tm;
            layer.z = (layer.name == "F.Mask") ? board.thickness - tm : 0.0;
        } else if (layer.kind == LayerKind::Silkscreen) {
            // Ink sits ON the mask, i.e. outside the finished thickness. KiCad's
            // job file lists silkscreen in the stackup with no thickness at all,
            // so this is a rendering nicety rather than a fab dimension.
            layer.thickness = board.silkThickness;
            layer.z = (layer.name == "F.SilkS") ? board.thickness
                                                : -board.silkThickness;
        } else if (layer.kind != LayerKind::Copper) {
            // Silkscreen/paste/etc: park them on the outer surfaces. Not
            // rendered yet; this only keeps z meaningful if something asks.
            if (layer.name.rfind("F.", 0) == 0) layer.z = board.thickness;
            else if (layer.name.rfind("B.", 0) == 0) layer.z = 0.0;
        }
    }
}

void readLayers(const Node& root, BoardModel& board) {
    const Node* layers = root.child("layers");
    if (!layers) throw std::runtime_error("kicad_pcb has no (layers ...) block");

    for (size_t i = 1; i < layers->kids.size(); ++i) {
        const Node& entry = layers->kids[i];
        if (!entry.isList || entry.kids.empty()) continue;

        Layer layer;
        layer.kicadId = static_cast<int>(entry.num(0, -1));
        layer.name = std::string(entry.str(1));
        layer.kind = classifyLayer(layer.name);
        board.layers.push_back(std::move(layer));
    }
    buildLayerStack(board);
}

// Rotate a footprint-local point into board space.
//
// KiCad stores orientation in degrees; the file's Y axis points down. The sign
// convention here is checked by validateNetGeometry(), which asserts that track
// endpoints land on pads of the same net -- a wrong sign desynchronises rotated
// footprints from their tracks and shows up immediately as orphaned endpoints.
// Rotate a footprint-local point into board space.
//
// Deliberately takes no mirror parameter. KiCad stores bottom-side pad
// coordinates already flipped, so mirroring them again breaks them: measured on
// cx4multicart_v3, an added X mirror drops bottom-pad connectivity from 52/52 to
// 6/52 and orphans 47 track endpoints. `bottom` matters for layer assignment and
// component Z, not for this transform.
Vec2 applyTransform(Vec2 local, Vec2 origin, double degrees) {
    // Negative: KiCad's stored angle is counter-clockwise as displayed, but the
    // file's Y axis points down, so in raw file coordinates the rotation is
    // clockwise. Measured, not assumed -- the positive convention orphans 58 of
    // 2512 track endpoints; this one orphans zero.
    const double rad = -degrees * std::numbers::pi / 180.0;
    const double c = std::cos(rad);
    const double s = std::sin(rad);
    return Vec2{origin.x + local.x * c - local.y * s,
                origin.y + local.x * s + local.y * c};
}

PadShape parsePadShape(std::string_view s, BoardModel& board) {
    if (s == "circle") return PadShape::Circle;
    if (s == "rect") return PadShape::Rect;
    if (s == "roundrect") return PadShape::RoundRect;
    if (s == "oval") return PadShape::Oval;
    if (s == "custom") return PadShape::Custom;
    if (s == "trapezoid") {
        board.warnings.push_back(
            "pad shape 'trapezoid' not implemented; treated as rect");
        return PadShape::Rect;
    }
    board.warnings.push_back("unknown pad shape '" + std::string(s) +
                             "'; treated as rect");
    return PadShape::Rect;
}

PadType parsePadType(std::string_view s) {
    if (s == "smd") return PadType::Smd;
    if (s == "thru_hole") return PadType::ThruHole;
    if (s == "np_thru_hole") return PadType::NpThruHole;
    return PadType::Connect;
}

void readPad(const Node& padNode, const Component& comp, int componentIndex,
             BoardModel& board) {
    Pad pad;
    pad.component = componentIndex;
    pad.number = std::string(padNode.str(1));
    pad.type = parsePadType(padNode.str(2));
    pad.shape = parsePadShape(padNode.str(3), board);

    const Node* at = padNode.child("at");
    const Vec2 local = at ? readXY(*at) : Vec2{};

    pad.at = applyTransform(local, comp.at, comp.rotation);

    // The pad's POSITION is footprint-local and must be transformed. Its
    // ROTATION is not: KiCad stores pad rotation as an absolute board angle that
    // already includes the footprint's orientation. Adding comp.rotation here
    // double-counts it.
    //
    // Measured on cx4multicart_v3 -- stored pad rot always equals footprint rot
    // when the pad carries no extra rotation of its own:
    //   U1  footprint -90 -> pad rot 270  (== -90)
    //   C2, R2, U3  footprint 90 -> pad rot 90
    //   C6, U2  footprint 0 -> pad rot absent (== 0)
    //
    // Getting this wrong is invisible to every positional check: rotating a
    // pad's shape does not move its centre, so connectivity still passes at zero
    // orphans. It shows up only as geometry -- on U1 (TSOP-56, 0.5mm pitch) the
    // double-count laid each 1.575mm pad ALONG the pitch axis, overlapping its
    // neighbours so all 28 unioned into one solid bar.
    pad.rotation = at ? at->num(3, 0.0) : 0.0;

    if (const Node* size = padNode.child("size")) pad.size = readXY(*size);
    pad.roundrectRatio = padNode.childNum("roundrect_rratio", 0.0);
    pad.net = std::string(padNode.childStr("net", ""));

    if (const Node* drill = padNode.child("drill")) {
        // (drill 0.8) or (drill oval 0.8 1.2) -- oval drills take the larger
        // axis for now; proper slot geometry is a phase 3 concern.
        if (drill->str(1) == "oval") {
            pad.drill = std::max(drill->num(2), drill->num(3));
            board.warnings.push_back("oval drill on pad " + pad.number +
                                     " approximated as round");
        } else {
            pad.drill = drill->num(1);
        }
        if (const Node* offset = drill->child("offset")) {
            pad.drillOffset = readXY(*offset);
        }
    }

    if (const Node* layers = padNode.child("layers")) {
        for (size_t i = 1; i < layers->kids.size(); ++i) {
            const std::string_view name = layers->kids[i].atom;

            // Wildcards: "*.Cu" is every copper layer, "*.Mask" both masks.
            if (name == "*.Cu") {
                for (int idx : board.copperLayers()) pad.layers.push_back(idx);
                continue;
            }
            if (name == "*.Mask") {
                for (int idx : {board.layerIndex("F.Mask"), board.layerIndex("B.Mask")}) {
                    if (idx >= 0) pad.maskLayers.push_back(idx);
                }
                continue;
            }

            const int idx = board.layerIndex(name);
            if (idx < 0) continue;

            // Copper and mask are kept apart; paste and everything else are not
            // geometry we render, so they are dropped deliberately.
            if (board.layers[idx].kind == LayerKind::Copper) {
                pad.layers.push_back(idx);
            } else if (board.layers[idx].kind == LayerKind::Soldermask) {
                pad.maskLayers.push_back(idx);
            }
        }
    }

    if (pad.drill > 0.0) {
        Drill drill;
        drill.at = Vec2{pad.at.x + pad.drillOffset.x, pad.at.y + pad.drillOffset.y};
        drill.diameter = pad.drill;
        drill.plated = (pad.type != PadType::NpThruHole);
        board.drills.push_back(drill);
    }

    board.pads.push_back(std::move(pad));
}

// Read gr_*/fp_* drawn shapes.
//
// `comp` is null for top-level gr_* (already board-space); for fp_* inside a
// footprint it supplies the transform, exactly as for pads. Everything lands in
// board space so the tessellator never has to care where a shape came from.
void readGraphics(const Node& parent, const Component* comp, BoardModel& board) {
    const auto place = [&](Vec2 p) {
        return comp ? applyTransform(p, comp->at, comp->rotation) : p;
    };

    struct Spec {
        const char* name;
        GraphicKind kind;
    };
    const Spec specs[] = {
        {comp ? "fp_line" : "gr_line", GraphicKind::Segment},
        {comp ? "fp_circle" : "gr_circle", GraphicKind::Circle},
        {comp ? "fp_rect" : "gr_rect", GraphicKind::Rect},
        {comp ? "fp_poly" : "gr_poly", GraphicKind::Polygon},
        {comp ? "fp_arc" : "gr_arc", GraphicKind::Arc},
    };

    for (const Spec& spec : specs) {
        for (const Node* node : parent.childList(spec.name)) {
            const int layer = board.layerIndex(node->childStr("layer", ""));
            if (layer < 0) continue;
            // Edge.Cuts is stitched separately into the board outline; copper
            // graphics are not something these boards use.
            if (board.layers[layer].kind == LayerKind::EdgeCuts) continue;

            Graphic g;
            g.kind = spec.kind;
            g.layer = layer;
            if (const Node* stroke = node->child("stroke")) {
                g.width = stroke->childNum("width", 0.0);
            } else {
                g.width = node->childNum("width", 0.0);  // pre-7.0 spelling
            }
            const std::string_view fill = node->childStr("fill", "no");
            g.filled = (fill == "yes" || fill == "solid");

            if (spec.kind == GraphicKind::Polygon) {
                const Node* pts = node->child("pts");
                if (!pts) continue;
                for (const Node* xy : pts->childList("xy")) {
                    g.points.push_back(place(readXY(*xy)));
                }
                if (g.points.size() < 3) continue;
            } else if (spec.kind == GraphicKind::Circle) {
                const Node* c = node->child("center");
                const Node* e = node->child("end");
                if (!c || !e) continue;
                g.start = place(readXY(*c));
                g.end = place(readXY(*e));
            } else if (spec.kind == GraphicKind::Arc) {
                const Node* s = node->child("start");
                const Node* m = node->child("mid");
                const Node* e = node->child("end");
                if (!s || !m || !e) continue;
                g.start = place(readXY(*s));
                g.mid = place(readXY(*m));
                g.end = place(readXY(*e));
            } else if (spec.kind == GraphicKind::Rect) {
                const Node* s = node->child("start");
                const Node* e = node->child("end");
                if (!s || !e) continue;
                // Store as a polygon, not two corners: inside a rotated
                // footprint the corners rotate but "two opposite corners" can
                // only describe an axis-aligned box, so the shape would come out
                // wrong. Expanding here, where the transform is known, keeps the
                // tessellator honest.
                const Vec2 a = readXY(*s);
                const Vec2 b = readXY(*e);
                g.kind = GraphicKind::Polygon;
                g.points = {place(Vec2{a.x, a.y}), place(Vec2{b.x, a.y}),
                            place(Vec2{b.x, b.y}), place(Vec2{a.x, b.y})};
            } else {
                const Node* s = node->child("start");
                const Node* e = node->child("end");
                if (!s || !e) continue;
                g.start = place(readXY(*s));
                g.end = place(readXY(*e));
            }
            board.graphics.push_back(std::move(g));
        }
    }
}

// Read a text node -- a footprint `property`/`fp_text`, or a top-level `gr_text`.
// `contentIndex` is where the string lives: property is (property "Key" "Value"),
// gr_text is (gr_text "Value").
void readText(const Node& node, size_t contentIndex, const Component* comp,
              BoardModel& board) {
    // KiCad marks unplotted text hidden; it is not on the physical board.
    if (node.child("hide") && node.child("hide")->str(1) == "yes") return;
    if (node.hasAtom("hide")) return;  // pre-8.0 spelling: a bare (hide) atom

    const int layer = board.layerIndex(node.childStr("layer", ""));
    if (layer < 0) return;

    TextItem t;
    t.content = std::string(node.str(contentIndex));
    if (t.content.empty()) return;
    t.layer = layer;

    if (const Node* at = node.child("at")) {
        const Vec2 local = readXY(*at);
        // Position is footprint-local; rotation is absolute. Same split as pads.
        t.at = comp ? applyTransform(local, comp->at, comp->rotation) : local;
        t.rotation = at->num(3, 0.0);
    }
    if (comp) t.mirror = comp->bottom;

    if (const Node* effects = node.child("effects")) {
        if (const Node* font = effects->child("font")) {
            if (const Node* size = font->child("size")) {
                // KiCad stores (size height width) -- height first.
                t.size = Vec2{size->num(2, 1.0), size->num(1, 1.0)};
            }
            t.thickness = font->childNum("thickness", 0.15);
            t.italic = font->hasAtom("italic") ||
                       (font->child("italic") &&
                        font->child("italic")->str(1) == "yes");
        }
        if (const Node* justify = effects->child("justify")) {
            // `mirror` here flips the text independently of the board side.
            if (justify->hasAtom("mirror")) t.mirror = !t.mirror;
        }
    }
    board.texts.push_back(std::move(t));
}

void readFootprint(const Node& fp, BoardModel& board) {
    Component comp;
    comp.footprint = std::string(fp.str(1));

    if (const Node* at = fp.child("at")) {
        comp.at = readXY(*at);
        comp.rotation = at->num(3, 0.0);
    }
    comp.bottom = (fp.childStr("layer", "F.Cu") == "B.Cu");

    for (const Node* prop : fp.childList("property")) {
        const std::string_view key = prop->str(1);
        if (key == "Reference") comp.reference = std::string(prop->str(2));
        else if (key == "Value") comp.value = std::string(prop->str(2));
    }
    for (const Node* prop : fp.childList("property")) readText(*prop, 2, &comp, board);
    for (const Node* txt : fp.childList("fp_text")) readText(*txt, 2, &comp, board);

    const int index = static_cast<int>(board.components.size());
    for (const Node* pad : fp.childList("pad")) readPad(*pad, comp, index, board);
    readGraphics(fp, &comp, board);
    board.components.push_back(std::move(comp));
}

void readZone(const Node& zoneNode, BoardModel& board) {
    const std::string net(zoneNode.childStr("net_name", ""));
    const std::string name(zoneNode.childStr("name", ""));

    // Each filled_polygon carries its own layer, which matters for zones spanning
    // several layers. Group them so one ZoneFill exists per layer.
    std::map<int, ZoneFill> byLayer;

    for (const Node* filled : zoneNode.childList("filled_polygon")) {
        const int layer = board.layerIndex(filled->childStr("layer", ""));
        if (layer < 0) {
            board.warnings.push_back("zone fill on unknown layer '" +
                                     std::string(filled->childStr("layer", "?")) +
                                     "'");
            continue;
        }

        const Node* pts = filled->child("pts");
        if (!pts) continue;

        Polygon poly;
        for (const Node* xy : pts->childList("xy")) {
            poly.outer.push_back(readXY(*xy));
        }
        if (poly.outer.size() < 3) continue;

        ZoneFill& fill = byLayer[layer];
        fill.layer = layer;
        fill.net = net.empty() ? std::string(zoneNode.childStr("net", "")) : net;
        fill.name = name;
        fill.polygons.push_back(std::move(poly));
    }

    for (auto& [layer, fill] : byLayer) board.zones.push_back(std::move(fill));
}

// --- Board outline -------------------------------------------------------
//
// Edge.Cuts arrives as loose primitives that must be stitched into closed
// loops. This board uses gr_line, gr_rect and gr_circle -- no arcs.

struct Seg {
    Vec2 a;
    Vec2 b;
    bool used = false;
};

bool nearly(Vec2 p, Vec2 q) {
    return std::abs(p.x - q.x) < kStitchTolerance &&
           std::abs(p.y - q.y) < kStitchTolerance;
}

std::vector<Vec2> circlePoints(Vec2 center, double radius, int segments = 64) {
    std::vector<Vec2> pts;
    pts.reserve(segments);
    for (int i = 0; i < segments; ++i) {
        const double t = 2.0 * std::numbers::pi * i / segments;
        pts.push_back(Vec2{center.x + radius * std::cos(t),
                           center.y + radius * std::sin(t)});
    }
    return pts;
}

void stitchOutline(std::vector<Seg>& segs, BoardModel& board) {
    for (size_t i = 0; i < segs.size(); ++i) {
        if (segs[i].used) continue;

        segs[i].used = true;
        Polygon poly;
        poly.outer.push_back(segs[i].a);
        Vec2 cursor = segs[i].b;

        for (;;) {
            if (nearly(cursor, poly.outer.front())) break;  // loop closed

            bool advanced = false;
            for (size_t j = 0; j < segs.size(); ++j) {
                if (segs[j].used) continue;
                if (nearly(segs[j].a, cursor)) {
                    poly.outer.push_back(cursor);
                    cursor = segs[j].b;
                } else if (nearly(segs[j].b, cursor)) {
                    poly.outer.push_back(cursor);
                    cursor = segs[j].a;
                } else {
                    continue;
                }
                segs[j].used = true;
                advanced = true;
                break;
            }
            if (!advanced) {
                board.warnings.push_back(
                    "Edge.Cuts outline does not close; open chain of " +
                    std::to_string(poly.outer.size()) + " points dropped");
                poly.outer.clear();
                break;
            }
        }

        if (poly.outer.size() >= 3) board.outline.push_back(std::move(poly));
    }
}

void readOutline(const Node& root, BoardModel& board) {
    std::vector<Seg> segs;

    for (const Node* line : root.childList("gr_line")) {
        if (line->childStr("layer", "") != "Edge.Cuts") continue;
        const Node* start = line->child("start");
        const Node* end = line->child("end");
        if (start && end) segs.push_back(Seg{readXY(*start), readXY(*end)});
    }

    for (const Node* rect : root.childList("gr_rect")) {
        if (rect->childStr("layer", "") != "Edge.Cuts") continue;
        const Node* start = rect->child("start");
        const Node* end = rect->child("end");
        if (!start || !end) continue;

        const Vec2 a = readXY(*start);
        const Vec2 b = readXY(*end);
        Polygon poly;
        poly.outer = {Vec2{a.x, a.y}, Vec2{b.x, a.y}, Vec2{b.x, b.y},
                      Vec2{a.x, b.y}};
        board.outline.push_back(std::move(poly));
    }

    for (const Node* circle : root.childList("gr_circle")) {
        if (circle->childStr("layer", "") != "Edge.Cuts") continue;
        const Node* center = circle->child("center");
        const Node* end = circle->child("end");
        if (!center || !end) continue;

        const Vec2 c = readXY(*center);
        const Vec2 e = readXY(*end);
        const double radius = std::hypot(e.x - c.x, e.y - c.y);

        Polygon poly;
        poly.outer = circlePoints(c, radius);
        board.outline.push_back(std::move(poly));
    }

    stitchOutline(segs, board);
}

}  // namespace

BoardModel importPcb(const std::string& path) {
    sexpr::Document doc = sexpr::parseFile(path);
    const Node& root = doc.root;

    if (root.head() != "kicad_pcb") {
        throw std::runtime_error("not a kicad_pcb file: " + path);
    }

    BoardModel board;
    board.sourcePath = path;

    if (const Node* general = root.child("general")) {
        board.thickness = general->childNum("thickness", 1.6);
    }
    if (const Node* setup = root.child("setup")) {
        board.padToMaskClearance = setup->childNum("pad_to_mask_clearance", 0.0);
        // Note: (setup (tenting ...)) needs no handling. Tenting is expressed by
        // vias simply carrying no mask layer, so a tented via contributes no
        // opening and the mask closes over it for free.

        // The explicit stackup, when present. Read VERBATIM and in order --
        // KiCad lists it top-down, and the order is what places every film.
        if (const Node* stack = setup->child("stackup")) {
            for (const Node* l : stack->childList("layer")) {
                BoardModel::StackupEntry e;
                e.name = std::string(l->str(1));
                e.type = std::string(l->childStr("type", ""));
                if (const Node* t = l->child("thickness")) {
                    e.thickness = t->num(1, 0.0);
                    e.hasThickness = true;
                }
                board.stackup.push_back(std::move(e));
            }
            board.copperFinish = std::string(stack->childStr("copper_finish", ""));
        }
    }

    readLayers(root, board);

    // Curved copper. KiCad 6+ stores a curved track as `(arc (start) (mid)
    // (end) ...)`, a THREE-POINT arc -- not as a segment. Reading only
    // `segment` dropped that copper from the board entirely: missing from the
    // render, from the net length, and from the measure graph. Chording it
    // into ordinary tracks fixes all three at once, because everything
    // downstream already handles straight segments.
    for (const Node* arc : root.childList("arc")) {
        const Node* sN = arc->child("start");
        const Node* mN = arc->child("mid");
        const Node* eN = arc->child("end");
        if (!sN || !mN || !eN) continue;
        const auto s0 = readXY(*sN), m0 = readXY(*mN), e0 = readXY(*eN);
        const int layer = board.layerIndex(arc->childStr("layer", ""));
        if (layer < 0) {
            board.warnings.push_back(
                "arc track on unknown layer '" +
                std::string(arc->childStr("layer", "?")) + "'");
            continue;
        }
        const double width = arc->childNum("width", 0.0);
        const std::string net(arc->childStr("net", ""));

        // Circumcentre of the three points. Collinear (or duplicate) points
        // have no circle -- fall back to a straight track rather than
        // dividing by ~0 and scattering geometry across the board.
        const double ax = s0.x, ay = s0.y, bx = m0.x, by = m0.y;
        const double cx2 = e0.x, cy2 = e0.y;
        const double d = 2.0 * (ax * (by - cy2) + bx * (cy2 - ay) +
                                cx2 * (ay - by));
        const auto pushTrack = [&](double x0, double y0, double x1, double y1) {
            Track t;
            t.start = {x0, y0};
            t.end = {x1, y1};
            t.width = width;
            t.layer = layer;
            t.net = net;
            board.tracks.push_back(std::move(t));
        };
        if (std::abs(d) < 1e-12) {
            pushTrack(s0.x, s0.y, e0.x, e0.y);
            continue;
        }
        const double a2 = ax * ax + ay * ay, b2 = bx * bx + by * by;
        const double c2 = cx2 * cx2 + cy2 * cy2;
        const double ox = (a2 * (by - cy2) + b2 * (cy2 - ay) + c2 * (ay - by)) / d;
        const double oy = (a2 * (cx2 - bx) + b2 * (ax - cx2) + c2 * (bx - ax)) / d;
        const double r = std::hypot(ax - ox, ay - oy);

        double a0 = std::atan2(ay - oy, ax - ox);
        double am = std::atan2(by - oy, bx - ox);
        double a1 = std::atan2(cy2 - oy, cx2 - ox);
        // Direction is whichever sweep passes THROUGH the mid point -- the
        // only thing that distinguishes the minor arc from the major one.
        const double twoPi = 2.0 * std::numbers::pi;
        const auto norm = [twoPi](double v) {
            while (v < 0) v += twoPi;
            while (v >= twoPi) v -= twoPi;
            return v;
        };
        const bool ccw = norm(am - a0) < norm(a1 - a0);
        double sweep = ccw ? norm(a1 - a0) : -norm(a0 - a1);
        constexpr int kArcSegments = 48;  // per full circle, as in the Gerber path
        const int segs = std::max(
            2, static_cast<int>(std::ceil(std::abs(sweep) / twoPi * kArcSegments)));
        double px = ax, py = ay;
        for (int k = 1; k <= segs; ++k) {
            const double t = a0 + sweep * k / segs;
            const double qx = ox + r * std::cos(t), qy = oy + r * std::sin(t);
            pushTrack(px, py, qx, qy);
            px = qx; py = qy;
        }
    }

    for (const Node* seg : root.childList("segment")) {
        Track track;
        if (const Node* start = seg->child("start")) track.start = readXY(*start);
        if (const Node* end = seg->child("end")) track.end = readXY(*end);
        track.width = seg->childNum("width", 0.0);
        track.layer = board.layerIndex(seg->childStr("layer", ""));
        track.net = std::string(seg->childStr("net", ""));

        if (track.layer < 0) {
            board.warnings.push_back("track on unknown layer '" +
                                     std::string(seg->childStr("layer", "?")) + "'");
            continue;
        }
        board.tracks.push_back(std::move(track));
    }

    // Deepest copper stackIndex; a via reaching both 0 and this spans the board.
    int lastCopperStack = 0;
    for (int idx : board.copperLayers()) {
        lastCopperStack = std::max(lastCopperStack, board.layers[idx].stackIndex);
    }

    for (const Node* viaNode : root.childList("via")) {
        Via via;
        if (const Node* at = viaNode->child("at")) via.at = readXY(*at);
        via.size = viaNode->childNum("size", 0.0);
        via.drill = viaNode->childNum("drill", 0.0);
        via.net = std::string(viaNode->childStr("net", ""));

        if (const Node* layers = viaNode->child("layers")) {
            via.fromLayer = board.layerIndex(layers->str(1));
            via.toLayer = board.layerIndex(layers->str(2));
        }
        board.vias.push_back(via);

        // A via only breaks the outer faces if it spans the whole stack. A real
        // blind/buried via is drilled before lamination and leaves them intact.
        //
        // Judged by LAYER SPAN, never by the `(via buried)` type token. The two
        // disagree and layers are the truth: on cx4multicart_v3 all 121 vias
        // tagged `buried` span F.Cu..B.Cu, and KiCad's own drill program drills
        // every one of them -- 217 holes, which is what gets manufactured. KiCad's
        // 3D viewer keys off the type token instead and hides 121 holes that the
        // fab will physically drill. We follow the drill file.
        const bool spansOuterFaces =
            via.fromLayer >= 0 && via.toLayer >= 0 &&
            std::min(board.layers[via.fromLayer].stackIndex,
                     board.layers[via.toLayer].stackIndex) == 0 &&
            std::max(board.layers[via.fromLayer].stackIndex,
                     board.layers[via.toLayer].stackIndex) == lastCopperStack;

        if (via.drill > 0.0 && spansOuterFaces) {
            board.drills.push_back(Drill{via.at, via.drill, true});
        }
    }

    for (const Node* fp : root.childList("footprint")) readFootprint(*fp, board);
    for (const Node* zone : root.childList("zone")) readZone(*zone, board);
    readGraphics(root, nullptr, board);
    for (const Node* txt : root.childList("gr_text")) readText(*txt, 1, nullptr, board);
    readOutline(root, board);

    return board;
}

}  // namespace pcbview::kicad
