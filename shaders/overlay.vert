#version 450

// Screen-space overlay (measurements, dimension callouts): vertices arrive in
// PIXELS with the origin at the top-left, which maps straight onto Vulkan's
// y-down NDC -- no flip. Everything is pre-built as triangles on the CPU
// (thick lines, arrowheads, stroked text), so this is a pure transform.

layout(location = 0) in vec2 inPos;    // pixels, origin top-left
layout(location = 1) in vec4 inColor;

layout(push_constant) uniform Pc { vec4 viewport; } pc;  // x = w, y = h in px

layout(location = 0) out vec4 vColor;

void main() {
    vec2 ndc = vec2(inPos.x / pc.viewport.x, inPos.y / pc.viewport.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    vColor = inColor;
}
