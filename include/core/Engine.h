#ifndef ENGINE_H
#define ENGINE_H

#include <GLFW/glfw3.h>

class Engine {
public:
    Engine();
    ~Engine();
    
    // Initialize the engine
    bool init(int width = 640, int height = 480, bool fullscreen = false);
    
    // Shutdown the engine
    void shutdown();
    
    // Get the window
    GLFWwindow* getWindow() const { return window; }
    
private:
    GLFWwindow* window;
    bool initialized;
};

#endif