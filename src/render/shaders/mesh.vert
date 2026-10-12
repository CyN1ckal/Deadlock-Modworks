#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec3 in_normal;

layout(push_constant) uniform Push {
    mat4 view_projection;
    vec4 eye;
    vec4 color;
} push;

layout(location = 0) out vec3 out_normal;
layout(location = 1) out vec3 out_world;

void main() {
    out_normal = in_normal;
    out_world = in_position;
    gl_Position = push.view_projection * vec4(in_position, 1.0);
}
