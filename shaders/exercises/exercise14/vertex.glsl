#version 410

layout(location = 0) in vec3 vertex_position;
layout(location = 1) in float vertex_size;  // Size of billboard

uniform mat4 view;
uniform mat4 proj;

out float billboard_size;

void main() {
    // Just pass through to geometry shader
    // Don't apply projection yet - geometry shader will do that
    gl_Position = view * vec4(vertex_position, 1.0);
    billboard_size = vertex_size;
}