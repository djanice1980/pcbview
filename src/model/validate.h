#pragma once

// Geometric self-checks on a BoardModel.
//
// These are correctness harnesses, not cosmetics. Net connectivity is a strong
// invariant: a track endpoint carrying net N must physically touch something
// else on net N. Transform bugs (a flipped rotation sign, a bad footprint
// offset) break that invariant loudly, which is exactly what we want.

#include <string>
#include <vector>

#include "model/board.h"

namespace pcbview {

struct GeometryReport {
    int trackEndpoints = 0;
    int orphanEndpoints = 0;
    std::vector<std::string> examples;  // capped; for diagnosis

    bool clean() const { return orphanEndpoints == 0; }
};

// Asserts every track endpoint touches a pad / via / track on the same net.
//
// Note this is one-directional and cannot see a mispositioned pad: the track
// endpoint that should have landed on it may still touch a neighbouring track
// mid-chain. Use validatePadConnectivity() for the other direction.
GeometryReport validateNetGeometry(const BoardModel& board);

// How many net-carrying pads are actually reached by a track or via, split by
// board side.
//
// The absolute rate is not meaningful on its own -- a pad connected only by a
// copper pour is legitimately untouched. The *gap between top and bottom* is
// what matters: footprint mirroring applies solely to bottom-side parts, so a
// mirroring bug shows up as bottom pads connecting far worse than top ones.
struct PadReport {
    int topPads = 0;
    int topTouched = 0;
    int bottomPads = 0;
    int bottomTouched = 0;

    double topRate() const {
        return topPads ? 100.0 * topTouched / topPads : 0.0;
    }
    double bottomRate() const {
        return bottomPads ? 100.0 * bottomTouched / bottomPads : 0.0;
    }
};

PadReport validatePadConnectivity(const BoardModel& board);

// Pads on different nets that physically overlap on a shared copper layer.
//
// Such an overlap is a short, so the correct count is always zero. This is the
// check that catches pad *orientation* bugs, which every positional check is
// blind to -- rotating a pad's shape does not move its centre, so connectivity
// happily reports zero orphans while the shapes are wrong. A double-counted
// rotation on a fine-pitch part lays each pad across the pitch axis and merges
// the whole row into one bar; here that shows up as a pile of shorts.
//
// Compares oriented bounding boxes via the separating axis theorem: exact for
// rect pads, slightly conservative for roundrect/oval (whose corners are cut).
struct OverlapReport {
    int pairsChecked = 0;
    int shorts = 0;
    std::vector<std::string> examples;  // capped

    bool clean() const { return shorts == 0; }
};

OverlapReport validatePadOverlaps(const BoardModel& board);

}  // namespace pcbview
