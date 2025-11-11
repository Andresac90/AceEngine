#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise2.h"
#include "graphics/shader.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise2(GLFWwindow* window) {
    gl_log("Running Exercise 2 - Vertex Colors\n");
    
    // Make context current
    glfwMakeContextCurrent(window);

    // Initialize GLAD
    gl_log("Initializing GLAD\n");
    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return;
    }

    // Set initial viewport
    glViewport(0, 0, g_fb_width, g_fb_height);
    gl_log("Initial viewport set to %dx%d\n", g_fb_width, g_fb_height);

    // Get version info
    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    gl_log("Renderer: %s\n", renderer);
    gl_log("OpenGL version supported: %s\n", version);
    std::cout << "Renderer: " << renderer << std::endl;
    std::cout << "OpenGL version supported: " << version << std::endl;

    // Enable settings
    glEnable(GL_MULTISAMPLE);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    gl_log("Multisampling and depth testing enabled\n");

    // Enable back-face culling
    glEnable(GL_CULL_FACE);      // Enable culling
    glCullFace(GL_BACK);         // Cull back faces
    glFrontFace(GL_CW);         // GL_CCW for counter clock-wise (Default), GL_CW for clock-wise

    // Triangle vertex positions
    GLfloat points[] = {
         0.0f,  0.5f,  0.0f,
         0.5f, -0.5f,  0.0f,
        -0.5f, -0.5f,  0.0f
    };

    // Triangle vertex colors (RGB)
    GLfloat colours[] = {
        1.0f, 0.0f, 0.0f,  // Red
        0.0f, 1.0f, 0.0f,  // Green
        0.0f, 0.0f, 1.0f   // Blue
    };

    gl_log("Creating VBOs and VAO\n");
    
    // Create VBO for points
    GLuint points_vbo = 0;
    glGenBuffers(1, &points_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);

    // Create VBO for colors
    GLuint colours_vbo = 0;
    glGenBuffers(1, &colours_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, colours_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(colours), colours, GL_STATIC_DRAW);

    // Create VAO and configure vertex attributes
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    // Bind points VBO and set attribute pointer for position (location 0)
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    // Bind colors VBO and set attribute pointer for color (location 1)
    glBindBuffer(GL_ARRAY_BUFFER, colours_vbo);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);

    // Load shaders
    Shader shader;
    if (!shader.loadFromFiles("shaders/exercises/exercise2/vertex.glsl", 
                               "shaders/exercises/exercise2/fragment.glsl")) {
        gl_log_err("Failed to load shader\n");
        return;
    }
    
    shader.printAll();
    shader.validate();

    // Set background color
    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);

    gl_log("Entering render loop\n");
    
    // Draw Loop
    while (!glfwWindowShouldClose(window)) {
        // Update FPS counter
        update_fps_counter(window);
        
        // Update input with shader reload
        updateInputWithShaderReload(window, &shader, nullptr);
        
        // Clear and set viewport
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);
        
        // Draw triangle
        shader.use();
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    gl_log("Exiting render loop, cleaning up\n");

    // Cleanup
    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &points_vbo);
    glDeleteBuffers(1, &colours_vbo);

    gl_log("Exercise 2 completed\n");
}

REGISTER_EXERCISE("2. Vertex Colors", runExercise2)