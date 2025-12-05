#version 410

in vec2 texcoords;

uniform sampler2D billboard_texture;
uniform int use_texture;

out vec4 frag_colour;

void main() {
    if (use_texture == 1) {
        vec4 texel = texture(billboard_texture, texcoords);
        
        // Discard transparent pixels
        if (texel.a < 0.1) {
            discard;
        }
        
        frag_colour = texel;
    } else {
        // Solid color with circular shape
        vec2 centered = texcoords * 2.0 - 1.0;
        float dist = length(centered);
        
        if (dist > 1.0) {
            discard;
        }
        
        // Color based on distance (gradient)
        vec3 color = mix(vec3(1.0, 1.0, 0.0), vec3(1.0, 0.5, 0.0), dist);
        frag_colour = vec4(color, 1.0);
    }
}