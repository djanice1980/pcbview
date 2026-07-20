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

layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;  // .w = RT enable
    vec4 params;
    // xyz = camera forward; w = orbit distance in ORTHO, 0 in perspective.
    vec4 camAxis;
    // x = highlighted net index, -1 for none.
    ivec4 highlight;
} push;

// Per-triangle net index and position along that net. Keep in step with
// board.frag, including netChase below.
struct TriInfo {
    int net;
    float phase;
};

layout(std430, set = 0, binding = 2) readonly buffer TriNets {
    TriInfo tris[];
} triNetTable;

// Per-net glow colour; a = 1 when highlighted. See board.frag.
layout(std430, set = 0, binding = 3) readonly buffer NetColours {
    vec4 colours[];
} netColourTable;

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
    const TriInfo info = triNetTable.tris[tri];
    if (info.net < 0) return vec4(0.0);
    vec4 c = netColourTable.colours[info.net];
    c.rgb *= netChase(info.phase);
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
    if (push.camAxis.w > 0.0) return -push.camAxis.xyz;
    return normalize(push.cameraPos.xyz - worldPos);
}
float viewScale(vec3 worldPos) {
    if (push.camAxis.w > 0.0) return push.camAxis.w;
    return length(push.cameraPos.xyz - worldPos);
}

const float kSubstratePeelAlpha = 0.42;

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
    vec3 keyDir;
    float keyDist;
    if (push.camAxis.w > 0.0) {
        keyDir = normalize(-push.camAxis.xyz + camRight * 0.55 + camUp * 0.55);
        keyDist = 1.0e4;
    } else {
        vec3 keyPos =
            push.cameraPos.xyz + (camRight * 0.55 + camUp * 0.55) * viewDist;
        keyDir = normalize(keyPos - inWorldPos);
        keyDist = length(keyPos - inWorldPos);
    }
    vec3 fillDir = normalize(viewDir - camRight * 0.5 - camUp * 0.25);

    float key = max(dot(n, keyDir), 0.0);
    float fill = max(dot(n, fillDir), 0.0);

    // Ray-traced shadow + AO, only when the gate is set.
    float shadow = 1.0;
    float ao = 1.0;
    if (push.cameraPos.w > 0.5) {
        if (!occluded(inWorldPos + n * 0.05, keyDir, keyDist))
            shadow = 1.0;
        else
            shadow = 0.0;
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
                      mix(m.albedo.a, kSubstratePeelAlpha, push.params.w),
                      m.params.w);

    outColor = vec4(lit, alpha);
}
