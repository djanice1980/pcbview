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

#include <clipper2/clipper.h>

#include <string>
#include <vector>

#include "geom/layer_art.h"

namespace pcbview::gerber {

// Board area from a profile drawn as thin stroked LOOPS (perimeter plus
// cutouts): unions the strokes into ribbons, keeps outer boundaries, even-odd
// fills. Shared with the ODB++ importer, whose profile files sometimes stroke
// the outline the same way instead of filling a surface.
Clipper2Lib::Paths64 boardFromProfile(const Clipper2Lib::Paths64& strokes);

// Import from any of: a .zip path, a directory, or a .gbrjob path.
// Throws std::runtime_error if nothing usable is found.
geom::LayerArt importPackage(const std::string& path);

// True if `path` looks like something importPackage can handle.
bool isGerberPackage(const std::string& path);

}  // namespace pcbview::gerber
