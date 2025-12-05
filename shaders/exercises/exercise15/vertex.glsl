#version 410

layout(location = 0) in vec3 vp_loc;

out vec3 controlpoint_world;

void main() {
    // Just pass control points through to tessellation control shader
    controlpoint_world = vp_loc;
}