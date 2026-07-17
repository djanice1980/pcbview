#pragma once

// A tiny top-down software rasteriser.
//
// This is a verification tool, not the renderer. It exists so geometry can be
// checked visually before any Vulkan code exists -- which keeps geometry bugs
// and renderer bugs from hiding behind each other. It stays useful afterwards as
// a headless smoke test with no GPU or window required.

#include <string>

#include "geom/tessellate.h"

namespace pcbview::soft {

struct PreviewOptions {
    int width = 1400;

    // Look up at the board from underneath: keep the lowest surface, and mirror
    // in X the way KiCad and every fab view does. Without the mirror you get an
    // x-ray through the board rather than a bottom view.
    bool fromBottom = false;

    // Suppress the mirror to get that x-ray instead -- features stay registered
    // with the top image, which makes the two directly diffable. Diagnostic
    // only; ignored unless fromBottom is set.
    bool noMirror = false;
};

// Orthographic, z-buffered, Lambert-shaded. Writes a 24-bit BMP.
void renderPreview(const geom::BoardMesh& mesh, const std::string& path,
                   const PreviewOptions& opts = {});

}  // namespace pcbview::soft
