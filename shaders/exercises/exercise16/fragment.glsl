#version 410

in vec2 st;

uniform sampler2D gui_texture;
uniform int has_texture;
uniform vec4 panel_color;

out vec4 frag_colour;

void main() {
    if (has_texture == 1) {
        vec4 texel = texture(gui_texture, st);
        // Discard transparent pixels
        if (texel.a < 0.1) {
            discard;
        }
        frag_colour = texel;
    } else {
        // Solid color panel
        frag_colour = panel_color;
    }
}