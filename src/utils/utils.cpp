#include <glad/glad.h> 
#include "utils/utils.h"
#include "utils/log.h"
#include "graphics/shader.h"
#include <fstream>
#include <sstream>
#include <iostream>
#include <cstdio>

// Global window dimensions
int g_win_width = 640;
int g_win_height = 480;
int g_fb_width = 640;
int g_fb_height = 480;

// FPS counter variables
static double previous_seconds = 0.0;
static int frame_count = 0;

// Function to read shader file
std::string readShaderFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        gl_log_err("ERROR: Could not open shader file: %s\n", filepath.c_str());
        std::cerr << "ERROR: Could not open shader file: " << filepath << std::endl;
        return "";
    }
    
    std::stringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
}

// Update function - handles common input like ESC to quit
void updateInput(GLFWwindow* window) {
    // Check for ESC key press to close the window
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, 1);
    }
}

// Update input with shader reload (R key)
void updateInputWithShaderReload(GLFWwindow* window, Shader* shader1, Shader* shader2) {
    // Check for ESC key press to close the window
    if (glfwGetKey(window, GLFW_KEY_ESCAPE) == GLFW_PRESS) {
        glfwSetWindowShouldClose(window, 1);
    }
    
    // Check for R key press to reload shaders
    static bool r_key_was_pressed = false;
    bool r_key_is_pressed = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
    
    if (r_key_is_pressed && !r_key_was_pressed) {
        std::cout << "\n=== Reloading Shaders ===" << std::endl;
        gl_log("=== Reloading Shaders ===\n");
        
        if (shader1) {
            shader1->reload();
        }
        if (shader2) {
            shader2->reload();
        }
    }
    
    r_key_was_pressed = r_key_is_pressed;
}

// Update FPS counter in window title
void update_fps_counter(GLFWwindow* window) {
    double current_seconds = glfwGetTime();
    double elapsed_seconds = current_seconds - previous_seconds;
    
    // Limit text updates to 4 per second
    if (elapsed_seconds > 0.25) {
        previous_seconds = current_seconds;
        double fps = (double)frame_count / elapsed_seconds;
        double ms_per_frame = 1000.0 / fps;
        
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "OpenGL @ fps: %.2f | ms/frame: %.2f", fps, ms_per_frame);
        glfwSetWindowTitle(window, tmp);
        
        frame_count = 0;
    }
    
    frame_count++;
}

// Window size callback - called when window is resized
void glfw_window_size_callback(GLFWwindow* /*window*/, int width, int height) {
    g_win_width = width;
    g_win_height = height;
    gl_log("Window resized to %dx%d\n", width, height);
    std::cout << "Window resized to " << width << "x" << height << std::endl;
}

// Framebuffer resize callback - called when framebuffer is resized
void glfw_framebuffer_resize_callback(GLFWwindow* /*window*/, int width, int height) {
    g_fb_width = width;
    g_fb_height = height;
    glViewport(0, 0, width, height);
    gl_log("Framebuffer resized to %dx%d\n", width, height);
    std::cout << "Framebuffer resized to " << width << "x" << height << std::endl;
}