#version 410

layout(location = 0) in vec3 vertex_position;
layout(location = 1) in vec3 vertex_normal;
layout(location = 2) in vec2 vertex_texcoord;

uniform mat4 proj;
uniform mat4 view;
uniform mat4 model;

out vec2 st;

void main() {
    st = vertex_texcoord;
    gl_Position = proj * view * model * vec4(vertex_position, 1.0);
}