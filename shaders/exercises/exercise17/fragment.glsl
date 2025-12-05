#version 410

in vec2 st;

uniform sampler2D sprite_sheet;

out vec4 frag_colour;

void main() {
    vec4 texel = texture(sprite_sheet, st);
    
    // Discard fully transparent pixels
    if (texel.a < 0.01) {
        discard;
    }
    
    frag_colour = texel;
}