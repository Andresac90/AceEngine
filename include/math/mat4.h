#ifndef MAT4_H
#define MAT4_H

#include <cmath>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define ONE_DEG_IN_RAD (2.0 * M_PI) / 360.0 // 0.017444444

// 4x4 matrix in column-major order (OpenGL standard)
struct mat4 {
    float m[16];
    
    // Constructor - identity matrix by default
    mat4();
    
    // Access element at row i, column j
    float& at(int row, int col);
    const float& at(int row, int col) const;
    
    // Print matrix (for debugging)
    void print() const;
};

// 3D vector
struct vec3 {
    float v[3];
    
    vec3();
    vec3(float x, float y, float z);
    
    float& operator[](int index);
    const float& operator[](int index) const;
    
    float length() const;
};

// Identity matrix
mat4 identity_mat4();

// Translation matrix
mat4 translate(float x, float y, float z);
mat4 translate(const vec3& v);

// Rotation matrices
mat4 rotate_x(float deg);
mat4 rotate_y(float deg);
mat4 rotate_z(float deg);

// Scale matrix
mat4 scale(float x, float y, float z);

// Matrix multiplication
mat4 operator*(const mat4& a, const mat4& b);

// Perspective projection matrix
mat4 perspective(float fovy, float aspect, float near, float far);

// View matrix (look at)
mat4 look_at(const vec3& cam_pos, const vec3& target_pos, const vec3& up);

// Vector operations
vec3 normalize(const vec3& v);
vec3 cross(const vec3& a, const vec3& b);
vec3 subtract(const vec3& a, const vec3& b);
vec3 add(const vec3& a, const vec3& b);
float dot(const vec3& a, const vec3& b);

// Frustum culling
struct Plane {
    vec3 normal;
    float distance;
    
    Plane() : normal(0, 0, 0), distance(0) {}
    Plane(const vec3& n, float d) : normal(n), distance(d) {}
};

struct Frustum {
    Plane planes[6]; // left, right, bottom, top, near, far
};

// Extract frustum planes from projection * view matrix
Frustum extract_frustum(const mat4& proj_view);

// Check if a point is inside the frustum
bool point_in_frustum(const Frustum& frustum, const vec3& point);

// Check if a sphere is inside the frustum
bool sphere_in_frustum(const Frustum& frustum, const vec3& center, float radius);

#endif