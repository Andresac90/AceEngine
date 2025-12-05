#version 410

layout(location = 0) in vec3 vp;

uniform mat4 proj;
uniform mat4 view;

out vec3 texcoords;

void main() {
    texcoords = vp;
    
    // Remove translation from view matrix (only keep rotation)
    mat4 view_no_translation = mat4(mat3(view));
    
    // Transform skybox
    gl_Position = proj * view_no_translation * vec4(vp, 1.0);
}