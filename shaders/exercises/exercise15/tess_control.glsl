#version 410

// Number of control points per patch (3 for triangles)
layout (vertices = 3) out;

// Input from vertex shader
in vec3 controlpoint_world[];

// Output to tessellation evaluation shader
out vec3 evaluationpoint_world[];

// Tessellation levels (controlled by keyboard)
uniform float tess_level_inner;
uniform float tess_level_outer;

void main() {
    // Pass control points through
    evaluationpoint_world[gl_InvocationID] = controlpoint_world[gl_InvocationID];
    
    // Set tessellation levels (only needs to be done once per patch)
    if (gl_InvocationID == 0) {
        // Inner level - subdivisions inside the triangle
        gl_TessLevelInner[0] = tess_level_inner;
        
        // Outer levels - subdivisions along each edge
        gl_TessLevelOuter[0] = tess_level_outer;  // Edge 0
        gl_TessLevelOuter[1] = tess_level_outer;  // Edge 1
        gl_TessLevelOuter[2] = tess_level_outer;  // Edge 2
    }
}