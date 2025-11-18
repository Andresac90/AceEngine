#include "utils/gl_debug.h"
#include "utils/log.h"
#include <iostream>
#include <cstring>

// Track if debug callback is available
static bool debug_callback_available = false;

void GLAPIENTRY gl_debug_callback(
    GLenum source,
    GLenum type,
    GLuint id,
    GLenum severity,
    GLsizei length,
    const GLchar* message,
    const void* userParam
) {
    // This function won't be called on macOS, but we keep it for other platforms
    (void)source; (void)type; (void)id; (void)severity;
    (void)length; (void)message; (void)userParam;
}

void init_gl_debug_output() {
    // On macOS, OpenGL debug callback is not available (requires OpenGL 4.3+)
    // macOS only supports up to OpenGL 4.1
    std::cout << "OpenGL debug output: NOT AVAILABLE on macOS" << std::endl;
    std::cout << "  Using manual glGetError() checking instead" << std::endl;
    gl_log("OpenGL debug callback not available on macOS - using manual error checking\n");
    debug_callback_available = false;
}

// Get error string from error code
static const char* gl_error_string(GLenum error) {
    switch (error) {
        case GL_NO_ERROR:          return "GL_NO_ERROR";
        case GL_INVALID_ENUM:      return "GL_INVALID_ENUM";
        case GL_INVALID_VALUE:     return "GL_INVALID_VALUE";
        case GL_INVALID_OPERATION: return "GL_INVALID_OPERATION";
        case GL_OUT_OF_MEMORY:     return "GL_OUT_OF_MEMORY";
        case GL_INVALID_FRAMEBUFFER_OPERATION: return "GL_INVALID_FRAMEBUFFER_OPERATION";
        default:                   return "UNKNOWN_ERROR";
    }
}

// Manual error checking
void check_gl_error(const char* file, int line, const char* function) {
    GLenum error;
    bool has_error = false;
    
    while ((error = glGetError()) != GL_NO_ERROR) {
        has_error = true;
        std::cerr << "\n=== OpenGL Error (Manual Check) ===" << std::endl;
        std::cerr << "Error: " << gl_error_string(error) << " (0x" << std::hex << error << std::dec << ")" << std::endl;
        std::cerr << "File: " << file << std::endl;
        std::cerr << "Line: " << line << std::endl;
        std::cerr << "Function: " << function << std::endl;
        std::cerr << "===================================\n" << std::endl;
        
        gl_log_err("OpenGL Error: %s at %s:%d in %s\n", 
                   gl_error_string(error), file, line, function);
    }
    
    (void)has_error;
}

// Check and clear all errors (useful in render loops)
bool check_and_clear_gl_errors(const char* context) {
    GLenum error;
    bool has_error = false;
    
    while ((error = glGetError()) != GL_NO_ERROR) {
        has_error = true;
        if (context) {
            std::cerr << "OpenGL Error in " << context << ": " 
                      << gl_error_string(error) << std::endl;
            gl_log_err("OpenGL Error in %s: %s\n", context, gl_error_string(error));
        } else {
            std::cerr << "OpenGL Error: " << gl_error_string(error) << std::endl;
            gl_log_err("OpenGL Error: %s\n", gl_error_string(error));
        }
    }
    
    return has_error;
}