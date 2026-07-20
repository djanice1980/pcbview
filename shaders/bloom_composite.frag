#version 450

// Bloom, pass 2: add the blurred, thresholded image back over the scene.
//
// Drawn with additive blending straight onto the scene target, sampling the
// quarter-resolution bloom texture -- the bilinear upsample is the second
// half of the blur, which is why one downsample level is enough for the halo
// around a glowing net.

layout(set = 0, binding = 0) uniform sampler2D bloomTex;

layout(push_constant) uniform Pc {
    // xy = 1/destination size, z = intensity, w = unused
    vec4 params;
} pc;

layout(location = 0) out vec4 outColor;

void main() {
    const vec2 uv = gl_FragCoord.xy * pc.params.xy;
    // A 4-tap tent on the way back up, so the halo has no visible blockiness
    // from the quarter-res source.
    const vec2 t = pc.params.xy;
    vec3 b = texture(bloomTex, uv).rgb * 4.0;
    b += texture(bloomTex, uv + vec2(-t.x, -t.y)).rgb;
    b += texture(bloomTex, uv + vec2( t.x, -t.y)).rgb;
    b += texture(bloomTex, uv + vec2(-t.x,  t.y)).rgb;
    b += texture(bloomTex, uv + vec2( t.x,  t.y)).rgb;
    outColor = vec4(b * (1.0 / 8.0) * pc.params.z, 1.0);
}
