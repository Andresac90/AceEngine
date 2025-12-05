#version 410

layout(location = 0) in vec3 vertex_position;
layout(location = 1) in vec3 vertex_normal;
layout(location = 2) in vec2 vertex_texcoord;

uniform mat4 proj;
uniform mat4 view;
uniform mat4 model;

out vec3 position_eye;
out vec3 normal_eye;
out vec2 st;

void main() {
    position_eye = vec3(view * model * vec4(vertex_position, 1.0));
    normal_eye = vec3(view * model * vec4(vertex_normal, 0.0));
    st = vertex_texcoord;
    gl_Position = proj * vec4(position_eye, 1.0);
}