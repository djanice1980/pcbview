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
#include <array>
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

    // Linear draws on THIS layer that carried no net tag (mm, net unused).
    // Kept per layer because pseudo-net inference assigns each one to the
    // copper island containing it -- and islands from different layers
    // overlap in 2D, so a flat list could tag a bottom-layer stroke with a
    // top-layer net. Unused (and empty) when the package carries real nets.
    struct LooseSeg {
        double ax = 0, ay = 0, bx = 0, by = 0;
    };
    std::vector<LooseSeg> looseSegments;

    // What `art` MEANS depends on kind, and the difference is load-bearing:
    //   Copper / Silkscreen : the filled geometry itself
    //   Soldermask          : the OPENINGS, not the film
    //
    // Mask is inverted because that is what both formats give us. KiCad lists
    // which pads open the mask; a Gerber mask file flashes the openings. The
    // film is derived in assemble() as (outline - openings), which is also what
    // makes via tenting fall out for free.
    Clipper2Lib::Paths64 art;

    // Copper only: the same geometry as `art`, but SPLIT BY NET so net
    // identity survives into the mesh (net highlighting needs to know which
    // triangles belong to which net, and `art` is a single merged union in
    // which that is gone).
    //
    // Splitting is lossless: design rules keep different nets apart, so
    // unioning each net separately covers exactly the same area as unioning
    // them together. `net` indexes LayerArt::nets; -1 means unknown -- copper
    // with no net in KiCad, and any Gerber object with no %TO.N% attribute.
    struct NetRegion {
        int net = -1;
        Clipper2Lib::Paths64 paths;
    };
    std::vector<NetRegion> netArt;

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

    // Surface finish on exposed copper, from KiCad's `(copper_finish ...)`.
    // Empty when unknown (every Gerber package -- a .gbrjob does not record
    // it), in which case the renderer keeps its gold default rather than
    // guessing something duller and being wrong more often.
    std::string copperFinish;
    double silkThickness = 0.010;

    // Board profile, closed loops. Drills are NOT yet subtracted; assemble()
    // does that, because the mask must tent ACROSS a drill and therefore needs
    // the un-drilled profile.
    Clipper2Lib::Paths64 outline;
    Clipper2Lib::Paths64 drills;

    // Plated drills only (PTH / vias). assemble() lines these with a copper barrel
    // that spans the whole copper stack as ONE intact part -- a via is a single
    // plated tube through the board, not something the exploded view should slice
    // into per-layer pieces. Non-plated (NPTH) holes get no barrel.
    Clipper2Lib::Paths64 barrels;

    // Partial-depth plated bores: blind/buried vias. Each is drilled only
    // between its two end copper layers, so it appears in neither `drills`
    // (which punches the whole stack) nor `barrels` (full-stack tubes).
    // assemble() bores it through just the copper and dielectric inside its
    // span and lines it with a span-length barrel. End layers are identified
    // by ArtLayer NAME, not by Z, so applyThickness's re-stacking moves the
    // bore with its layers for free.
    struct PartialBore {
        Clipper2Lib::Path64 path;  // the hole, at drill radius
        std::string fromLayer, toLayer;  // copper ArtLayer names, either order
    };
    std::vector<PartialBore> partialBores;

    std::vector<ArtLayer> layers;
    // WARNINGS are things that may make the render disagree with the board:
    // a file we could not identify, a thickness we had to guess. NOTES are
    // things we identified correctly and deliberately did not use.
    //
    // Keeping them apart is not cosmetic. A package where everything was
    // understood should not announce five warnings -- a viewer that cries
    // wolf on a healthy import teaches the user to ignore it, and then the
    // one warning that actually matters goes unread.
    std::vector<std::string> warnings;
    std::vector<std::string> notes;

    // Net table: name plus the routed copper length (sum of that net's track
    // segments) and via count, for the measure tool's net panel. Filled from
    // KiCad connectivity, or from Gerber X2 %TO.N% object attributes when the
    // package carries them (many do -- KiCad emits them by default).
    struct NetInfo {
        std::string name;
        double routedMm = 0.0;
        int viaCount = 0;
        // Copper area in mm^2, for DERIVED nets. (Those also get a routedMm
        // now -- summed from the untagged strokes assigned to their islands --
        // but area remains the honest headline for a net that is mostly pour.)
        double copperMm2 = 0.0;
        // The net has pour/plane copper (an X2 region or a KiCad zone fill)
        // that the track graph cannot walk -- so "no track route" between two
        // of its points can honestly say "joined through the plane" instead.
        bool hasPlane = false;
    };
    std::vector<NetInfo> nets;

    // True when `nets` was DERIVED from copper geometry rather than read from
    // a netlist (see geom/connectivity.h). Such nets have no real names, an
    // unrouted net appears as several of them, and two shorted nets appear as
    // one -- so every surface that shows them must say where they came from.
    bool netsArePseudo = false;
    // The net table came from an IPC-D-356 netlist: REAL names at test-point
    // positions (kept in netPoints), but no name->copper mapping yet.
    // extractPseudoNets recognises this and names its derived groups from the
    // contained test points instead of refusing to run.
    bool netsFromTestPoints = false;

    // Every track segment with its net (mm, Y flip applied) -- the graph the
    // measure tool walks to report the routed distance BETWEEN two points on
    // a net, not just the net's total. Layers are deliberately ignored: a
    // same-net layer change virtually always happens through a via at the
    // shared endpoint, so a 2D endpoint graph is the right approximation.
    struct NetSeg {
        double ax = 0, ay = 0, bx = 0, by = 0;
        int net = -1;
    };
    std::vector<NetSeg> netSegments;

    // Measurement snap targets the importers know exactly: pad and via
    // centres (KiCad only -- a Gerber flash is not distinguishable from any
    // other exposure, even with net attributes present), in mm with the Y flip
    // applied, z = 3D height for the
    // marker, net = index into `nets` (-1 for none). Drill/bore centres and
    // outline vertices are derived in assemble() from the geometry above and
    // do not need to be listed here.
    struct NetPoint {
        double pos[3] = {0, 0, 0};
        int net = -1;
    };
    std::vector<NetPoint> netPoints;
};

}  // namespace pcbview::geom
