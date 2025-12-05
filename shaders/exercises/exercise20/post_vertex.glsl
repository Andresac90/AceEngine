#version 410 core

layout(location = 0) in vec2 vp;  // Vertex position
layout(location = 1) in vec2 vt;  // Texture coordinates

out vec2 st;

void main() {
    st = vt;
    gl_Position = vec4(vp, 0.0, 1.0);
}