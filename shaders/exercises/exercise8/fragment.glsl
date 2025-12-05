#version 410

in vec2 st;

uniform sampler2D first_texture;
uniform sampler2D second_texture;

out vec4 frag_colour;

void main() {
    // Sample both textures
    vec4 first_sample = texture(first_texture, st);
    vec4 second_sample = texture(second_texture, st);
    
    // Blend based on horizontal position (left to right)
    // Left side (st.s = 0.0) shows first texture
    // Right side (st.s = 1.0) shows second texture
    frag_colour = mix(first_sample, second_sample, st.s);
}