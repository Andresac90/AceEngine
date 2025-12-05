#version 410

layout(location = 0) in vec2 vp;  // 2D positions in clip space

uniform vec2 gui_scale;      // Scale to pixel size
uniform vec2 gui_position;   // Position in clip space

out vec2 st;

void main() {
    // Convert clip space coords (-1 to 1) to texture coords (0 to 1)
    st = (vp + 1.0) * 0.5;
    
    // Apply scale and position
    // W = 1.0 to avoid perspective division distortion!
    gl_Position = vec4(vp * gui_scale + gui_position, 0.0, 1.0);
}