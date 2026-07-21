#pragma once

// IPC-2581 (DPMX) -> LayerArt.
//
// A single XML file carrying the whole product model: layer list, a REAL
// stackup with thicknesses (the only import format we have that ships them),
// pad/track/plane geometry per layer with net names on every Set, and drill
// holes on span-named DRILL layers. Parsed with pugixml; geometry realized
// through the same shape builders and dark/clear compositor as the ODB++
// importer, producing the same LayerArt the other importers feed assemble().

#include <string>

#include "geom/layer_art.h"

namespace pcbview::ipc2581 {

// Throws std::runtime_error when the file is unreadable or not IPC-2581.
geom::LayerArt importFile(const std::string& path);

// True if `path` is an XML file whose head contains an <IPC-2581 root.
bool isIpc2581(const std::string& path);

}  // namespace pcbview::ipc2581
