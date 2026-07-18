#version 460

// Resolve the path tracer's HDR accumulation (a running sum) to a display image:
// divide by the sample count, tonemap, and let the SRGB colour attachment apply
// the gamma encoding on store -- so this outputs LINEAR, same as board.frag.

layout(rgba32f, set = 0, binding = 0) uniform readonly image2D hdrImg;
layout(rgba32f, set = 0, binding = 1) uniform readonly image2D denoisedImg;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    uvec2 dim;
    uint sampleCount;
    uint flags;  // bit 0: read the denoised image instead of the raw accumulation
} pc;

// Hue-preserving soft shoulder, IDENTITY below the knee. The raster path never
// tonemaps -- board.frag writes lit colour straight to the SRGB target -- so any
// curve applied here is a colour DIFFERENCE between the two modes. Per-channel
// ACES (the previous curve) lifted the midtones ~40% and desaturated saturated
// colours (small channels rise proportionally more than the dominant one), which
// is exactly the washed/milky "blown out" look. This curve instead passes every
// normally-lit value through UNCHANGED and only rolls the max component smoothly
// from the knee to 1.0, so sun glints compress without clipping and without any
// hue shift (all channels scale together).
vec3 shoulder(vec3 x) {
    const float knee = 0.80;
    float m = max(x.r, max(x.g, x.b));
    if (m <= knee) return x;
    float t = m - knee;
    // Reinhard-style rolloff; slope is exactly 1 at the knee (C1 with identity),
    // approaching 1.0 as m -> infinity.
    float mo = knee + (1.0 - knee) * t / (t + (1.0 - knee));
    return x * (mo / m);
}

void main() {
    ivec2 c = ivec2(gl_FragCoord.xy);
    vec3 hdr = ((pc.flags & 1u) != 0u ? imageLoad(denoisedImg, c).rgb
                                      : imageLoad(hdrImg, c).rgb) /
               max(float(pc.sampleCount), 1.0);
    // PT's sun + sky sum to ~1.1-1.25x albedo on a lit top face, a touch hotter
    // than the raster rig (whose diffuse weights sum to 1.0); a fixed exposure
    // brings a fully lit surface back to ~its albedo so the two modes match.
    const float kExposure = 0.85;
    outColor = vec4(shoulder(hdr * kExposure), 1.0);
}
