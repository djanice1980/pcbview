#version 450

// Vertex layout matches geom::Vertex exactly: position then normal, both vec3.
layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;

// Must match board.frag byte for byte -- one push block spans both stages.
layout(push_constant) uniform Push {
    mat4 viewProj;
    vec4 cameraPos;
    // x = mm per stage, y = eased progress, z = max |rank|,
    // w = peel amount 0..1 (normalised progress)
    vec4 params;
    // Fragment-stage only (camera forward + ortho orbit distance), declared
    // here because one push block spans both stages.
    vec4 camAxis;
} push;

struct Material {
    vec4 albedo;  // rgb + opacity
    // x = roughness, y = metallic, z = explode rank,
    // w = 1 if this material turns translucent as the board peels
    vec4 params;
};

// Same bindless table the fragment stage uses. Read here only for the explode
// rank, which is per-part static data -- so exploding costs one push constant
// per frame rather than re-uploading geometry or materials.
layout(std430, set = 0, binding = 0) readonly buffer Materials {
    Material materials[];
} materialTable;

layout(location = 0) out vec3 outNormal;
layout(location = 1) out vec3 outWorldPos;
layout(location = 2) flat out uint outMaterial;

void main() {
    // RT-readiness rule 3: the material is addressed by instance ID, never by a
    // per-draw binding. A closest-hit shader gets gl_InstanceCustomIndexEXT and
    // nothing else, so the raster path resolves materials the same way now --
    // that keeps phase 4 additive instead of a rewrite.
    outMaterial = gl_InstanceIndex;

    // Exploded view, peeled outside-in one ring at a time.
    //
    // A layer's ring R = |rank| is its distance from the board's mid-plane. The
    // OUTERMOST ring (R == maxRank) starts moving at progress 0; each ring
    // inwards waits one full stage longer. Once a ring starts it never stops, so
    // outer layers keep flying away while the next one lifts.
    //
    //     travel(R) = max(progress - (maxRank - R), 0) * mmPerStage
    //
    // The dwell that lets you stop and look lives in the easing of `progress` on
    // the CPU, not here -- see VulkanWindow::easedExplodeProgress().
    //
    // Rank is signed, so the two halves of the stack peel in opposite directions
    // and the board opens symmetrically. Rank 0 (the centre part) has sign 0 and
    // never moves, which is what anchors the whole thing.
    float rank = materialTable.materials[gl_InstanceIndex].params.z;
    float ring = abs(rank);
    float travel = max(push.params.y - (push.params.z - ring), 0.0) * push.params.x;

    vec3 pos = inPosition;
    pos.z += sign(rank) * travel;

    outNormal = inNormal;
    outWorldPos = pos;
    gl_Position = push.viewProj * vec4(pos, 1.0);
}
