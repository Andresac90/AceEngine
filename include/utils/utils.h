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

// Global window title (stored so FPS counter can append to it)
extern std::string g_window_title;

// Read shader file
std::string readShaderFile(const std::string& filepath);

// Update function called every frame - handles ESC key to quit AND P key for screenshots
void updateInput(GLFWwindow* window);

// Update input with shader reload (R key) AND screenshot (P key)
void updateInputWithShaderReload(GLFWwindow* window, Shader* shader1, Shader* shader2 = nullptr);

// Update FPS counter in window title (appends to g_window_title)
void update_fps_counter(GLFWwindow* window);

// Set the base window title (called by Engine during init)
void set_window_title(const char* title);

// GLFW callbacks
void glfw_window_size_callback(GLFWwindow* window, int width, int height);
void glfw_framebuffer_resize_callback(GLFWwindow* window, int width, int height);

#endif