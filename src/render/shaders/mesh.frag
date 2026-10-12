#version 450

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 eye;
    vec4 color;
    // 0: push.color, 1: the vertex's dominant-bone color, 2: the color map.
    uint mode;
    // Bit 0: discard texels with alpha below one half.
    uint flags;
} push;

layout(set = 0, binding = 0) uniform sampler2D color_map;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec3 in_world;
layout(location = 2) in vec3 in_bone_color;
layout(location = 3) in vec2 in_texcoord;

layout(location = 0) out vec4 out_color;

void main() {
    vec4 texel = push.mode == 2u ? texture(color_map, in_texcoord) : vec4(1.0);
    if ((push.flags & 1u) != 0u && texel.a < 0.5) {
        discard;
    }

    vec3 to_eye = normalize(push.eye.xyz - in_world);
    vec3 normal = normalize(in_normal);
    // Mods do not agree on winding, so light whichever side faces the camera.
    if (dot(normal, to_eye) < 0.0) {
        normal = -normal;
    }

    // Headlight key plus a sky/ground fill so silhouettes stay readable from any angle.
    float key = max(dot(normal, to_eye), 0.0);
    vec3 fill = mix(vec3(0.20, 0.18, 0.16), vec3(0.42, 0.45, 0.50), normal.z * 0.5 + 0.5);
    float rim = pow(1.0 - key, 4.0) * 0.15;
    // Bone colors are display-encoded; decode so the output encoding round-trips them.
    // Color maps use sRGB formats, so sampling already returns linear values.
    vec3 base = push.mode == 1u ? pow(in_bone_color, vec3(2.2)) : push.mode == 2u ? texel.rgb : push.color.rgb;
    vec3 linear = base * (fill + 0.75 * key) + rim;

    // The target is UNORM and ImGui shows it as-is, so encode for display here.
    // Alpha drives blending for translucent materials; the solid pipeline does not write it.
    out_color = vec4(pow(clamp(linear, 0.0, 1.0), vec3(1.0 / 2.2)), push.mode == 2u ? texel.a : 1.0);
}
