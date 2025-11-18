#version 410

in vec2 texcoord;

uniform sampler2D basic_texture;

out vec4 frag_colour;

void main() {
    vec4 texel = texture(basic_texture, texcoord);
    frag_colour = texel;
}