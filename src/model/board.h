#pragma once

// BoardModel: the format-neutral intermediate representation.
//
// Both the KiCad and Gerber importers converge here; renderers only ever see
// this. All units are millimetres. Coordinates are as authored (KiCad's Y axis
// points down); the Y flip happens once, at tessellation.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pcbview {

struct Vec2 {
    double x = 0.0;
    double y = 0.0;
};

// A simple polygon with optional holes. Winding is not guaranteed by importers;
// the geometry stage normalises it.
struct Polygon {
    std::vector<Vec2> outer;
    std::vector<std::vector<Vec2>> holes;
};

enum class LayerKind { Copper, Soldermask, Silkscreen, Paste, EdgeCuts, Other };

struct Layer {
    int kicadId = -1;      // ordinal from the file; NOT physical stack order
    std::string name;      // "F.Cu", "In1.Cu", ...
    LayerKind kind = LayerKind::Other;

    // Physical placement, filled in by the importer from board thickness.
    // stackIndex is the real top-to-bottom order.
    int stackIndex = -1;
    double z = 0.0;
    double thickness = 0.0;
};

struct Track {
    Vec2 start;
    Vec2 end;
    double width = 0.0;
    int layer = -1;  // index into BoardModel::layers
    std::string net;
};

struct Via {
    Vec2 at;
    double size = 0.0;   // outer copper diameter
    double drill = 0.0;
    int fromLayer = -1;
    int toLayer = -1;
    std::string net;
};

enum class PadShape { Circle, Rect, RoundRect, Oval, Custom };
enum class PadType { Smd, ThruHole, NpThruHole, Connect };

struct Pad {
    PadType type = PadType::Smd;
    PadShape shape = PadShape::Circle;

    Vec2 at;             // absolute, footprint transform already applied
    double rotation = 0.0;  // degrees
    Vec2 size;
    double roundrectRatio = 0.0;

    double drill = 0.0;  // 0 when none
    Vec2 drillOffset;

    std::vector<int> layers;  // copper layer indices this pad appears on

    // Soldermask layers this pad opens. Kept separate from `layers` so the
    // copper path never accidentally iterates mask layers. A pad carrying
    // F.Mask punches an opening in the front mask; a tented via carries none,
    // which is exactly how KiCad decides what to cover.
    std::vector<int> maskLayers;

    std::string net;
    std::string number;
    int component = -1;  // index into BoardModel::components
};

// Copper pour, already filled by the authoring tool. KiCad computes these for
// us (thermal reliefs and all), so we never run a pour algorithm.
struct ZoneFill {
    int layer = -1;
    std::string net;
    std::string name;
    std::vector<Polygon> polygons;
};

struct Drill {
    Vec2 at;
    double diameter = 0.0;
    bool plated = true;
};

struct Component {
    std::string footprint;
    std::string reference;
    std::string value;
    Vec2 at;
    double rotation = 0.0;  // degrees
    bool bottom = false;
};

// A drawn shape on a non-copper layer -- silkscreen, courtyard, fab, etc.
// Footprint-local coordinates are already transformed into board space by the
// importer, exactly as pads are.
enum class GraphicKind { Segment, Circle, Rect, Polygon, Arc };

struct Graphic {
    GraphicKind kind = GraphicKind::Segment;
    int layer = -1;
    double width = 0.0;   // stroke width; 0 means no outline
    bool filled = false;

    // Segment: two endpoints.
    // Circle:  start = centre, end = a point on the rim.
    // Rect:    opposite corners.
    // Arc:     start / end, with `mid` on the arc.
    Vec2 start;
    Vec2 end;
    Vec2 mid;

    std::vector<Vec2> points;  // Polygon
};

// A text item to be rendered as stroked glyphs.
//
// Like pads, POSITION is footprint-local (already transformed here) while
// ROTATION is absolute and already normalised by KiCad into a readable range --
// do not add the footprint's rotation to it. Verified: U1 sits at -90 but its
// reference stores +90, because KiCad normalises text so it never renders upside
// down. Everything else on this board has prop rot == footprint rot.
struct TextItem {
    std::string content;
    int layer = -1;
    Vec2 at;
    double rotation = 0.0;  // degrees, absolute
    Vec2 size{1.0, 1.0};    // em size in mm
    double thickness = 0.15;
    bool mirror = false;  // bottom-side text reads correctly from below
    bool italic = false;
};

struct BoardModel {
    std::string sourcePath;

    // Finished-board thickness, from (general (thickness ...)). This INCLUDES
    // both soldermask films and every copper foil -- it is not the bare core.
    double thickness = 1.6;

    // Physical stackup. These are fabrication facts, not render options, which
    // is why they live on the board rather than in TessellateOptions.
    // KiCad defaults; dielectric is derived in buildLayerStack().
    double copperThickness = 0.035;
    double maskThickness = 0.010;

    // The explicit stackup, when the board carries `(setup (stackup ...))`.
    // KiCad lists it TOP-DOWN and gives each film its own thickness, so an
    // asymmetric stack (thin prepreg between signal layers, thick core in the
    // middle) is only reproducible from this block -- deriving one dielectric
    // height for the whole board spreads that asymmetry evenly and puts every
    // inner foil in the wrong place. Empty when the board has no block, in
    // which case buildLayerStack derives as before.
    struct StackupEntry {
        std::string name;   // "F.Cu", "dielectric 1", "F.Mask", "F.SilkS"
        std::string type;   // "copper", "core", "prepreg", "Top Solder Mask"
        double thickness = 0.0;
        bool hasThickness = false;
    };
    std::vector<StackupEntry> stackup;

    // `(copper_finish "ENIG")` and friends. Drives how exposed copper is
    // shaded: bare copper is what an unfinished board looks like, and almost
    // no real board ships that way.
    std::string copperFinish;
    double dielectricThickness = 0.0;
    // Silkscreen ink sits ON the mask, outside the finished thickness -- KiCad's
    // gerber job file lists it in the stackup with no thickness at all.
    double silkThickness = 0.010;

    // How far a mask opening is expanded beyond the pad it exposes.
    // From (setup (pad_to_mask_clearance ...)); zero on many boards.
    double padToMaskClearance = 0.0;

    std::vector<Layer> layers;
    std::vector<Polygon> outline;  // from Edge.Cuts, stitched into loops

    std::vector<Track> tracks;
    std::vector<Via> vias;
    std::vector<Pad> pads;
    std::vector<ZoneFill> zones;
    std::vector<Drill> drills;
    std::vector<Component> components;
    std::vector<Graphic> graphics;
    std::vector<TextItem> texts;

    // Diagnostics the importer could not resolve. Surfaced to the user rather
    // than silently dropped.
    std::vector<std::string> warnings;

    std::vector<int> copperLayers() const;
    const Layer* findLayer(std::string_view name) const;
    int layerIndex(std::string_view name) const;
};

}  // namespace pcbview
