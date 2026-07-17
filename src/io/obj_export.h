#pragma once

// Debug output: dump a BoardMesh as Wavefront OBJ.
//
// This exists so geometry can be inspected in any 3D viewer before the Vulkan
// renderer exists. It is a diagnostic, not a product feature.

#include <string>

#include "geom/tessellate.h"

namespace pcbview {

// One OBJ group per part, so layers can be toggled in a viewer.
void exportObj(const geom::BoardMesh& mesh, const std::string& path);

}  // namespace pcbview
