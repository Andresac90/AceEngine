#version 410

layout(location = 0) in vec2 vp;  // 2D vertex position
layout(location = 1) in vec2 vt;  // Texture coordinates

uniform vec2 position;  // Text position in clip space
uniform float scale;    // Text scale

out vec2 st;

void main() {
    st = vt;
    // Position text in clip space with scaling
    gl_Position = vec4(vp * scale + position, 0.0, 1.0);
}