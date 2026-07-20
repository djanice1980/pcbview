#pragma once

// RS-274X (Gerber X2) parser: one file -> filled polygons.
//
// Produces the DARK image of a single Gerber layer as Clipper paths (Y already
// flipped, ready for LayerArt), plus per-net copper when the file carries X2
// %TO.N% attributes. Handles the feature set KiCad emits and that
// real fab packages use; anything unsupported is reported in `warnings` rather
// than silently mis-drawn -- a viewer that renders what the fab will build must
// not guess.

#include <clipper2/clipper.h>

#include <string>
#include <vector>

namespace pcbview::gerber {

// The %TF.FileFunction attribute, when present. This is what identifies a layer
// without filename heuristics -- copper (with stack position), mask, silk, etc.
struct FileFunction {
    enum class Kind { Unknown, Copper, Soldermask, Silkscreen, Paste, Profile };
    Kind kind = Kind::Unknown;
    int copperIndex = 0;      // 1-based stack position for copper (L1 = top)
    bool top = false;
    bool bottom = false;
};

// Copper tagged with its net, from the Gerber X2 %TO.N,<netname>*% object
// attribute. KiCad emits these when "Use extended X2 format" is on; plenty of
// packages have no net attributes at all, in which case this is simply empty
// and the layer behaves as it always has.
//
// `area` is the geometry drawn while that net was current, in the same Clipper
// units and orientation as `dark`. `segments` are the LINEAR draws only (mm),
// which is what the measure tool can walk as a graph; arcs and flashes
// contribute area but no segment.
struct NetArea {
    std::string name;
    Clipper2Lib::Paths64 area;
    double routedMm = 0.0;
    struct Seg { double ax, ay, bx, by; };
    std::vector<Seg> segments;
};

struct GerberImage {
    // Final dark image after all polarity operations, in Clipper units with Y
    // flipped. Positive winding.
    Clipper2Lib::Paths64 dark;

    FileFunction function;
    bool hasFunction = false;

    // Per-net copper, empty when the file carries no TO.N attributes.
    std::vector<NetArea> nets;

    std::vector<std::string> warnings;
    bool ok = false;  // false if a fatal parse error left the image unusable
};

// Parse Gerber source text. `name` is only for diagnostics.
GerberImage parseGerber(const std::string& text, const std::string& name);

// Parse a FileFunction attribute value (e.g. "Copper,L1,Top"). Used by the
// project layer to read the same attribute out of a .gbrjob manifest.
FileFunction parseFileFunctionPublic(const std::string& value);

}  // namespace pcbview::gerber
