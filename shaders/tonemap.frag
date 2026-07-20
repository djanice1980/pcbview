#version 460

// Resolve the path tracer's HDR accumulation (a running sum) to a display image:
// divide by the sample count, tonemap, and let the SRGB colour attachment apply
// the gamma encoding on store -- so this outputs LINEAR, same as board.frag.

layout(rgba32f, set = 0, binding = 0) uniform readonly image2D hdrImg;
layout(rgba32f, set = 0, binding = 1) uniform readonly image2D denoisedImg;
// First-hit net phase: .r = position along the net, .g = 1 on highlighted
// copper. See pathtrace.comp.
layout(rgba32f, set = 0, binding = 2) uniform readonly image2D netPhaseImg;

layout(location = 0) out vec4 outColor;

layout(push_constant) uniform Push {
    uvec2 dim;
    uint sampleCount;
    uint flags;  // bit 0: read the denoised image instead of the raw accumulation
                 // bit 1: run the net chase animation
    uint timeMs;  // milliseconds since the net selection, for the chase
} pc;

// The chase, applied at DISPLAY time rather than during tracing.
//
// This is the whole trick that makes an animated highlight possible in a path
// tracer built as a progressive accumulator: touching the scene would reset
// convergence every frame and the image would never resolve past one noisy
// sample. Instead the net is traced ONCE at full emission, and the animation
// becomes a modulation of the finished pixels.
//
// The honest limitation: this only animates copper the camera sees DIRECTLY.
// The red spill thrown onto surrounding copper and laminate is baked into the
// accumulated radiance and cannot be unbaked per-pixel, so it stays steady
// while the band travels. In practice that reads well -- a moving filament
// inside a stable glow -- but it is not what a physically moving emitter
// would do.
//
// Keep in step with netChase() in board.frag.
float netChase(float phase) {
    const float t = float(pc.timeMs) * 0.001;
    const float kWipe = 2.2;
    if (t < kWipe) {
        const float head = t / kWipe;
        // Floor rather than zero: the accumulated pixel here IS the emissive
        // net, so multiplying by 0 would punch a black hole in the copper.
        // A low floor reads as unlit trace instead.
        if (phase > head) return 0.06;
        return mix(2.4, 1.0, clamp((head - phase) * 5.0, 0.0, 1.0));
    }
    const float g = fract(phase * 1.5 - (t - kWipe) * 0.275);
    return 0.12 + 0.88 * (0.5 + 0.5 * cos(6.2831853 * g));
}

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
    if ((pc.flags & 2u) != 0u) {
        const vec4 ph = imageLoad(netPhaseImg, c);
        if (ph.g > 0.5) hdr *= netChase(ph.r);
    }
    const float kExposure = 0.85;
    outColor = vec4(shoulder(hdr * kExposure), 1.0);
}
