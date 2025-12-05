#version 410 core

in vec2 st;

uniform sampler2D depth_tex;

out vec4 frag_colour;

void main() {
    float depth = texture(depth_tex, st).r;
    frag_colour = vec4(vec3(depth), 1.0);
}