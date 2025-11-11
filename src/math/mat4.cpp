#include "math/mat4.h"
#include <iostream>
#include <cstring>
#include <cmath>
// vec3 implementation
vec3::vec3() {
    v[0] = v[1] = v[2] = 0.0f;
}

vec3::vec3(float x, float y, float z) {
    v[0] = x;
    v[1] = y;
    v[2] = z;
}

float& vec3::operator[](int index) {
    return v[index];
}

const float& vec3::operator[](int index) const {
    return v[index];
}

float vec3::length() const {
    return sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

// Vector operations
vec3 normalize(const vec3& v) {
    float len = v.length();
    if (len == 0.0f) return vec3(0, 0, 0);
    return vec3(v.v[0] / len, v.v[1] / len, v.v[2] / len);
}

vec3 cross(const vec3& a, const vec3& b) {
    return vec3(
        a.v[1] * b.v[2] - a.v[2] * b.v[1],
        a.v[2] * b.v[0] - a.v[0] * b.v[2],
        a.v[0] * b.v[1] - a.v[1] * b.v[0]
    );
}

vec3 subtract(const vec3& a, const vec3& b) {
    return vec3(a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2]);
}

vec3 add(const vec3& a, const vec3& b) {
    return vec3(a.v[0] + b.v[0], a.v[1] + b.v[1], a.v[2] + b.v[2]);
}

float dot(const vec3& a, const vec3& b) {
    return a.v[0] * b.v[0] + a.v[1] * b.v[1] + a.v[2] * b.v[2];
}

// mat4 implementation
mat4::mat4() {
    memset(m, 0, sizeof(m));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

float& mat4::at(int row, int col) {
    return m[col * 4 + row];
}

const float& mat4::at(int row, int col) const {
    return m[col * 4 + row];
}

void mat4::print() const {
    for (int row = 0; row < 4; row++) {
        std::cout << "[ ";
        for (int col = 0; col < 4; col++) {
            printf("%7.3f ", at(row, col));
        }
        std::cout << "]\n";
    }
}

mat4 identity_mat4() {
    return mat4();
}

mat4 translate(float x, float y, float z) {
    mat4 result;
    result.m[12] = x;
    result.m[13] = y;
    result.m[14] = z;
    return result;
}

mat4 translate(const vec3& v) {
    return translate(v.v[0], v.v[1], v.v[2]);
}

mat4 rotate_x(float deg) {
    mat4 result;
    float rad = deg * ONE_DEG_IN_RAD;
    float c = cosf(rad);
    float s = sinf(rad);
    
    result.m[5] = c;
    result.m[6] = s;
    result.m[9] = -s;
    result.m[10] = c;
    
    return result;
}

mat4 rotate_y(float deg) {
    mat4 result;
    float rad = deg * ONE_DEG_IN_RAD;
    float c = cosf(rad);
    float s = sinf(rad);
    
    result.m[0] = c;
    result.m[2] = -s;
    result.m[8] = s;
    result.m[10] = c;
    
    return result;
}

mat4 rotate_z(float deg) {
    mat4 result;
    float rad = deg * ONE_DEG_IN_RAD;
    float c = cosf(rad);
    float s = sinf(rad);
    
    result.m[0] = c;
    result.m[1] = s;
    result.m[4] = -s;
    result.m[5] = c;
    
    return result;
}

mat4 scale(float x, float y, float z) {
    mat4 result;
    result.m[0] = x;
    result.m[5] = y;
    result.m[10] = z;
    return result;
}

mat4 operator*(const mat4& a, const mat4& b) {
    mat4 result;
    
    for (int col = 0; col < 4; col++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.0f;
            for (int k = 0; k < 4; k++) {
                sum += a.at(row, k) * b.at(k, col);
            }
            result.at(row, col) = sum;
        }
    }
    
    return result;
}

mat4 perspective(float fovy, float aspect, float near, float far) {
    mat4 result;
    memset(result.m, 0, sizeof(result.m));
    
    float fov_rad = fovy * ONE_DEG_IN_RAD;
    float range = tanf(fov_rad * 0.5f) * near;
    float sx = (2.0f * near) / (range * aspect + range * aspect);
    float sy = near / range;
    float sz = -(far + near) / (far - near);
    float pz = -(2.0f * far * near) / (far - near);
    
    result.m[0] = sx;
    result.m[5] = sy;
    result.m[10] = sz;
    result.m[11] = -1.0f;
    result.m[14] = pz;
    
    return result;
}

mat4 look_at(const vec3& cam_pos, const vec3& target_pos, const vec3& up) {
    vec3 forward = normalize(subtract(cam_pos, target_pos));
    vec3 right = normalize(cross(up, forward));
    vec3 cam_up = cross(forward, right);
    
    mat4 result;
    result.m[0] = right.v[0];
    result.m[4] = right.v[1];
    result.m[8] = right.v[2];
    
    result.m[1] = cam_up.v[0];
    result.m[5] = cam_up.v[1];
    result.m[9] = cam_up.v[2];
    
    result.m[2] = forward.v[0];
    result.m[6] = forward.v[1];
    result.m[10] = forward.v[2];
    
    result.m[12] = -dot(right, cam_pos);
    result.m[13] = -dot(cam_up, cam_pos);
    result.m[14] = -dot(forward, cam_pos);
    
    return result;
}

// Frustum culling implementation
Frustum extract_frustum(const mat4& proj_view) {
    Frustum frustum;
    const float* m = proj_view.m;
    
    // Left plane
    frustum.planes[0].normal.v[0] = m[3] + m[0];
    frustum.planes[0].normal.v[1] = m[7] + m[4];
    frustum.planes[0].normal.v[2] = m[11] + m[8];
    frustum.planes[0].distance = m[15] + m[12];
    
    // Right plane
    frustum.planes[1].normal.v[0] = m[3] - m[0];
    frustum.planes[1].normal.v[1] = m[7] - m[4];
    frustum.planes[1].normal.v[2] = m[11] - m[8];
    frustum.planes[1].distance = m[15] - m[12];
    
    // Bottom plane
    frustum.planes[2].normal.v[0] = m[3] + m[1];
    frustum.planes[2].normal.v[1] = m[7] + m[5];
    frustum.planes[2].normal.v[2] = m[11] + m[9];
    frustum.planes[2].distance = m[15] + m[13];
    
    // Top plane
    frustum.planes[3].normal.v[0] = m[3] - m[1];
    frustum.planes[3].normal.v[1] = m[7] - m[5];
    frustum.planes[3].normal.v[2] = m[11] - m[9];
    frustum.planes[3].distance = m[15] - m[13];
    
    // Near plane
    frustum.planes[4].normal.v[0] = m[3] + m[2];
    frustum.planes[4].normal.v[1] = m[7] + m[6];
    frustum.planes[4].normal.v[2] = m[11] + m[10];
    frustum.planes[4].distance = m[15] + m[14];
    
    // Far plane
    frustum.planes[5].normal.v[0] = m[3] - m[2];
    frustum.planes[5].normal.v[1] = m[7] - m[6];
    frustum.planes[5].normal.v[2] = m[11] - m[10];
    frustum.planes[5].distance = m[15] - m[14];
    
    // Normalize planes
    for (int i = 0; i < 6; i++) {
        float length = frustum.planes[i].normal.length();
        if (length > 0.0f) {
            frustum.planes[i].normal.v[0] /= length;
            frustum.planes[i].normal.v[1] /= length;
            frustum.planes[i].normal.v[2] /= length;
            frustum.planes[i].distance /= length;
        }
    }
    
    return frustum;
}

bool point_in_frustum(const Frustum& frustum, const vec3& point) {
    for (int i = 0; i < 6; i++) {
        float dist = dot(frustum.planes[i].normal, point) + frustum.planes[i].distance;
        if (dist < 0.0f) {
            return false; // Point is outside this plane
        }
    }
    return true; // Point is inside all planes
}

bool sphere_in_frustum(const Frustum& frustum, const vec3& center, float radius) {
    for (int i = 0; i < 6; i++) {
        float dist = dot(frustum.planes[i].normal, center) + frustum.planes[i].distance;
        if (dist < -radius) {
            return false; // Sphere is completely outside this plane
        }
    }
    return true; // Sphere intersects or is inside frustum
}