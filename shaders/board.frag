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
};

layout(std430, set = 0, binding = 0) readonly buffer Materials {
    Material materials[];
} materialTable;

// Must match board.vert byte for byte -- one push block spans both stages.
layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;
    // x = mm per stage, y = eased progress, z = max |rank|,
    // w = peel amount 0..1 (normalised progress)
    vec4 params;
} push;

// How transparent the substrate goes at a full peel. The laminate is what hides
// the copper, so fading it is what makes an exploded view legible -- but it must
// stay more solid than the mask (0.72) or the board reads as glass.
const float kSubstratePeelAlpha = 0.42;

void main() {
    Material m = materialTable.materials[inMaterial];

    vec3 n = normalize(inNormal);
    vec3 viewDir = normalize(push.cameraPos.xyz - inWorldPos);

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
    float viewDist = length(push.cameraPos.xyz - inWorldPos);
    vec3 keyPos = push.cameraPos.xyz + (camRight * 0.55 + camUp * 0.55) * viewDist;
    vec3 keyDir = normalize(keyPos - inWorldPos);
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

    vec3 lit = m.albedo.rgb * diffuse + vec3(spec) * mix(0.1, 0.6, m.params.y)
             + vec3(fresnel) * 0.08;

    // Materials flagged to fade (the substrate) go from fully solid at rest to
    // partly transparent as the stack peels, so the inner copper is visible
    // through the laminate. At peel 0 this is exactly 1.0, i.e. unchanged.
    float alpha = mix(m.albedo.a,
                      mix(m.albedo.a, kSubstratePeelAlpha, push.params.w),
                      m.params.w);

    outColor = vec4(lit, alpha);
}
