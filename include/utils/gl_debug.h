#ifndef GL_DEBUG_H
#define GL_DEBUG_H

#include <glad/glad.h>

// OpenGL debug callback function
void GLAPIENTRY gl_debug_callback(
    GLenum source,
    GLenum type,
    GLuint id,
    GLenum severity,
    GLsizei length,
    const GLchar* message,
    const void* userParam
);

// Initialize OpenGL debug output (if available)
void init_gl_debug_output();

// Manual error checking for systems without debug callback (macOS)
// Call this after OpenGL calls to check for errors
void check_gl_error(const char* file, int line, const char* function);

// Macro for easy error checking: CHECK_GL_ERROR();
#define CHECK_GL_ERROR() check_gl_error(__FILE__, __LINE__, __func__)

// Check and clear all pending OpenGL errors (useful in render loops)
bool check_and_clear_gl_errors(const char* context = nullptr);

#endif