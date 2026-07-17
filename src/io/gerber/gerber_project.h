#pragma once

// Gerber package -> LayerArt.
//
// Accepts a .zip, a folder, or a .gbrjob, discovers the layers, parses each
// Gerber and the Excellon drill file, and assembles the same LayerArt the KiCad
// importer produces -- so both feed assemble() identically.
//
// Layer identity comes from, in order of preference:
//   1. the .gbrjob manifest (files, functions, and a real stackup with
//      thicknesses -- this is what closes the "we derive the stackup" gap);
//   2. each file's own %TF.FileFunction attribute;
//   3. filename heuristics, only as a last resort, with a warning.

#include <string>
#include <vector>

#include "geom/layer_art.h"

namespace pcbview::gerber {

// Import from any of: a .zip path, a directory, or a .gbrjob path.
// Throws std::runtime_error if nothing usable is found.
geom::LayerArt importPackage(const std::string& path);

// True if `path` looks like something importPackage can handle.
bool isGerberPackage(const std::string& path);

}  // namespace pcbview::gerber
