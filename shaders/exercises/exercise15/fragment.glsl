#version 410

in vec3 world_pos;

out vec4 frag_colour;

uniform int color_mode;

void main() {
    if (color_mode == 0) {
        // White wireframe
        frag_colour = vec4(1.0, 1.0, 1.0, 1.0);
    } else {
        // Color based on position (makes it easier to see tessellation)
        vec3 color = world_pos * 0.5 + 0.5;
        frag_colour = vec4(color, 1.0);
    }
}