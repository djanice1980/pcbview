#version 460
#extension GL_EXT_ray_query : require

// Raster shading + ray-traced contact shadows and ambient occlusion.
//
// Byte-identical to board.frag except for the ray-query block: it traces against
// a TLAS over the board (bound at set 0, binding 1) to darken points the key
// light cannot reach and crevices under/between components. Tracing is gated by
// push.cameraPos.w (the CPU sets it only on a device with ray_query, with RT
// toggled on, and only at rest -- the acceleration structure is over the
// un-exploded geometry). When the gate is 0 this shades exactly like board.frag.

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inWorldPos;
layout(location = 2) flat in uint inMaterial;

layout(location = 0) out vec4 outColor;

struct Material {
    vec4 albedo;
    vec4 params;  // x roughness, y metallic, z explode rank, w fades-on-peel
    uvec4 extra;  // x = this draw's first global triangle
};

layout(std430, set = 0, binding = 0) readonly buffer Materials {
    Material materials[];
} materialTable;

layout(set = 0, binding = 1) uniform accelerationStructureEXT tlas;

// See board.vert for what MULTIVIEW changes and why.
#ifdef MULTIVIEW
#extension GL_EXT_multiview : require
layout(push_constant) uniform Push {
    mat4 viewProj[2];
    vec4 cameraPos[2];  // .w = RT enable
    vec4 params;
    vec4 camAxis[2];
    ivec4 highlight;
} push;
#define CAMERAPOS push.cameraPos[gl_ViewIndex]
#define CAMAXIS   push.camAxis[gl_ViewIndex]
#else
layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;  // .w = RT enable
    vec4 params;
    // xyz = camera forward; w = orbit distance in ORTHO, 0 in perspective.
    vec4 camAxis;
    // x = highlighted net index, -1 for none.
    ivec4 highlight;
} push;
#define CAMERAPOS push.cameraPos
#define CAMAXIS   push.camAxis
#endif

// Per-triangle net index and position along that net. Keep in step with
// board.frag, including netChase below.
layout(std430, set = 0, binding = 2) readonly buffer TriNets {
    int nets[];
} triNetTable;

// Per-net glow colour; a = 1 when highlighted. See board.frag.
layout(std430, set = 0, binding = 3) readonly buffer NetColours {
    vec4 colours[];
} netColourTable;

// Per-net ORIGIN (xyz) and inverse span (w): one end of the run, and the
// reciprocal of its length. Phase is derived from this PER FRAGMENT rather
// than stored per triangle -- a per-triangle value is constant across each
// triangle, so the highlight rendered the triangulation instead of the shape,
// and a round pad lit up as a visible fan of facets.
layout(std430, set = 0, binding = 4) readonly buffer NetSpans {
    vec4 spans[];
} netSpanTable;

// Keep in step with board.frag and pathtrace.comp.
const vec3 kNetGlow = vec3(1.0, 0.09, 0.06);

// Wipe then cycling gradient -- see board.frag for the reasoning.
float netChase(float phase) {
    if (push.highlight.w == 0) return 1.0;
    const float t = float(push.highlight.z) * 0.001;

    const float kWipe = 2.2;
    if (t < kWipe) {
        const float head = t / kWipe;
        if (phase > head) return 0.0;
        return mix(2.4, 1.0, clamp((head - phase) * 5.0, 0.0, 1.0));
    }
    // Trough well below 1 so the band clears the glow multiplier's clipping
    // point -- see board.frag.
    // Duty cycle is deliberately skewed dark. A plain cosine is symmetric --
    // half the net lit at any moment -- which reads as a pulsing net rather
    // than a travelling band. Raising the band to a power narrows the bright
    // pulse and stretches the gap between passes, so the eye tracks one head
    // moving instead of the whole run breathing.
    const float g = fract(phase * 1.5 - (t - kWipe) * 0.275);
    // An explicit pulse rather than a cosine. A cosine is symmetric -- half
    // the net lit at any instant -- which reads as the whole run breathing
    // rather than one head travelling along it. kPulse is the lit half-width
    // in cycles, so the net is lit ~2*kPulse of the time and dark the rest:
    // at 0.13 that is roughly a quarter lit, three quarters dark.
    const float kPulse = 0.13;
    const float d = min(g, 1.0 - g);          // distance to the pulse centre
    const float band = 1.0 - smoothstep(0.0, kPulse, d);
    return 0.12 + 0.88 * band;
}

vec4 netHighlight() {
    if (push.highlight.x < 0) return vec4(0.0);
    const uint tri = materialTable.materials[inMaterial].extra.x +
                     uint(gl_PrimitiveID);
    const int net = triNetTable.nets[tri];
    if (net < 0) return vec4(0.0);
    // Distance along the net from its far end, normalised -- smooth across a
    // triangle because it depends on the fragment's position, not its face.
    const vec4 span = netSpanTable.spans[net];
    const float phase = clamp(length(inWorldPos - span.xyz) * span.w, 0.0, 1.0);
    vec4 c = netColourTable.colours[net];
    c.rgb *= netChase(phase);
    return c;
}

vec3 applyNetHighlight(vec3 albedo) {
    if (push.highlight.x < 0) return albedo;
    return mix(albedo, vec3(dot(albedo, vec3(0.299, 0.587, 0.114))), 0.85) *
           0.42;
}

// See board.frag: orthographic has ONE view direction for every fragment, and
// the eye-point shortcut reverses at/behind the eye plane, blackening the near
// edge. Keep these two in step with board.frag.
vec3 viewVector(vec3 worldPos) {
    if (CAMAXIS.w > 0.0) return -CAMAXIS.xyz;
    return normalize(CAMERAPOS.xyz - worldPos);
}
float viewScale(vec3 worldPos) {
    if (CAMAXIS.w > 0.0) return CAMAXIS.w;
    return length(CAMERAPOS.xyz - worldPos);
}

// Peeled-substrate target opacity: user-adjustable, in extra.y (x1000).

// True if anything opaque lies between `origin` and `origin + dir*tmax`.
bool occluded(vec3 origin, vec3 dir, float tmax) {
    rayQueryEXT rq;
    rayQueryInitializeEXT(rq, tlas,
                          gl_RayFlagsTerminateOnFirstHitEXT | gl_RayFlagsOpaqueEXT,
                          0xFF, origin, 0.02, dir, tmax);
    while (rayQueryProceedEXT(rq)) {}
    return rayQueryGetIntersectionTypeEXT(rq, true) !=
           gl_RayQueryCommittedIntersectionNoneEXT;
}

// The same sun the path tracer uses, so raster and traced modes agree about
// where the light is. See pathtrace.comp's kSunDirWorld.
const vec3 kSunDirWorld = normalize(vec3(0.35, 0.25, 1.0));

// Fractional visibility of a sun that has ANGULAR SIZE, rather than a single
// yes/no ray against a point light.
//
// A point sun gives every shadow a razor edge at every distance, which is the
// giveaway that a render is not path-traced: real contact shadows are crisp
// where an object meets the board and soften as they run away from it. Taps are
// spread across a small disc about the sun direction, so an edge lands
// somewhere between fully lit and fully shadowed depending on how much of the
// sun is blocked.
//
// A FIXED pattern, like the AO kernel: no per-pixel randomness, so no shimmer.
// Banding instead of noise is the right trade here -- the eye forgives a soft
// gradient and cannot forgive a boiling one, least of all in a headset.
float sunVisibility(vec3 p, vec3 n, vec3 dir, float tmax) {
    // ~1.5 degrees of angular radius, near enough the real sun.
    const float radius = 0.026;
    vec3 up = abs(dir.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, dir));
    vec3 b = cross(dir, t);

    // Centre plus four, rotated 45 degrees so they straddle both axes.
    //
    // Four and not eight: shadow rays are the single most expensive thing this
    // shader does, and at eye resolution across two eyes, nine taps against one
    // was enough on its own to take a headset from smooth to a slideshow. Five
    // still reads as a soft edge; the difference from nine is visible only if
    // you go looking for it, and it costs nearly half as much.
    const vec2 disc[4] = vec2[](
        vec2( 0.71,  0.71), vec2(-0.71,  0.71),
        vec2(-0.71, -0.71), vec2( 0.71, -0.71));

    float open = occluded(p, dir, tmax) ? 0.0 : 1.0;
    // Count the taps actually taken. Dividing by a fixed five while skipping
    // below-horizon directions would score those skips as shadow and darken
    // every grazing surface -- and the cosine falloff is already applied
    // separately, so encoding it again here would double it.
    float taps = 1.0;
    for (int i = 0; i < 4; ++i) {
        vec3 d = normalize(dir + (t * disc[i].x + b * disc[i].y) * radius);
        if (dot(n, d) <= 0.0) continue;      // below the horizon: no light anyway
        taps += 1.0;
        if (!occluded(p, d, tmax)) open += 1.0;
    }
    return open / taps;
}

// Fraction of a short hemisphere around the normal that is open. 1 = fully open,
// 0 = fully enclosed. A fixed kernel biased toward the normal -- no per-pixel
// randomness, so it is temporally stable (no shimmer).
float ambientOcclusion(vec3 p, vec3 n) {
    const vec3 k[6] = vec3[](
        vec3(0.0, 0.0, 1.0),
        vec3(0.60, 0.0, 0.80),
        vec3(-0.60, 0.0, 0.80),
        vec3(0.0, 0.60, 0.80),
        vec3(0.0, -0.60, 0.80),
        vec3(0.42, 0.42, 0.80));
    // Tangent basis from the normal.
    vec3 up = abs(n.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
    vec3 t = normalize(cross(up, n));
    vec3 b = cross(n, t);
    const float radius = 2.5;  // mm; contact-shadow scale under small parts
    float open = 0.0;
    for (int i = 0; i < 6; ++i) {
        vec3 s = normalize(k[i]);
        vec3 dir = t * s.x + b * s.y + n * s.z;
        if (!occluded(p + n * 0.05, dir, radius)) open += 1.0;
    }
    return open / 6.0;
}

void main() {
    Material m = materialTable.materials[inMaterial];

    // Emissive highlight, before any shading -- see board.frag. Also skips
    // the shadow and AO rays for these fragments, so highlighting is if
    // anything slightly cheaper.
    const vec4 hl = netHighlight();
    if (hl.a > 0.0 && any(greaterThan(hl.rgb, vec3(0.002)))) {
        outColor = vec4(hl.rgb * (float(push.highlight.y) * 0.01) * 0.8, 1.0);
        return;
    }

    vec3 n = normalize(inNormal);
    vec3 viewDir = viewVector(inWorldPos);
    if (dot(n, viewDir) < 0.0) n = -n;

    vec3 camRight = normalize(cross(viewDir, vec3(0.0, 0.0, 1.0)));
    if (length(camRight) < 0.01) camRight = vec3(1.0, 0.0, 0.0);
    vec3 camUp = cross(camRight, viewDir);

    float viewDist = viewScale(inWorldPos);
    // Directional key in ORTHO, positional in perspective -- see board.frag
    // for why a positional lamp blackens edge walls in a parallel projection.
    //
    // cameraPos.w carries the lighting mode rather than a plain on/off, because
    // the push block is already at 128 bytes -- the size every Vulkan device is
    // required to offer and some offer nothing beyond. A float used as a
    // boolean has room for a third state and costs nothing.
    //   0 = no ray tracing, 1 = RT with the camera-relative key, 2 = RT with a
    //   world-fixed sun.
    //
    // The camera-relative key is a deliberate choice on the desktop: the light
    // sits over your shoulder however you orbit, so nothing is ever lit from
    // behind. In a HEADSET that same rule glues the sun to your skull -- turn
    // your head and every shadow swings with it, and the board never appears to
    // sit in a lit room. The world-fixed sun is the same one the path tracer
    // uses, so the two modes agree about where the light comes from.
    const bool worldSun = CAMERAPOS.w > 1.5;
    vec3 keyDir;
    float keyDist;
    if (worldSun) {
        keyDir = kSunDirWorld;
        keyDist = 1.0e5;
    } else if (CAMAXIS.w > 0.0) {
        keyDir = normalize(-CAMAXIS.xyz + camRight * 0.55 + camUp * 0.55);
        keyDist = 1.0e4;
    } else {
        vec3 keyPos =
            CAMERAPOS.xyz + (camRight * 0.55 + camUp * 0.55) * viewDist;
        keyDir = normalize(keyPos - inWorldPos);
        keyDist = length(keyPos - inWorldPos);
    }
    vec3 fillDir = normalize(viewDir - camRight * 0.5 - camUp * 0.25);

    float key = max(dot(n, keyDir), 0.0);
    float fill = max(dot(n, fillDir), 0.0);

    // Ray-traced shadow + AO, only when the gate is set.
    float shadow = 1.0;
    float ao = 1.0;
    if (CAMERAPOS.w > 0.5) {
        shadow = sunVisibility(inWorldPos + n * 0.05, n, keyDir, keyDist);
        ao = ambientOcclusion(inWorldPos, n);
    }


    // Shadowed key drops to a soft floor rather than black (there is fill + sky);
    // AO darkens ambient and crevices.
    float shadowedKey = key * mix(0.30, 1.0, shadow);
    float diffuse = 0.15 * mix(0.4, 1.0, ao) + 0.65 * shadowedKey + 0.20 * fill;
    diffuse *= mix(0.7, 1.0, ao);

    vec3 h = normalize(keyDir + viewDir);
    float roughness = clamp(m.params.x, 0.05, 1.0);
    float specPower = 2.0 / (roughness * roughness) - 2.0;
    float spec = pow(max(dot(n, h), 0.0), specPower) * (1.0 - roughness) * shadow;

    float fresnel = pow(1.0 - max(dot(n, viewDir), 0.0), 4.0);

    // Metals reflect their own colour, much brighter than dielectrics.
    vec3 specTint = mix(vec3(1.0), m.albedo.rgb, m.params.y);
    // Cheap metallic environment reflection so flat pads facing up read as shiny
    // from any angle (weighted by metallic -- matte IC bodies stay matte).
    vec3 refl = reflect(-viewDir, n);
    float envUp = clamp(refl.z * 0.5 + 0.5, 0.0, 1.0);
    vec3 env = mix(vec3(0.22), vec3(1.05), envUp);
    vec3 lit = applyNetHighlight(m.albedo.rgb) * diffuse
             + specTint * spec * mix(0.12, 1.3, m.params.y)
             + specTint * env * (m.params.y * 0.35)
             + vec3(fresnel) * 0.08;

    float alpha = mix(m.albedo.a,
                      mix(m.albedo.a, float(m.extra.y) * 0.001, push.params.w),
                      m.params.w);

    outColor = vec4(lit, alpha);
}
