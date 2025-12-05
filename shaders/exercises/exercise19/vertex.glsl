#version 410 core

layout(location = 0) in vec3 v_i;           // Initial velocity
layout(location = 1) in float start_time;    // When this particle starts

uniform mat4 view;
uniform mat4 proj;
uniform vec3 emitter_pos;           // Emitter position in world space
uniform float elapsed_system_time;  // Total elapsed time

out float opacity;

void main() {
    // Calculate elapsed time for THIS particle
    float t = elapsed_system_time - start_time;
    
    // Loop the animation every 3 seconds
    t = mod(t, 3.0);
    
    // Start at emitter position
    vec3 p = emitter_pos;
    
    // Apply gravity
    vec3 a = vec3(0.0, -1.0, 0.0);
    
    // Kinematics equation: p = p0 + v*t + 0.5*a*t²
    p += v_i * t + 0.5 * a * t * t;
    
    // Fade out over 3 seconds
    opacity = 1.0 - (t / 3.0);
    
    // Transform to clip space
    gl_Position = proj * view * vec4(p, 1.0);
    
    // Set point size in pixels
    gl_PointSize = 15.0;
}