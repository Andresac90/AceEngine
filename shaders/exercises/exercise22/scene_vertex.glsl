#version 410 core

layout(location = 0) in vec3 vp;
layout(location = 1) in vec3 vn;

uniform mat4 P, V, M;
uniform mat4 caster_P, caster_V;

out vec3 normal;
out vec4 st_shadow;

void main() {
    // Normal rendering
    gl_Position = P * V * M * vec4(vp, 1.0);
    normal = vn;
    
    // Create shadow map texture coordinate
    st_shadow = caster_P * caster_V * M * vec4(vp, 1.0);
    
    // Convert from clip space to texture coordinates
    st_shadow.xyz /= st_shadow.w;  // Perspective division
    st_shadow.xyz += 1.0;           // -1:1 to 0:2
    st_shadow.xyz *= 0.5;           // 0:2 to 0:1
}