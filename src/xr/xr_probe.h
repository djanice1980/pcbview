#pragma once

namespace pcbview::xr {

// `pcbview --xr-probe`: connect to whatever OpenXR runtime the system has
// registered, describe the headset, and print the numbers stereo rendering has
// to be built against -- per-eye swapchain size, refresh rate, blend modes, and
// whether the Vulkan-2 binding this renderer needs is actually offered.
//
// Deliberately does NOT create a session or touch Vulkan. A session forces the
// graphics binding, and OpenXR dictates the physical device and the instance
// and device extensions, which means restructuring how pcbview creates its
// VkInstance/VkDevice. Establishing that the runtime, headset and controllers
// are reachable is worth doing BEFORE paying that cost.
//
// Returns 0 when a headset was found and described, non-zero otherwise.
int probe();

}  // namespace pcbview::xr
