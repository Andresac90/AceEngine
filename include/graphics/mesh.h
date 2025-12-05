#ifndef MESH_H
#define MESH_H

#include <glad/glad.h>
#include <string>
#include <vector>

class Mesh {
public:
    Mesh();
    ~Mesh();
    
    bool loadFromFile(const char* filename);
    void draw() const;
    
    int getVertexCount() const { return vertex_count; }
    int getTriangleCount() const { return vertex_count / 3; }
    bool hasTangents() const { return has_tangents; }
    
private:
    GLuint vao;
    GLuint points_vbo;
    GLuint normals_vbo;
    GLuint texcoords_vbo;
    GLuint tangents_vbo;  
    
    int vertex_count;
    bool has_tangents;    
};

#endif