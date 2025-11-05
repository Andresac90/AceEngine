#include "graphics/shader.h"
#include "utils/log.h"
#include "utils/utils.h"
#include <iostream>

Shader::Shader() : programme(0), vertex_shader(0), fragment_shader(0) {}

Shader::~Shader() {
    if (vertex_shader) glDeleteShader(vertex_shader);
    if (fragment_shader) glDeleteShader(fragment_shader);
    if (programme) glDeleteProgram(programme);
}

bool Shader::loadFromFiles(const std::string& vertex_path, const std::string& fragment_path) {
    // Store paths for reload functionality
    this->vertex_path = vertex_path;
    this->fragment_path = fragment_path;
    
    gl_log("Loading shaders: %s, %s\n", vertex_path.c_str(), fragment_path.c_str());
    
    // Read shader source files
    std::string vertex_source = readShaderFile(vertex_path);
    std::string fragment_source = readShaderFile(fragment_path);
    
    if (vertex_source.empty() || fragment_source.empty()) {
        gl_log_err("Failed to load shader files\n");
        return false;
    }
    
    // Create shader objects
    vertex_shader = glCreateShader(GL_VERTEX_SHADER);
    fragment_shader = glCreateShader(GL_FRAGMENT_SHADER);
    
    // Compile shaders
    if (!compileShader(vertex_shader, vertex_source)) {
        gl_log_err("Vertex shader compilation failed\n");
        return false;
    }
    
    if (!compileShader(fragment_shader, fragment_source)) {
        gl_log_err("Fragment shader compilation failed\n");
        return false;
    }
    
    // Create and link program
    programme = glCreateProgram();
    glAttachShader(programme, vertex_shader);
    glAttachShader(programme, fragment_shader);
    
    if (!linkProgram()) {
        gl_log_err("Shader program linking failed\n");
        return false;
    }
    
    gl_log("Shader programme %i loaded successfully\n", programme);
    return true;
}

bool Shader::reload() {
    gl_log("Reloading shaders: %s, %s\n", vertex_path.c_str(), fragment_path.c_str());
    
    // Save old programme in case reload fails
    GLuint old_programme = programme;
    GLuint old_vs = vertex_shader;
    GLuint old_fs = fragment_shader;
    
    // Reset IDs
    programme = 0;
    vertex_shader = 0;
    fragment_shader = 0;
    
    // Try to reload
    if (!loadFromFiles(vertex_path, fragment_path)) {
        gl_log_err("Shader reload failed, keeping old shaders\n");
        // Restore old shaders
        programme = old_programme;
        vertex_shader = old_vs;
        fragment_shader = old_fs;
        return false;
    }
    
    // Delete old shaders
    glDeleteShader(old_vs);
    glDeleteShader(old_fs);
    glDeleteProgram(old_programme);
    
    gl_log("Shaders reloaded successfully!\n");
    std::cout << "✓ Shaders reloaded successfully!" << std::endl;
    return true;
}

bool Shader::compileShader(GLuint shader_index, const std::string& source) {
    const char* src = source.c_str();
    glShaderSource(shader_index, 1, &src, nullptr);
    glCompileShader(shader_index);
    
    // Check for compile errors
    int params = -1;
    glGetShaderiv(shader_index, GL_COMPILE_STATUS, &params);
    if (GL_TRUE != params) {
        gl_log_err("ERROR: GL shader index %i did not compile\n", shader_index);
        printShaderInfoLog(shader_index);
        return false;
    }
    
    gl_log("Shader %i compiled successfully\n", shader_index);
    return true;
}

bool Shader::linkProgram() {
    glLinkProgram(programme);
    
    // Check if link was successful
    int params = -1;
    glGetProgramiv(programme, GL_LINK_STATUS, &params);
    if (GL_TRUE != params) {
        gl_log_err("ERROR: could not link shader programme GL index %u\n", programme);
        printProgramInfoLog(programme);
        return false;
    }
    
    gl_log("Shader programme %i linked successfully\n", programme);
    return true;
}

void Shader::use() {
    glUseProgram(programme);
}

bool Shader::isInUse() const {
    GLint current_programme = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &current_programme);
    return (GLuint)current_programme == programme;
}

bool Shader::validate() {
    glValidateProgram(programme);
    int params = -1;
    glGetProgramiv(programme, GL_VALIDATE_STATUS, &params);
    gl_log("program %i GL_VALIDATE_STATUS = %i\n", programme, params);
    if (GL_TRUE != params) {
        printProgramInfoLog(programme);
        return false;
    }
    return true;
}

GLint Shader::getUniformLocation(const std::string& name) {
    GLint location = glGetUniformLocation(programme, name.c_str());
    if (location == -1) {
        gl_log_err("Warning: uniform '%s' not found or not active\n", name.c_str());
    }
    return location;
}

// Uniform setters with automatic shader activation check
void Shader::setUniform(const std::string& name, int value) {
    if (!isInUse()) {
        gl_log_err("ERROR: Trying to set uniform '%s' but shader %i is not in use!\n", name.c_str(), programme);
        return;
    }
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniform1i(location, value);
    }
}

void Shader::setUniform(const std::string& name, float value) {
    if (!isInUse()) {
        gl_log_err("ERROR: Trying to set uniform '%s' but shader %i is not in use!\n", name.c_str(), programme);
        return;
    }
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniform1f(location, value);
    }
}

void Shader::setUniform(const std::string& name, float x, float y) {
    if (!isInUse()) {
        gl_log_err("ERROR: Trying to set uniform '%s' but shader %i is not in use!\n", name.c_str(), programme);
        return;
    }
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniform2f(location, x, y);
    }
}

void Shader::setUniform(const std::string& name, float x, float y, float z) {
    if (!isInUse()) {
        gl_log_err("ERROR: Trying to set uniform '%s' but shader %i is not in use!\n", name.c_str(), programme);
        return;
    }
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniform3f(location, x, y, z);
    }
}

void Shader::setUniform(const std::string& name, float x, float y, float z, float w) {
    if (!isInUse()) {
        gl_log_err("ERROR: Trying to set uniform '%s' but shader %i is not in use!\n", name.c_str(), programme);
        return;
    }
    GLint location = getUniformLocation(name);
    if (location != -1) {
        glUniform4f(location, x, y, z, w);
    }
}

void Shader::printShaderInfoLog(GLuint shader_index) {
    int max_length = 2048;
    int actual_length = 0;
    char log[2048];
    glGetShaderInfoLog(shader_index, max_length, &actual_length, log);
    gl_log("shader info log for GL index %u:\n%s\n", shader_index, log);
    std::cerr << "shader info log for GL index " << shader_index << ":\n" << log << std::endl;
}

void Shader::printProgramInfoLog(GLuint programme) {
    int max_length = 2048;
    int actual_length = 0;
    char log[2048];
    glGetProgramInfoLog(programme, max_length, &actual_length, log);
    gl_log("program info log for GL index %u:\n%s\n", programme, log);
    std::cerr << "program info log for GL index " << programme << ":\n" << log << std::endl;
}

void Shader::printAll() {
    gl_log("--------------------\nshader programme %i info:\n", programme);
    std::cout << "--------------------\nshader programme " << programme << " info:" << std::endl;
    
    int params = -1;
    
    // Link status
    glGetProgramiv(programme, GL_LINK_STATUS, &params);
    gl_log("GL_LINK_STATUS = %i\n", params);
    std::cout << "GL_LINK_STATUS = " << params << std::endl;
    
    // Attached shaders
    glGetProgramiv(programme, GL_ATTACHED_SHADERS, &params);
    gl_log("GL_ATTACHED_SHADERS = %i\n", params);
    std::cout << "GL_ATTACHED_SHADERS = " << params << std::endl;
    
    // Active attributes
    glGetProgramiv(programme, GL_ACTIVE_ATTRIBUTES, &params);
    gl_log("GL_ACTIVE_ATTRIBUTES = %i\n", params);
    std::cout << "GL_ACTIVE_ATTRIBUTES = " << params << std::endl;
    
    for (int i = 0; i < params; i++) {
        char name[64];
        int max_length = 64;
        int actual_length = 0;
        int size = 0;
        GLenum type;
        glGetActiveAttrib(programme, i, max_length, &actual_length, &size, &type, name);
        if (size > 1) {
            for (int j = 0; j < size; j++) {
                char long_name[77];
                snprintf(long_name, 77, "%s[%i]", name, j);
                int location = glGetAttribLocation(programme, long_name);
                gl_log("  %i) type:%s name:%s location:%i\n", i, glTypeToString(type), long_name, location);
                std::cout << "  " << i << ") type:" << glTypeToString(type) << " name:" << long_name << " location:" << location << std::endl;
            }
        } else {
            int location = glGetAttribLocation(programme, name);
            gl_log("  %i) type:%s name:%s location:%i\n", i, glTypeToString(type), name, location);
            std::cout << "  " << i << ") type:" << glTypeToString(type) << " name:" << name << " location:" << location << std::endl;
        }
    }
    
    // Active uniforms
    glGetProgramiv(programme, GL_ACTIVE_UNIFORMS, &params);
    gl_log("GL_ACTIVE_UNIFORMS = %i\n", params);
    std::cout << "GL_ACTIVE_UNIFORMS = " << params << std::endl;
    
    for (int i = 0; i < params; i++) {
        char name[64];
        int max_length = 64;
        int actual_length = 0;
        int size = 0;
        GLenum type;
        glGetActiveUniform(programme, i, max_length, &actual_length, &size, &type, name);
        if (size > 1) {
            for (int j = 0; j < size; j++) {
                char long_name[77];
                snprintf(long_name, 77, "%s[%i]", name, j);
                int location = glGetUniformLocation(programme, long_name);
                gl_log("  %i) type:%s name:%s location:%i\n", i, glTypeToString(type), long_name, location);
                std::cout << "  " << i << ") type:" << glTypeToString(type) << " name:" << long_name << " location:" << location << std::endl;
            }
        } else {
            int location = glGetUniformLocation(programme, name);
            gl_log("  %i) type:%s name:%s location:%i\n", i, glTypeToString(type), name, location);
            std::cout << "  " << i << ") type:" << glTypeToString(type) << " name:" << name << " location:" << location << std::endl;
        }
    }
    
    printProgramInfoLog(programme);
}

const char* Shader::glTypeToString(GLenum type) {
    switch (type) {
        case GL_BOOL: return "bool";
        case GL_INT: return "int";
        case GL_FLOAT: return "float";
        case GL_FLOAT_VEC2: return "vec2";
        case GL_FLOAT_VEC3: return "vec3";
        case GL_FLOAT_VEC4: return "vec4";
        case GL_FLOAT_MAT2: return "mat2";
        case GL_FLOAT_MAT3: return "mat3";
        case GL_FLOAT_MAT4: return "mat4";
        case GL_SAMPLER_2D: return "sampler2D";
        case GL_SAMPLER_3D: return "sampler3D";
        case GL_SAMPLER_CUBE: return "samplerCube";
        case GL_SAMPLER_2D_SHADOW: return "sampler2DShadow";
        default: return "other";
    }
}