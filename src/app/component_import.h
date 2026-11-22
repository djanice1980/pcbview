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

#include <QString>
#include <QStringList>

#include <string>
#include <vector>

#include "geom/tessellate.h"

namespace pcbview::app {

struct ComponentImport {
    std::vector<geom::Part> parts;  // Material::Component, world space (mm)
    bool available = false;         // kicad-cli found and export produced a GLB
    std::string message;            // status / reason, for the status bar
};

// The import in two halves, so the window can run kicad-cli WITHOUT blocking:
// a cold STEP tessellation of a full board takes minutes the first time, and
// a synchronous wait froze the window and then gave up at two minutes with
// the board left bare and no explanation.
struct ComponentExportPlan {
    QString cli;          // empty: kicad-cli not found (message says so)
    QStringList args;     // the export command, when it needs running
    QString cache;        // the GLB this board resolves to
    bool cached = false;  // the GLB already exists: no process needed
    std::string message;  // set when cli is empty
};
ComponentExportPlan planComponentExport(const std::string& pcbPath);

// After the export ran (`ran` = a process was started; its exit code, whether
// it exited normally, and its stderr), or straight away on a cache hit.
ComponentImport finishComponentImport(const ComponentExportPlan& plan, bool ran,
                                      int exitCode, bool normalExit,
                                      const QString& stderrText,
                                      const geom::Bounds& boardBounds);

// Both halves, synchronously (headless captures, which wait for everything).
// Never throws. On any failure (no kicad-cli, export error, empty board)
// returns available=false with `message` explaining why and parts empty, so the
// board still renders without components. `boardBounds` is the assembled board's
// bounds, used to decide whether each part mounts top or bottom.
ComponentImport importComponents(const std::string& pcbPath,
                                 const geom::Bounds& boardBounds);

}  // namespace pcbview::app
