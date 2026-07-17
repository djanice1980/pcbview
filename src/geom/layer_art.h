#pragma once

// LayerArt -- where the KiCad and Gerber importers meet.
//
// A board, once every format-specific semantic has been resolved to geometry:
// filled polygons per physical layer, a profile, drills, and a stackup that
// places them in Z. Nothing above this line survives: no tracks, no pads, no
// nets, no zones.
//
// The split exists because Gerber HAS no semantics to preserve. A .kicad_pcb
// describes intent (this is a track on net GND); a Gerber describes only
// exposure (this region is copper). There is no BoardModel to be recovered from
// a Gerber -- no nets, no pads, no vias -- so the two formats cannot share the
// semantic IR. They share this one instead:
//
//     KiCad  -> BoardModel (semantic) -> LayerArt -> mesh
//     Gerber ---------------------------> LayerArt -> mesh
//
// Everything downstream (clipping to the profile, subtracting drills, mask
// tenting, extrusion) is format-agnostic and lives in assemble().
//
// Coordinates are Clipper integers with the Y flip ALREADY applied, because that
// is the currency both importers already deal in and converting back to doubles
// here would be pure loss.

#include <clipper2/clipper.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "model/board.h"

namespace pcbview::geom {

// Clipper2 works in integers. KiCad's own internal unit is the nanometre, so
// 1e6 units per mm loses nothing and keeps a 50mm board at ~5e7 -- nowhere near
// int64 limits.
inline constexpr double kScale = 1e6;

inline int64_t toInt(double mm) {
    return static_cast<int64_t>(std::llround(mm * kScale));
}
inline double toMm(int64_t units) {
    return static_cast<double>(units) / kScale;
}

// The single Y flip. KiCad's Y axis points down; ours points up. Applied once,
// here, on the way into Clipper -- everything downstream is Y-up.
inline Clipper2Lib::Point64 toClipper(Vec2 p) {
    return Clipper2Lib::Point64{toInt(p.x), toInt(-p.y)};
}

// Force every path to positive (CCW) orientation.
//
// Load-bearing at the LayerArt boundary: FillRule::NonZero sums winding numbers,
// so two overlapping paths wound opposite ways cancel to zero and punch a hole.
// Both importers must normalise before art crosses into assemble(). (Silkscreen
// rings are the one place normalisation must happen AFTER a NonZero union, not
// before -- see buildLayerArt.)
inline void normalizeWinding(Clipper2Lib::Paths64& paths) {
    for (Clipper2Lib::Path64& path : paths) {
        if (Clipper2Lib::Area(path) < 0) {
            std::reverse(path.begin(), path.end());
        }
    }
}

struct ArtLayer {
    std::string name;  // "F.Cu", "In1.Cu", "F.Mask", "F.SilkS"
    LayerKind kind = LayerKind::Copper;
    double z = 0.0;  // bottom of the film
    double thickness = 0.0;

    // What `art` MEANS depends on kind, and the difference is load-bearing:
    //   Copper / Silkscreen : the filled geometry itself
    //   Soldermask          : the OPENINGS, not the film
    //
    // Mask is inverted because that is what both formats give us. KiCad lists
    // which pads open the mask; a Gerber mask file flashes the openings. The
    // film is derived in assemble() as (outline - openings), which is also what
    // makes via tenting fall out for free.
    Clipper2Lib::Paths64 art;

    // Soldermask only: how many openings were punched. Cross-checks against the
    // flash count in a *_Mask gerber.
    int openings = -1;
};

struct LayerArt {
    std::string sourcePath;

    // Finished-board thickness -- INCLUDES both mask films and every copper
    // foil. Not the bare core.
    double thickness = 1.6;
    double copperThickness = 0.035;
    double maskThickness = 0.010;
    double silkThickness = 0.010;

    // Board profile, closed loops. Drills are NOT yet subtracted; assemble()
    // does that, because the mask must tent ACROSS a drill and therefore needs
    // the un-drilled profile.
    Clipper2Lib::Paths64 outline;
    Clipper2Lib::Paths64 drills;

    std::vector<ArtLayer> layers;
    std::vector<std::string> warnings;
};

}  // namespace pcbview::geom
