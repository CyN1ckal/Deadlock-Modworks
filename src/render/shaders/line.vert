#version 450

layout(location = 0) in vec3 in_position;
layout(location = 1) in vec4 in_color;

layout(push_constant) uniform Push {
    mat4 view_projection;
} push;

layout(location = 0) out vec4 out_color;

void main() {
    out_color = in_color;
    gl_Position = push.view_projection * vec4(in_position, 1.0);
}
