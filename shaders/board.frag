#version 450

layout(location = 0) in vec3 inNormal;
layout(location = 1) in vec3 inWorldPos;
layout(location = 2) flat in uint inMaterial;

layout(location = 0) out vec4 outColor;

// Bindless material table, indexed by instance ID. Deliberately an SSBO rather
// than per-draw descriptor sets: ray tracing cannot rebind per draw, so the
// lookup must work from an instance index alone. See RT-readiness rule 3.
struct Material {
    vec4 albedo;  // rgb + opacity
    // x = roughness, y = metallic, z = explode rank,
    // w = 1 if this material turns translucent as the board peels
    vec4 params;
    // x = this draw's first global triangle; see MaterialGpu::extra.
    uvec4 extra;
};

layout(std430, set = 0, binding = 0) readonly buffer Materials {
    Material materials[];
} materialTable;

// Per-triangle net index, -1 for none. Indexed globally, hence the material's
// triangle base added to gl_PrimitiveID (which restarts every draw).
// Alongside the net, how far along it this triangle sits: 0 at one end of the
// run, 1 at the other. That ordering is what lets the chase animation sweep a
// head down the trace without the shader knowing any geometry.
struct TriInfo {
    int net;
    float phase;
};

layout(std430, set = 0, binding = 2) readonly buffer TriNets {
    TriInfo tris[];
} triNetTable;

// Per-NET glow colour: rgb, with a = 1 when that net is highlighted. Looking
// the colour up per net (rather than comparing against one index) is what
// lets several nets glow at once, each in its own colour.
layout(std430, set = 0, binding = 3) readonly buffer NetColours {
    vec4 colours[];
} netColourTable;

// Must match board.vert byte for byte -- one push block spans both stages.
layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;
    // x = mm per stage, y = eased progress, z = max |rank|,
    // w = peel amount 0..1 (normalised progress)
    vec4 params;
    // xyz = camera forward; w = orbit distance in ORTHO, 0 in perspective.
    vec4 camAxis;
    // x = highlighted net index, -1 for none.
    ivec4 highlight;
} push;

// Net highlighting. The chosen net keeps its colour and gains a warm lift;
// everything else desaturates toward the laminate so the signal reads at a
// glance across layers. Returns the albedo unchanged when nothing is picked,
// so a board with no highlight is bit-identical to before.
// Net highlight colour: a saturated red. Copper is gold and the laminate
// green, so red is the one hue that cannot be mistaken for either. Keep in
// step with pathtrace.comp, which uses the same colour as an EMITTER.
const vec3 kNetGlow = vec3(1.0, 0.09, 0.06);

// This fragment's highlight colour: rgb with a = 1 when it belongs to a
// highlighted net, a = 0 otherwise. The raster stage only gets
// gl_PrimitiveID, which restarts every draw, so the material's triangle base
// rebases it into the global per-triangle net table.
// The chase. Time arrives in milliseconds in highlight.z, and highlight.w is
// non-zero when the animation is on.
//
// Two acts, which is what makes a net readable rather than merely visible.
// First a WIPE: the net starts dark and a bright head travels from one end to
// the other, so you see which way the signal runs and where it terminates --
// a static glow can't tell you that. Once the head arrives, it hands over to
// a gradient cycling along the same axis forever, which keeps the direction
// legible without the eye having to re-find the net.
//
// Returns a multiplier on the net's colour, so an un-animated highlight is
// just this function returning 1.
float netChase(float phase) {
    if (push.highlight.w == 0) return 1.0;
    const float t = float(push.highlight.z) * 0.001;

    const float kWipe = 1.1;   // seconds for the head to run the net
    if (t < kWipe) {
        const float head = t / kWipe;
        if (phase > head) return 0.0;               // not reached yet
        // Bright at the head, settling to full behind it.
        return mix(2.4, 1.0, clamp((head - phase) * 5.0, 0.0, 1.0));
    }

    // Travelling gradient. 1.5 cycles across the net reads as motion without
    // turning into a barber pole on a long run.
    //
    // The floor has to go WELL below 1, not just dip a little: the highlight
    // is already multiplied by the glow strength (~2.5 by default), so any
    // trough above ~0.4 still clips past white and the whole cycle renders as
    // one flat saturated colour -- measurably identical frames. 0.12 at the
    // trough is what makes the band actually read as moving.
    const float g = fract(phase * 1.5 - (t - kWipe) * 0.55);
    return 0.12 + 0.88 * (0.5 + 0.5 * cos(6.2831853 * g));
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

// Everything NOT on the highlighted net desaturates and drops well below it,
// so the eye lands on the net rather than having to hunt for it.
vec3 applyNetHighlight(vec3 albedo) {
    if (push.highlight.x < 0) return albedo;
    return mix(albedo, vec3(dot(albedo, vec3(0.299, 0.587, 0.114))), 0.85) *
           0.42;
}

// The view vector, and the distance used to scale the light rig.
//
// Perspective: both come from the eye POINT. Orthographic: every ray is
// PARALLEL, so the direction is constant and the eye point is meaningless --
// worse, a fragment at or behind the eye plane (which a parallel projection
// still shows) yields a reversed vector, which flips the normal and swings the
// camera-relative key/fill lights away, collapsing the surface to ambient. That
// was the black near edge on a zoomed-in orthographic board.
vec3 viewVector(vec3 worldPos) {
    if (push.camAxis.w > 0.0) return -push.camAxis.xyz;
    return normalize(push.cameraPos.xyz - worldPos);
}
float viewScale(vec3 worldPos) {
    if (push.camAxis.w > 0.0) return push.camAxis.w;
    return length(push.cameraPos.xyz - worldPos);
}

// How transparent the substrate goes at a full peel. The laminate is what hides
// the copper, so fading it is what makes an exploded view legible -- but it must
// stay more solid than the mask (0.72) or the board reads as glass.
const float kSubstratePeelAlpha = 0.42;

void main() {
    Material m = materialTable.materials[inMaterial];

    // A highlighted net is EMISSIVE: output the glow directly and skip
    // shading entirely. Running it through the lighting rig instead let the
    // white specular and environment terms wash the red out to salmon, and
    // put the trace back in shadow under a component -- the opposite of what
    // "follow this signal" needs. Matches the path tracer, which adds the
    // same colour as radiance.
    // Zero rgb means the chase hasn't reached this stretch yet; fall through
    // so it shades like the rest of the dimmed board rather than reading as a
    // black gap cut out of the copper.
    const vec4 hl = netHighlight();
    if (hl.a > 0.0 && any(greaterThan(hl.rgb, vec3(0.002)))) {
        // Glow strength arrives in hundredths; scaled down relative to the
        // path tracer because raster has no exposure to soak it up -- past a
        // point it just clips to flat white and loses the hue.
        outColor = vec4(hl.rgb * (float(push.highlight.y) * 0.01) * 0.8, 1.0);
        return;
    }

    vec3 n = normalize(inNormal);
    vec3 viewDir = viewVector(inWorldPos);

    // Two-sided: we inspect geometry from underneath as often as above, and a
    // back-facing normal should read as shading rather than a black hole.
    if (dot(n, viewDir) < 0.0) n = -n;

    // Camera-relative lighting. A world-fixed key light means the board is lit
    // from above and the underside is permanently in shadow -- flipping to the
    // bottom view gives you a dark board, which is useless in an inspection
    // tool. The rig travels with the viewer, so whatever you look at is lit.
    //
    // The key is offset up-and-right of the eye rather than sitting on it: a
    // pure headlight (keyDir == viewDir) has no falloff across a facing surface
    // and reads completely flat.
    vec3 camRight = normalize(cross(viewDir, vec3(0.0, 0.0, 1.0)));
    // Looking straight down the Z axis makes that cross product degenerate.
    if (length(camRight) < 0.01) camRight = vec3(1.0, 0.0, 0.0);
    vec3 camUp = cross(camRight, viewDir);

    // The key is a POSITIONAL lamp, offset up-and-right of the eye, not a fixed
    // direction. A directional key hits every point on a flat face at the same
    // angle, so the top of an IC reads as one dead tone. A positioned lamp's
    // incidence sweeps across the face, giving a soft diffuse gradient. The
    // offset scales with view distance so the falloff looks the same at any zoom.
    float viewDist = viewScale(inWorldPos);
    // Perspective gets the positional lamp. ORTHOGRAPHIC gets a DIRECTIONAL
    // key -- the parallel-projection analogue. A positional lamp is placed at
    // eye + offset*viewDist, and in ortho "zoomed in" means a SMALL orbit
    // distance while still framing a wide area, so that lamp sinks down over
    // the board and every outward-facing edge wall falls into its shadow --
    // the board's near edge went black. A directional key cannot land inside
    // the scene, so edges stay lit at any zoom.
    vec3 keyDir;
    float keyDist;
    if (push.camAxis.w > 0.0) {
        keyDir = normalize(-push.camAxis.xyz + camRight * 0.55 + camUp * 0.55);
        keyDist = 1.0e4;  // effectively infinite, for the shadow ray
    } else {
        vec3 keyPos =
            push.cameraPos.xyz + (camRight * 0.55 + camUp * 0.55) * viewDist;
        keyDir = normalize(keyPos - inWorldPos);
        keyDist = length(keyPos - inWorldPos);
    }
    vec3 fillDir = normalize(viewDir - camRight * 0.5 - camUp * 0.25);

    float key = max(dot(n, keyDir), 0.0);
    float fill = max(dot(n, fillDir), 0.0);

    // Weights sum to 1.0 so a fully lit surface reaches its albedo and no
    // further -- summing past 1 clips the bright materials into flat white.
    float diffuse = 0.15 + 0.65 * key + 0.20 * fill;

    vec3 h = normalize(keyDir + viewDir);
    float roughness = clamp(m.params.x, 0.05, 1.0);
    float specPower = 2.0 / (roughness * roughness) - 2.0;
    float spec = pow(max(dot(n, h), 0.0), specPower) * (1.0 - roughness);

    // Fresnel rim: a soft grazing-angle sheen (Schlick). It gives silhouettes and
    // curved bodies -- capacitor cases, IC edges -- a gradient toward the rim
    // instead of a hard tone step, and lifts flat dark parts off the background.
    float fresnel = pow(1.0 - max(dot(n, viewDir), 0.0), 4.0);

    // Metals reflect their own colour and much more strongly than dielectrics, so
    // the specular is tinted toward the albedo and weighted up by metallic.
    vec3 specTint = mix(vec3(1.0), m.albedo.rgb, m.params.y);

    // Cheap environment reflection for metals: the reflected ray looking "up"
    // sees a bright sky, so a flat pad facing up reads as shiny plated metal from
    // any angle, not only where the key highlight happens to land. Weighted by
    // metallic, so gold pads pop while the matte IC bodies stay matte.
    vec3 refl = reflect(-viewDir, n);
    float envUp = clamp(refl.z * 0.5 + 0.5, 0.0, 1.0);
    vec3 env = mix(vec3(0.22), vec3(1.05), envUp);

    vec3 baseAlbedo = applyNetHighlight(m.albedo.rgb);
    vec3 lit = baseAlbedo * diffuse
             + specTint * spec * mix(0.12, 1.3, m.params.y)
             + specTint * env * (m.params.y * 0.35)
             + vec3(fresnel) * 0.08;

    // Materials flagged to fade (the substrate) go from fully solid at rest to
    // partly transparent as the stack peels, so the inner copper is visible
    // through the laminate. At peel 0 this is exactly 1.0, i.e. unchanged.
    float alpha = mix(m.albedo.a,
                      mix(m.albedo.a, kSubstratePeelAlpha, push.params.w),
                      m.params.w);

    outColor = vec4(lit, alpha);
}
