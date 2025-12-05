#version 410

layout (points) in;
layout (triangle_strip, max_vertices = 4) out;

uniform mat4 proj;

in float billboard_size[];
out vec2 texcoords;

void main() {
    // Get the position from vertex shader (already in view space)
    vec4 center = gl_in[0].gl_Position;
    float size = billboard_size[0];
    
    // Create a quad facing the camera
    // In view space, camera looks down -Z, so quad faces +Z
    
    // Bottom-left
    gl_Position = proj * (center + vec4(-size, -size, 0.0, 0.0));
    texcoords = vec2(0.0, 0.0);
    EmitVertex();
    
    // Bottom-right
    gl_Position = proj * (center + vec4(size, -size, 0.0, 0.0));
    texcoords = vec2(1.0, 0.0);
    EmitVertex();
    
    // Top-left
    gl_Position = proj * (center + vec4(-size, size, 0.0, 0.0));
    texcoords = vec2(0.0, 1.0);
    EmitVertex();
    
    // Top-right
    gl_Position = proj * (center + vec4(size, size, 0.0, 0.0));
    texcoords = vec2(1.0, 1.0);
    EmitVertex();
    
    EndPrimitive();
}