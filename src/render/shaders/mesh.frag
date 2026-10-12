#version 450

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 eye;
    vec4 color;
    // 0: push.color, 1: the vertex's dominant-bone color.
    uint mode;
} push;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec3 in_world;
layout(location = 2) in vec3 in_bone_color;

layout(location = 0) out vec4 out_color;

void main() {
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
    vec3 base = push.mode == 1u ? pow(in_bone_color, vec3(2.2)) : push.color.rgb;
    vec3 linear = base * (fill + 0.75 * key) + rim;

    // The target is UNORM and ImGui shows it as-is, so encode for display here.
    out_color = vec4(pow(clamp(linear, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
