#version 410 core

layout(location = 0) in vec3 vertex_position;
layout(location = 1) in vec3 vertex_normal;
layout(location = 2) in vec2 vertex_texcoord;

uniform mat4 model;
uniform mat4 view;
uniform mat4 proj;

out vec3 normal;
out vec2 texcoord;
out vec3 frag_pos;

void main() {
    frag_pos = vec3(model * vec4(vertex_position, 1.0));
    normal = mat3(transpose(inverse(model))) * vertex_normal;
    texcoord = vertex_texcoord;
    
    gl_Position = proj * view * vec4(frag_pos, 1.0);
}