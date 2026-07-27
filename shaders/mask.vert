#version 450

// The hidden-area mesh, drawn into depth before anything else.
//
// Positions arrive ALREADY IN NDC: the runtime supplies them on the z = -1
// plane of the view frustum, and the projection that maps them to screen is
// fixed per eye, so the transform is done once on the CPU rather than per
// vertex per frame. Nothing to do here but pass them through.
//
// z = 1.0 is the NEAR plane under this renderer's reversed-Z convention. The
// board is depth-tested GREATER against a buffer cleared to 0, so a masked
// pixel already holding 1.0 rejects everything drawn afterwards -- the
// rasterizer discards those fragments before they are ever shaded.
layout(location = 0) in vec2 inNdc;

void main() { gl_Position = vec4(inNdc, 1.0, 1.0); }
