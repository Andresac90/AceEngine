#version 410

in vec2 st;

uniform sampler2D font_atlas;
uniform vec4 text_color;

out vec4 frag_colour;

void main() {
    // Sample the font atlas (white letters with alpha)
    vec4 texel = texture(font_atlas, st);
    
    // Discard fully transparent pixels
    if (texel.a < 0.01) {
        discard;
    }
    
    // Apply text color, keeping alpha from texture
    frag_colour = vec4(text_color.rgb, texel.a * text_color.a);
}