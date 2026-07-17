#pragma once

#include <string>

#include "model/board.h"

namespace pcbview::kicad {

// Parses a .kicad_pcb into the neutral BoardModel.
// Throws std::runtime_error on unreadable or malformed input. Recoverable
// oddities land in BoardModel::warnings rather than being dropped silently.
BoardModel importPcb(const std::string& path);

}  // namespace pcbview::kicad
