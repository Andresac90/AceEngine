#version 410 core

in vec2 st;

uniform sampler2D tex;
uniform int effect_mode;

out vec4 frag_colour;

void main() {
    vec3 colour = texture(tex, st).rgb;
    
    if (effect_mode == 0) {
        // No effect
        frag_colour = vec4(colour, 1.0);
    }
    else if (effect_mode == 1) {
        // Split-screen invert (right side)
        if (st.s >= 0.5) {
            colour = 1.0 - colour;
        }
        frag_colour = vec4(colour, 1.0);
    }
    else if (effect_mode == 2) {
        // Grayscale
        float gray = dot(colour, vec3(0.299, 0.587, 0.114));
        frag_colour = vec4(vec3(gray), 1.0);
    }
    else if (effect_mode == 3) {
        // Sepia tone
        vec3 sepia;
        sepia.r = dot(colour, vec3(0.393, 0.769, 0.189));
        sepia.g = dot(colour, vec3(0.349, 0.686, 0.168));
        sepia.b = dot(colour, vec3(0.272, 0.534, 0.131));
        frag_colour = vec4(sepia, 1.0);
    }
    else if (effect_mode == 4) {
        // Edge detection (Sobel)
        float offset = 1.0 / 300.0;
        vec2 offsets[9] = vec2[](
            vec2(-offset,  offset), vec2( 0.0,    offset), vec2( offset,  offset),
            vec2(-offset,  0.0),    vec2( 0.0,    0.0),    vec2( offset,  0.0),
            vec2(-offset, -offset), vec2( 0.0,   -offset), vec2( offset, -offset)
        );
        
        float kernel[9] = float[](
            -1, -1, -1,
            -1,  8, -1,
            -1, -1, -1
        );
        
        vec3 sample_tex[9];
        for(int i = 0; i < 9; i++) {
            sample_tex[i] = texture(tex, st.st + offsets[i]).rgb;
        }
        
        vec3 col = vec3(0.0);
        for(int i = 0; i < 9; i++) {
            col += sample_tex[i] * kernel[i];
        }
        
        frag_colour = vec4(col, 1.0);
    }
    else if (effect_mode == 5) {
        // Blur
        float offset = 1.0 / 300.0;
        vec2 offsets[9] = vec2[](
            vec2(-offset,  offset), vec2( 0.0,    offset), vec2( offset,  offset),
            vec2(-offset,  0.0),    vec2( 0.0,    0.0),    vec2( offset,  0.0),
            vec2(-offset, -offset), vec2( 0.0,   -offset), vec2( offset, -offset)
        );
        
        float kernel[9] = float[](
            1.0/16, 2.0/16, 1.0/16,
            2.0/16, 4.0/16, 2.0/16,
            1.0/16, 2.0/16, 1.0/16
        );
        
        vec3 sample_tex[9];
        for(int i = 0; i < 9; i++) {
            sample_tex[i] = texture(tex, st.st + offsets[i]).rgb;
        }
        
        vec3 col = vec3(0.0);
        for(int i = 0; i < 9; i++) {
            col += sample_tex[i] * kernel[i];
        }
        
        frag_colour = vec4(col, 1.0);
    }
}