#include <GLFW/glfw3.h>
#include "log.h"
#include "utils.h"
#include "exercise1.h"

// GLFW error callback
void glfw_error_callback(int error, const char* description) {
    gl_log_err("GLFW ERROR: code %i msg: %s\n", error, description);
}

int main() {
    // Restart the log file
    if (!restart_gl_log()) {
        return 1;
    }
    
    // Log GLFW startup
    gl_log("starting GLFW\n%s\n", glfwGetVersionString());
    
    // Register the error callback function
    glfwSetErrorCallback(glfw_error_callback);
    
    // Initialize GLFW
    if (!glfwInit()) {
        gl_log_err("ERROR: could not start GLFW3\n");
        return 1;
    }
    
    // Set OpenGL version hints (required for macOS)
    gl_log("Setting OpenGL version hints\n");
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    
    // Set anti-aliasing BEFORE creating window
    glfwWindowHint(GLFW_SAMPLES, 4);  // 4x MSAA
    gl_log("Anti-aliasing set to 4x MSAA\n");
    
    // Windowed mode (easier to test resize and callbacks)
    GLFWwindow* window = glfwCreateWindow(640, 480, "Extended GL Init", nullptr, nullptr);
    g_win_width = 640;
    g_win_height = 480;
    
    /* Fullscreen mode - uncomment to use fullscreen
    GLFWmonitor* mon = glfwGetPrimaryMonitor();
    const GLFWvidmode* vmode = glfwGetVideoMode(mon);
    GLFWwindow* window = glfwCreateWindow(vmode->width, vmode->height, "Extended GL Init", mon, nullptr);
    g_win_width = vmode->width;
    g_win_height = vmode->height;
    */
    
    if (!window) {
        gl_log_err("ERROR: could not create window\n");
        glfwTerminate();
        return 1;
    }
    
    gl_log("Window created: %dx%d\n", g_win_width, g_win_height);
    
    // Get framebuffer dimensions
    glfwGetFramebufferSize(window, &g_fb_width, &g_fb_height);
    gl_log("Initial framebuffer size: %dx%d\n", g_fb_width, g_fb_height);
    
    // Register window callbacks
    glfwSetWindowSizeCallback(window, glfw_window_size_callback);
    glfwSetFramebufferSizeCallback(window, glfw_framebuffer_resize_callback);
    
    // Run your exercise with the window
    runExercise1(window);
    
    // Terminate GLFW
    glfwTerminate();
    gl_log("GLFW terminated\n");
    
    return 0;
}