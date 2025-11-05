#ifndef SHADER_H
#define SHADER_H

#include <glad/glad.h>
#include <string>

class Shader {
public:
    GLuint programme;
    
    Shader();
    ~Shader();
    
    // Load and compile shaders from files
    bool loadFromFiles(const std::string& vertex_path, const std::string& fragment_path);
    
    // Reload shaders (for live editing)
    bool reload();
    
    // Use this shader
    void use();
    
    // Check if this shader is currently in use
    bool isInUse() const;
    
    // Validate shader (only use during development)
    bool validate();
    
    // Get uniform location
    GLint getUniformLocation(const std::string& name);
    
    // Set uniform values (with automatic use() check)
    void setUniform(const std::string& name, int value);
    void setUniform(const std::string& name, float value);
    void setUniform(const std::string& name, float x, float y);
    void setUniform(const std::string& name, float x, float y, float z);
    void setUniform(const std::string& name, float x, float y, float z, float w);
    
    // Print all shader info
    void printAll();
    
private:
    GLuint vertex_shader;
    GLuint fragment_shader;
    
    // Store paths for reloading
    std::string vertex_path;
    std::string fragment_path;
    
    // Helper functions
    bool compileShader(GLuint shader_index, const std::string& source);
    bool linkProgram();
    void printShaderInfoLog(GLuint shader_index);
    void printProgramInfoLog(GLuint programme);
    const char* glTypeToString(GLenum type);
};

#endif