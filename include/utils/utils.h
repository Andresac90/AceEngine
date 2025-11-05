#ifndef UTILS_H
#define UTILS_H

#include <GLFW/glfw3.h>
#include <string>

// Forward declaration
class Shader;

// Global window dimensions
extern int g_win_width;
extern int g_win_height;
extern int g_fb_width;
extern int g_fb_height;

// Read shader file
std::string readShaderFile(const std::string& filepath);

// Update function called every frame - handles ESC key to quit
void updateInput(GLFWwindow* window);

// Update input with shader reload (R key)
void updateInputWithShaderReload(GLFWwindow* window, Shader* shader1, Shader* shader2 = nullptr);

// Update FPS counter in window title
void update_fps_counter(GLFWwindow* window);

// GLFW callbacks
void glfw_window_size_callback(GLFWwindow* window, int width, int height);
void glfw_framebuffer_resize_callback(GLFWwindow* window, int width, int height);

#endif