#version 410 core

in float opacity;

uniform sampler2D particle_tex;
uniform int use_texture;
uniform vec4 particle_color;

out vec4 frag_colour;

void main() {
    if (use_texture == 1) {
        // Use gl_PointCoord for automatic texture coordinates
        vec4 texel = texture(particle_tex, gl_PointCoord);
        
        // Discard fully transparent pixels
        if (texel.a < 0.01) {
            discard;
        }
        
        // Apply particle color and opacity
        frag_colour = vec4(particle_color.rgb * texel.rgb, opacity * texel.a);
    } else {
        // Simple colored particle
        frag_colour = vec4(particle_color.rgb, opacity);
    }
}