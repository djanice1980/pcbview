#version 450

// Screen-space overlay (measurements, dimension callouts): vertices arrive in
// PIXELS with the origin at the top-left, which maps straight onto Vulkan's
// y-down NDC -- no flip. Everything is pre-built as triangles on the CPU
// (thick lines, arrowheads, stroked text), so this is a pure transform.

layout(location = 0) in vec2 inPos;    // pixels, origin top-left
layout(location = 1) in vec4 inColor;

// x = w, y = h in px; z = horizontal shift in px, which is what gives the
// overlay a DEPTH in stereo.
//
// Drawn at the same pixels in both eyes, an overlay has whatever disparity the
// eyes' frusta happen to impose -- and on an asymmetric pair that is large and
// convergent, so it appears right against the face and has to be crossed at to
// read. Shifting each eye's copy horizontally places it at a chosen distance
// instead. Zero on the desktop, where there is one eye and no such thing as
// depth for a flat overlay.
layout(push_constant) uniform Pc { vec4 viewport; } pc;

layout(location = 0) out vec4 vColor;

void main() {
    vec2 p = vec2(inPos.x + pc.viewport.z, inPos.y);
    vec2 ndc = vec2(p.x / pc.viewport.x, p.y / pc.viewport.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = inColor;
}
