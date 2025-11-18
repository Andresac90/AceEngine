#ifndef ENGINE_H
#define ENGINE_H

#include <glad/glad.h>
#include <GLFW/glfw3.h>

class Engine {
public:
    Engine();
    ~Engine();
    
    bool init(int width, int height, bool fullscreen, const char* title = "AceEngine");
    void shutdown();
    
    GLFWwindow* getWindow() const { return window; }
    bool isInitialized() const { return initialized; }
    
private:
    GLFWwindow* window;
    bool initialized;
};

#endif