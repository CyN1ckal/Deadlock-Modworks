#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_bone_color;
layout(location = 3) in vec2 in_texcoord;
layout(location = 4) in uvec4 in_bones;
layout(location = 5) in vec4 in_weights;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 eye;
    vec4 color;
    uint mode;
    uint flags;
} push;

// Three rows of a 3x4 matrix per model bone.
layout(std430, set = 1, binding = 0) readonly buffer Bones {
    vec4 rows[];
} bones;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec3 out_world;
layout(location = 2) out vec3 out_bone_color;
layout(location = 3) out vec2 out_texcoord;

void main() {
    vec3 position = in_position;
    vec3 normal = in_normal;
    float total = dot(in_weights, vec4(1.0));
    if ((push.flags & 2u) != 0u && total > 0.0) {
        vec4 point = vec4(in_position, 1.0);
        vec4 direction = vec4(in_normal, 0.0);
        position = vec3(0.0);
        normal = vec3(0.0);
        for (int i = 0; i < 4; ++i) {
            float weight = in_weights[i] / total;
            if (weight <= 0.0) {
                continue;
            }
            uint row = in_bones[i] * 3u;
            vec4 r0 = bones.rows[row];
            vec4 r1 = bones.rows[row + 1u];
            vec4 r2 = bones.rows[row + 2u];
            position += weight * vec3(dot(r0, point), dot(r1, point), dot(r2, point));
            normal += weight * vec3(dot(r0, direction), dot(r1, direction), dot(r2, direction));
        }
    }
    out_normal = normal;
    out_world = position;
    out_bone_color = in_bone_color.rgb;
    out_texcoord = in_texcoord;
    gl_Position = push.view_projection * vec4(position, 1.0);
}
