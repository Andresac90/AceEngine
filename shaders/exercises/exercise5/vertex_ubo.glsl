#version 410

layout(location = 0) in vec3 vertex_position;
layout(location = 1) in vec3 vertex_normal;

// UBO for camera matrices (shared across all shaders)
layout(std140) uniform CameraMatrices {
    mat4 view;
    mat4 proj;
};

// Model matrix is still a regular uniform (changes per object)
uniform mat4 model;

out vec3 position_eye;
out vec3 normal_eye;

void main() {
    position_eye = vec3(view * model * vec4(vertex_position, 1.0));
    normal_eye = vec3(view * model * vec4(vertex_normal, 0.0));
    gl_Position = proj * vec4(position_eye, 1.0);
}