#version 410

// Tessellation mode: triangles, quads, or isolines
// Spacing: equal_spacing, fractional_even_spacing, fractional_odd_spacing
// Winding order: ccw (counter-clockwise) or cw (clockwise)
layout (triangles, equal_spacing, ccw) in;

// Input from tessellation control shader
in vec3 evaluationpoint_world[];

uniform mat4 view;
uniform mat4 proj;

out vec3 world_pos;

void main() {
    // gl_TessCoord contains barycentric coordinates for triangles
    // (x, y, z) where x + y + z = 1.0
    
    // Interpolate position using barycentric coordinates
    vec3 p0 = gl_TessCoord.x * evaluationpoint_world[0];
    vec3 p1 = gl_TessCoord.y * evaluationpoint_world[1];
    vec3 p2 = gl_TessCoord.z * evaluationpoint_world[2];
    
    vec3 pos = p0 + p1 + p2;
    
    world_pos = pos;
    gl_Position = proj * view * vec4(pos, 1.0);
}