#pragma once

// Component bodies for a KiCad board, sourced through the installed kicad-cli.
//
// KiCad 10 ships its 3D model library as STEP only -- no .wrl siblings -- so
// there is no lightweight mesh to parse directly. Rather than embed a B-rep
// kernel (OpenCASCADE), we let the installed kicad-cli do the STEP tessellation
// it already contains: it exports a components-only GLB, which we cache and
// then load as ordinary triangle parts. The first open of a board runs the
// export (~3s); every later open reuses the cached GLB and needs neither KiCad
// nor a network, which is the "portable, grows as needed" behaviour the cache
// is for.
//
// This lives in the app layer, not io/, because it drives a child process and
// touches user paths -- Qt (QProcess/QStandardPaths) rather than the Qt-free io
// parsers.

#include <string>
#include <vector>

#include "geom/tessellate.h"

namespace pcbview::app {

struct ComponentImport {
    std::vector<geom::Part> parts;  // Material::Component, world space (mm)
    bool available = false;         // kicad-cli found and export produced a GLB
    std::string message;            // status / reason, for the status bar
};

// Never throws. On any failure (no kicad-cli, export error, empty board)
// returns available=false with `message` explaining why and parts empty, so the
// board still renders without components. `boardBounds` is the assembled board's
// bounds, used to decide whether each part mounts top or bottom.
ComponentImport importComponents(const std::string& pcbPath,
                                 const geom::Bounds& boardBounds);

}  // namespace pcbview::app
