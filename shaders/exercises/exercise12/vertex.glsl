#version 410

layout(location = 0) in vec3 vertex_position;
layout(location = 1) in vec3 vertex_normal;
layout(location = 2) in vec2 texture_coord;
layout(location = 3) in vec4 vtangent;  // xyz = tangent, w = handedness

uniform mat4 model;
uniform mat4 view;
uniform mat4 proj;
uniform vec3 light_dir_world;  // Light direction in world space
uniform vec3 cam_pos_world;    // Camera position in world space

out vec2 st;
out vec3 view_dir_tan;   // View direction in tangent space
out vec3 light_dir_tan;  // Light direction in tangent space

void main() {
    gl_Position = proj * view * model * vec4(vertex_position, 1.0);
    st = texture_coord;
    
    // Calculate bi-tangent from normal and tangent
    // Multiply by handedness (w component) to correct orientation
    vec3 bitangent = cross(vertex_normal, vtangent.xyz) * vtangent.w;
    
    // Transform camera position and light direction to local space
    vec3 cam_pos_local = vec3(inverse(model) * vec4(cam_pos_world, 1.0));
    vec3 light_dir_local = vec3(inverse(model) * vec4(light_dir_world, 0.0));
    
    // Calculate view direction in local space
    vec3 view_dir_local = normalize(cam_pos_local - vertex_position);
    
    // Transform view direction to tangent space
    // This is equivalent to multiplying by inverse tangent matrix
    view_dir_tan = vec3(
        dot(vtangent.xyz, view_dir_local),
        dot(bitangent, view_dir_local),
        dot(vertex_normal, view_dir_local)
    );
    
    // Transform light direction to tangent space
    light_dir_tan = vec3(
        dot(vtangent.xyz, light_dir_local),
        dot(bitangent, light_dir_local),
        dot(vertex_normal, light_dir_local)
    );
}