#version 450

// Bloom, pass 1: threshold + downsample in one go.
//
// Rendered at a quarter of the scene resolution, so the 13-tap tent kernel
// below covers a wide area of the source for very little work -- the
// downsample IS most of the blur. Only the part of each sample ABOVE the
// threshold contributes, so ordinary lit copper does not haze; a net glowing
// past white does.

layout(set = 0, binding = 0) uniform sampler2D srcTex;

layout(push_constant) uniform Pc {
    // xy = 1/destination size, z = threshold, w = unused
    vec4 params;
} pc;

layout(location = 0) out vec4 outColor;

vec3 overThreshold(vec3 c) {
    // Soft knee: subtract the threshold and keep what is left, so a source
    // just past the knee fades in rather than popping.
    return max(c - vec3(pc.params.z), vec3(0.0));
}

void main() {
    const vec2 uv = gl_FragCoord.xy * pc.params.xy;
    // Texel step in SOURCE space; the source is twice the destination here,
    // and sampling on half-texel offsets gives a free bilinear box on top of
    // the tent weights.
    const vec2 t = pc.params.xy * 0.5;

    vec3 sum = overThreshold(texture(srcTex, uv).rgb) * 4.0;
    sum += overThreshold(texture(srcTex, uv + vec2(-t.x, -t.y)).rgb) * 2.0;
    sum += overThreshold(texture(srcTex, uv + vec2( t.x, -t.y)).rgb) * 2.0;
    sum += overThreshold(texture(srcTex, uv + vec2(-t.x,  t.y)).rgb) * 2.0;
    sum += overThreshold(texture(srcTex, uv + vec2( t.x,  t.y)).rgb) * 2.0;
    sum += overThreshold(texture(srcTex, uv + vec2(-2.0 * t.x, 0.0)).rgb);
    sum += overThreshold(texture(srcTex, uv + vec2( 2.0 * t.x, 0.0)).rgb);
    sum += overThreshold(texture(srcTex, uv + vec2(0.0, -2.0 * t.y)).rgb);
    sum += overThreshold(texture(srcTex, uv + vec2(0.0,  2.0 * t.y)).rgb);

    outColor = vec4(sum * (1.0 / 16.0), 1.0);
}
