// Pure camera-fit math, split out of VulkanWindow so the headless test suite
// can pin it down. No Qt, no Vulkan, no glm.
#pragma once

#include <algorithm>
#include <cmath>

namespace camfit {

// Margin factor: 0.62 backs off far enough that the fitted span occupies
// ~81% of its axis (0.5 would be edge-to-edge). Kept from the original
// vertical-only fit so existing framing tightness is unchanged.
inline constexpr float kMargin = 0.62f;

// Distance at which a spanX x spanY board fits a perspective frustum with
// the given vertical half-FOV (radians) and width/height aspect ratio.
//
// The original fit used max(spanX, spanY) against the VERTICAL FOV only.
// That silently under-covers the horizontal axis whenever the viewport is
// narrower than the board is wide relative to its height (aspect below
// spanX/spanY * margin ratio): on a folding-display machine the window can
// open portrait-narrow, and wide boards clipped at the frame edges in
// headless captures. Fit each axis against its own FOV and take the max.
inline float fitDistance(float spanX, float spanY, float halfFovRad,
                         float aspect) {
    const float t = std::tan(halfFovRad);
    if (t <= 0.0f) return 1.0f;
    const float a = aspect > 1e-4f ? aspect : 1.0f;
    const float dY = spanY * kMargin / t;        // vertical coverage
    const float dX = spanX * kMargin / (t * a);  // horizontal coverage
    return std::max(dX, dY);
}

}  // namespace camfit
