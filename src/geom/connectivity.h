#pragma once

// Pseudo-net extraction: recover connectivity from copper geometry alone.
//
// A net IS galvanically-connected copper, so when a package carries no netlist
// (a Gerber plotted without X2 %TO.N% attributes, which is most of them) the
// connectivity is still fully determined by what the copper touches. This
// walks it: connected islands per layer, joined through plated barrels and
// blind/buried bores, unioned into groups.
//
// The result is EXACT connectivity, not a guess -- but it is not a netlist,
// and the difference matters:
//
//   * There are no NAMES. Ground comes back as "~1", never "GND"; nothing in
//     the copper knows what it is called.
//   * A net that is not fully routed appears as SEVERAL pseudo-nets. That is
//     the copper honestly reporting that it is not connected.
//   * Two design nets shorted together appear as ONE. On a package with no
//     netlist to check against, that is the most useful thing here.
//   * Anything joined only THROUGH a component -- 0R links, ferrites,
//     net-ties -- stays separate, because a component is not copper.
//
// Callers must present the result as derived, never as ground truth. See
// LayerArt::netsArePseudo.

#include <functional>
#include <string>

#include "geom/layer_art.h"

namespace pcbview::geom {

struct PseudoNetStats {
    int groups = 0;      // total connected groups found
    int connecting = 0;  // groups spanning more than one island
};

// Replaces `art.nets` and every copper layer's `netArt` with pseudo-nets
// derived from the geometry, and sets `art.netsArePseudo`. Groups are ordered
// by copper area, largest first -- the big pours are almost always ground and
// power, so they land at the top of the panel where they are expected.
//
// `connecting` matters for reading the result: a group of ONE island is a
// lone piece of copper joined to nothing (an unconnected pad, a fragment of
// pour, a copper logo). Dense boards have hundreds of them, and quoting only
// the total makes the extraction look like it exploded when in fact most of
// what it found is isolated by design.
//
// Does nothing and returns zeroes when the board already has real nets:
// inferred data must never overwrite a netlist.
// Progress callback: (stage description, 0-100). Return false to CANCEL.
//
// Cancelling is safe at any point: `art` is not touched until the very last
// step, so an abandoned run leaves the board exactly as it was.
using ProgressFn = std::function<bool(const std::string& stage, int percent)>;

PseudoNetStats extractPseudoNets(LayerArt& art, const ProgressFn& progress = {});

}  // namespace pcbview::geom
