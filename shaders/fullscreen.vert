#version 460

// A single oversized triangle covering the viewport -- no vertex buffer. Used by
// the tonemap pass that resolves the path tracer's HDR accumulation image.
void main() {
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
