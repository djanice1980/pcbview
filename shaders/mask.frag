#version 450

// Depth only -- the pipeline writes no colour (its write mask is empty), so
// this exists solely because a graphics pipeline needs a fragment stage. The
// whole point is the depth value the rasterizer records, not any output here.
void main() {}
