#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise1.h"
#include "graphics/shader.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise1(GLFWwindow* window) {
    gl_log("Running Exercise 1\n");
    
    // Make context current
    glfwMakeContextCurrent(window);

    // Initialize GLAD
    gl_log("Initializing GLAD\n");
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return;
    }

    // Set initial viewport based on framebuffer size
    glViewport(0, 0, g_fb_width, g_fb_height);
    gl_log("Initial viewport set to %dx%d\n", g_fb_width, g_fb_height);

    // get version info
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    gl_log("Renderer: %s\n", renderer);
    gl_log("OpenGL version supported: %s\n", version);
    std::cout << "Renderer: " << renderer << std::endl;
    std::cout << "OpenGL version supported: " << version << std::endl;

    // Log OpenGL context parameters
    log_gl_params();

    // Enable anti-aliasing
    glEnable(GL_MULTISAMPLE);
    gl_log("Multisampling enabled\n");

    // tell GL to only draw onto a pixel if the shape is closer to the viewer
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    gl_log("Depth testing enabled\n");

    //Define float array of points - 2 triangles = 6 vertices
    GLfloat points[] = {
        // First triangle
        -0.5f,  0.5f,  0.0f,  // Top-left
         0.5f,  0.5f,  0.0f,  // Top-right
        -0.5f, -0.5f,  0.0f,  // Bottom-left
        
        // Second triangle
         0.5f,  0.5f,  0.0f,  // Top-right
         0.5f, -0.5f,  0.0f,  // Bottom-right
        -0.5f, -0.5f,  0.0f   // Bottom-left
    };

    // Second shape - a triangle on the right side
    GLfloat points2[] = {
        0.2f,  0.5f,  0.0f,
        0.8f,  0.5f,  0.0f,
        0.5f, -0.5f,  0.0f
    };

    gl_log("Creating VBOs and VAOs\n");
    
    //Build VBO
    GLuint vbo = 0;
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);

    //Build VAO
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    //Build second VBO
    GLuint vbo2 = 0;
    glGenBuffers(1, &vbo2);
    glBindBuffer(GL_ARRAY_BUFFER, vbo2);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points2), points2, GL_STATIC_DRAW);

    //Build second VAO
    GLuint vao2 = 0;
    glGenVertexArrays(1, &vao2);
    glBindVertexArray(vao2);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, vbo2);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Load shaders using Shader class
    Shader shader1;
    if (!shader1.loadFromFiles("shaders/exercises/exercise1/test.vert", 
                                "shaders/exercises/exercise1/test.frag")) {
        gl_log_err("Failed to load first shader\n");
        return;
    }
    
    // Print all shader info (useful for debugging)
    shader1.printAll();
    
    // Validate shader (only during development)
    if (!shader1.validate()) {
        gl_log_err("First shader validation failed\n");
    }

    Shader shader2;
    if (!shader2.loadFromFiles("shaders/exercises/exercise1/test.vert", 
                                "shaders/exercises/exercise1/test2.frag")) {
        gl_log_err("Failed to load second shader\n");
        return;
    }
    
    shader2.printAll();
    
    if (!shader2.validate()) {
        gl_log_err("Second shader validation failed\n");
    }

    // Set background color
    glClearColor(0.6f, 0.6f, 0.8f, 1.0f);

    gl_log("Entering render loop\n");
    
    //Draw Loop
    while (!glfwWindowShouldClose(window)) {
        // Update FPS counter
        update_fps_counter(window);
        
        // Update input with shader reload (R key) - pass both shaders
        updateInputWithShaderReload(window, &shader1, &shader2);
        
        // Clear and set viewport
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);
        
        // Draw first shape (purple square)
        shader1.use();
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        
        // Draw second shape (orange triangle)
        shader2.use();
        glBindVertexArray(vao2);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    gl_log("Exiting render loop, cleaning up\n");

    // Cleanup
    glDeleteVertexArrays(1, &vao);
    glDeleteVertexArrays(1, &vao2);
    glDeleteBuffers(1, &vbo);
    glDeleteBuffers(1, &vbo2);
    // Shaders are automatically cleaned up by Shader destructor

    gl_log("Exercise 1 completed\n");
}

REGISTER_EXERCISE("1. Hello Triangle", runExercise1)