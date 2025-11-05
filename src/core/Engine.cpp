#include "core/Engine.h"
#include "utils/log.h"
#include "utils/utils.h"
#include <iostream>

// GLFW error callback
static void glfw_error_callback(int error, const char* description) {
    gl_log_err("GLFW ERROR: code %i msg: %s\n", error, description);
}

Engine::Engine() : window(nullptr), initialized(false) {}

Engine::~Engine() {
    if (initialized) {
        shutdown();
    }
}

bool Engine::init(int width, int height, bool fullscreen) {
    // Restart the log file
    if (!restart_gl_log()) {
        return false;
    }
    
    // Log GLFW startup
    gl_log("Starting AceEngine\n");
    gl_log("GLFW Version: %s\n", glfwGetVersionString());
    
    // Register the error callback function
    glfwSetErrorCallback(glfw_error_callback);
    
    // Initialize GLFW
    if (!glfwInit()) {
        gl_log_err("ERROR: could not start GLFW3\n");
        return false;
    }
    
    // Set OpenGL version hints (required for macOS)
    gl_log("Setting OpenGL version hints\n");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    // Set anti-aliasing
    glfwWindowHint(GLFW_SAMPLES, 4);  // 4x MSAA
    gl_log("Anti-aliasing set to 4x MSAA\n");
    
    // Create window (windowed or fullscreen)
    if (fullscreen) {
        GLFWmonitor* mon = glfwGetPrimaryMonitor();
        const GLFWvidmode* vmode = glfwGetVideoMode(mon);
        window = glfwCreateWindow(vmode->width, vmode->height, "AceEngine", mon, nullptr);
        g_win_width = vmode->width;
        g_win_height = vmode->height;
        gl_log("Created fullscreen window: %dx%d\n", g_win_width, g_win_height);
    } else {
        window = glfwCreateWindow(width, height, "AceEngine", nullptr, nullptr);
        g_win_width = width;
        g_win_height = height;
        gl_log("Created windowed mode: %dx%d\n", width, height);
    }
    
    if (!window) {
        gl_log_err("ERROR: could not create window\n");
        glfwTerminate();
        return false;
    }
    
    // Get framebuffer dimensions
    glfwGetFramebufferSize(window, &g_fb_width, &g_fb_height);
    gl_log("Initial framebuffer size: %dx%d\n", g_fb_width, g_fb_height);
    
    // Register window callbacks
    glfwSetWindowSizeCallback(window, glfw_window_size_callback);
    glfwSetFramebufferSizeCallback(window, glfw_framebuffer_resize_callback);
    
    initialized = true;
    gl_log("Engine initialized successfully\n");
    return true;
}

void Engine::shutdown() {
    if (!initialized) {
        return;
    }
    
    gl_log("Shutting down engine\n");
    glfwTerminate();
    initialized = false;
}