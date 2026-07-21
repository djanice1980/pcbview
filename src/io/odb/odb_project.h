#pragma once

// ODB++ product model -> LayerArt.
//
// Accepts a job directory, a .tgz/.tar.gz/.tar, or a .zip containing one.
// Reads matrix/matrix for the layer list, picks the board step (skipping
// panelization steps that merely step-repeat it), realizes every BOARD-context
// layer's features, takes the outline from the step profile, drills from
// DRILL layers (plating from their tools files, spans from the matrix), and
// nets -- REAL nets with names, per-feature -- from eda/data when present.
// Produces the same LayerArt as the Gerber and KiCad importers, so everything
// downstream is shared.

#include <string>

#include "geom/layer_art.h"

namespace pcbview::odb {

// Throws std::runtime_error when the container is unreadable or holds no
// ODB++ job. `stepOverride` picks a specific step by name (e.g. a "panel"
// step -- its STEP-REPEAT blocks are expanded, geometry-only); empty keeps
// the automatic board-step choice. The GUI feeds PCBVIEW_ODB_STEP through
// here.
geom::LayerArt importJob(const std::string& path,
                         const std::string& stepOverride = {});

// True if `path` looks like an ODB++ job: a directory (or .zip) containing
// matrix/matrix at any single-folder depth, or a .tgz/.tar[.gz] by extension.
bool isOdbJob(const std::string& path);

}  // namespace pcbview::odb
