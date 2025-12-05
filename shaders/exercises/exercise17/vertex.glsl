#version 410

layout(location = 0) in vec3 vp;

uniform mat4 model;
uniform mat4 view;
uniform mat4 proj;
uniform vec2 st_offset;     // Top-left corner of sprite in atlas
uniform vec2 sprite_scale;  // Size of one sprite (1/cols, 1/rows)

out vec2 st;

void main() {
    // Convert vertex position (-1 to 1) to texture coords (0 to 1)
    // Note: Flip T coordinate because texture origin is bottom-left
    st = (vp.xy + 1.0) * 0.5;
    st.t = 1.0 - st.t;  // Flip vertically
    
    // Scale to sprite size and offset to correct sprite position
    st = st * sprite_scale + st_offset;
    
    gl_Position = proj * view * model * vec4(vp, 1.0);
}