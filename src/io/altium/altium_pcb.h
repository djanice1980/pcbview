#pragma once

// Altium Designer .PcbDoc -> LayerArt.
//
// A PcbDoc is an OLE compound document (CFB) of binary streams. Record
// layouts, unit conversion and layer identities are adapted from KiCad's
// Altium importer (pcbnew/pcb_io/altium, GPL-3 -- licence-compatible and
// credited in NOTICE.md); the CFB container is read with Microsoft's
// single-header compoundfilereader (MIT), the same one KiCad uses.
//
// Viewer-grade subset: stackup + outline from Board6, tracks/arcs/vias/pads/
// fills/regions with nets from Nets6, polygon pours from their Regions6
// fill pieces, soldermask openings derived from pads and vias, silkscreen
// from the overlay layers. Text is counted, not rendered.

#include <string>

#include "geom/layer_art.h"

namespace pcbview::altium {

// Throws std::runtime_error when the file is unreadable or not a PcbDoc.
geom::LayerArt importPcbDoc(const std::string& path);

// True if `path` has a PcbDoc extension and the CFB magic.
bool isPcbDoc(const std::string& path);

}  // namespace pcbview::altium
