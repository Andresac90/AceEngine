#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercise1.h"
#include "log.h"
#include "utils.h"

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

    gl_log("Loading shaders from files\n");
    
    //Load Vertex Shader from file
    std::string vertex_shader_str = readShaderFile("src/shaders/exercise1/test.vert");
    const char* vertex_shader = vertex_shader_str.c_str();

    //Load Fragment Shader from file
    std::string fragment_shader_str = readShaderFile("src/shaders/exercise1/test.frag");
    const char* fragment_shader = fragment_shader_str.c_str();

    // Check if shaders loaded successfully
    if (vertex_shader_str.empty() || fragment_shader_str.empty()) {
        gl_log_err("ERROR: Failed to load shader files\n");
        std::cerr << "ERROR: Failed to load shader files" << std::endl;
        return;
    }

    //Load second Fragment Shader (different color)
    std::string fragment_shader_str2 = readShaderFile("src/shaders/exercise1/test2.frag");
    const char* fragment_shader2 = fragment_shader_str2.c_str();
    
    if (fragment_shader_str2.empty()) {
        gl_log_err("ERROR: Failed to load second fragment shader\n");
        std::cerr << "ERROR: Failed to load second fragment shader" << std::endl;
        return;
    }

    gl_log("Compiling shaders\n");
    
    //Compile Shaders
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &vertex_shader, nullptr);
    glCompileShader(vs);
    
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fragment_shader, nullptr);
    glCompileShader(fs);

    //Create empty shader program
    GLuint shader_programme = glCreateProgram();
    glAttachShader(shader_programme, fs);
    glAttachShader(shader_programme, vs);
    glLinkProgram(shader_programme);
    gl_log("First shader program created\n");

    //Compile second fragment shader
    GLuint fs2 = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs2, 1, &fragment_shader2, nullptr);
    glCompileShader(fs2);

    //Create second shader program (reuse same vertex shader)
    GLuint shader_programme2 = glCreateProgram();
    glAttachShader(shader_programme2, fs2);
    glAttachShader(shader_programme2, vs);
    glLinkProgram(shader_programme2);
    gl_log("Second shader program created\n");

    // Set background color
    glClearColor(0.6f, 0.6f, 0.8f, 1.0f);

    gl_log("Entering render loop\n");
    
    //Draw Loop
    while (!glfwWindowShouldClose(window)) {
        // Update FPS counter
        update_fps_counter(window);
        
        // Update input (handles ESC key)
        updateInput(window);
        
        // Clear and set viewport
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);
        
        // Draw first shape (purple square)
        glUseProgram(shader_programme);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        
        // Draw second shape (orange triangle)
        glUseProgram(shader_programme2);
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
    glDeleteProgram(shader_programme);
    glDeleteProgram(shader_programme2);
    glDeleteShader(vs);
    glDeleteShader(fs);
    glDeleteShader(fs2);

    gl_log("Exercise 1 completed\n");
}