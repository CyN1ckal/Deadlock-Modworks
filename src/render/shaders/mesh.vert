#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;
layout(location = 2) in vec4 in_bone_color;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 eye;
    vec4 color;
    uint mode;
} push;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec3 out_world;
layout(location = 2) out vec3 out_bone_color;

void main() {
    out_normal = in_normal;
    out_world = in_position;
    out_bone_color = in_bone_color.rgb;
    gl_Position = push.view_projection * vec4(in_position, 1.0);
}
