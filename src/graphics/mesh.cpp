#include "graphics/mesh.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <iostream>
#include <cmath>

Mesh::Mesh() : vao(0), points_vbo(0), normals_vbo(0), texcoords_vbo(0), 
               tangents_vbo(0), vertex_count(0), has_tangents(false) {}

Mesh::~Mesh() {
    if (points_vbo) glDeleteBuffers(1, &points_vbo);
    if (normals_vbo) glDeleteBuffers(1, &normals_vbo);
    if (texcoords_vbo) glDeleteBuffers(1, &texcoords_vbo);
    if (tangents_vbo) glDeleteBuffers(1, &tangents_vbo);
    if (vao) glDeleteVertexArrays(1, &vao);
}

bool Mesh::loadFromFile(const char* filename) {
    Assimp::Importer importer;
    
    // IMPORTANT: Add aiProcess_CalcTangentSpace for normal mapping!
    const aiScene* scene = importer.ReadFile(filename, 
        aiProcess_Triangulate | 
        aiProcess_GenNormals |
        aiProcess_CalcTangentSpace);  // ← Generate tangents!
    
    if (!scene || !scene->HasMeshes()) {
        std::cerr << "ERROR: Could not load mesh: " << filename << std::endl;
        return false;
    }
    
    const aiMesh* mesh = scene->mMeshes[0];
    vertex_count = mesh->mNumVertices;
    
    std::vector<GLfloat> points;
    std::vector<GLfloat> normals;
    std::vector<GLfloat> texcoords;
    std::vector<GLfloat> tangents;  // NEW: 4 components (xyz + determinant)
    
    points.reserve(vertex_count * 3);
    normals.reserve(vertex_count * 3);
    if (mesh->HasTextureCoords(0)) {
        texcoords.reserve(vertex_count * 2);
    }
    
    // Check if tangents exist
    has_tangents = mesh->HasTangentsAndBitangents();
    if (has_tangents) {
        tangents.reserve(vertex_count * 4);
        std::cout << "Mesh has tangents and bitangents" << std::endl;
    }
    
    for (unsigned int i = 0; i < mesh->mNumVertices; i++) {
        // Positions
        const aiVector3D& v = mesh->mVertices[i];
        points.push_back(v.x);
        points.push_back(v.y);
        points.push_back(v.z);
        
        // Normals
        if (mesh->HasNormals()) {
            const aiVector3D& n = mesh->mNormals[i];
            normals.push_back(n.x);
            normals.push_back(n.y);
            normals.push_back(n.z);
        }
        
        // Texture coordinates
        if (mesh->HasTextureCoords(0)) {
            const aiVector3D& t = mesh->mTextureCoords[0][i];
            texcoords.push_back(t.x);
            texcoords.push_back(t.y);
        }
        
        // Tangents (with Gram-Schmidt orthogonalization)
        if (has_tangents) {
            const aiVector3D& tangent = mesh->mTangents[i];
            const aiVector3D& bitangent = mesh->mBitangents[i];
            const aiVector3D& normal = mesh->mNormals[i];
            
            // Convert to our format
            float t_x = tangent.x, t_y = tangent.y, t_z = tangent.z;
            float n_x = normal.x, n_y = normal.y, n_z = normal.z;
            float b_x = bitangent.x, b_y = bitangent.y, b_z = bitangent.z;
            
            // Gram-Schmidt orthogonalize tangent
            // t_i = normalize(t - n * dot(n, t))
            float dot_nt = n_x * t_x + n_y * t_y + n_z * t_z;
            float ti_x = t_x - n_x * dot_nt;
            float ti_y = t_y - n_y * dot_nt;
            float ti_z = t_z - n_z * dot_nt;
            
            // Normalize
            float len = std::sqrt(ti_x*ti_x + ti_y*ti_y + ti_z*ti_z);
            if (len > 0.0f) {
                ti_x /= len;
                ti_y /= len;
                ti_z /= len;
            }
            
            // Calculate determinant (handedness)
            // det = dot(cross(n, t), b)
            float cross_x = n_y * ti_z - n_z * ti_y;
            float cross_y = n_z * ti_x - n_x * ti_z;
            float cross_z = n_x * ti_y - n_y * ti_x;
            float det = cross_x * b_x + cross_y * b_y + cross_z * b_z;
            float handedness = (det < 0.0f) ? -1.0f : 1.0f;
            
            tangents.push_back(ti_x);
            tangents.push_back(ti_y);
            tangents.push_back(ti_z);
            tangents.push_back(handedness);  // Store in w component
        }
    }
    
    // Create VAO
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    // Points VBO (location 0)
    glGenBuffers(1, &points_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glBufferData(GL_ARRAY_BUFFER, points.size() * sizeof(GLfloat), points.data(), GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    // Normals VBO (location 1)
    if (!normals.empty()) {
        glGenBuffers(1, &normals_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, normals_vbo);
        glBufferData(GL_ARRAY_BUFFER, normals.size() * sizeof(GLfloat), normals.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(1);
    }
    
    // Texcoords VBO (location 2)
    if (!texcoords.empty()) {
        glGenBuffers(1, &texcoords_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, texcoords_vbo);
        glBufferData(GL_ARRAY_BUFFER, texcoords.size() * sizeof(GLfloat), texcoords.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(2);
    }
    
    // Tangents VBO (location 3) - 4 components!
    if (!tangents.empty()) {
        glGenBuffers(1, &tangents_vbo);
        glBindBuffer(GL_ARRAY_BUFFER, tangents_vbo);
        glBufferData(GL_ARRAY_BUFFER, tangents.size() * sizeof(GLfloat), tangents.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, 0, nullptr);  // 4 components!
        glEnableVertexAttribArray(3);
    }
    
    glBindVertexArray(0);
    
    return true;
}

void Mesh::draw() const {
    glBindVertexArray(vao);
    glDrawArrays(GL_TRIANGLES, 0, vertex_count);
    glBindVertexArray(0);
}