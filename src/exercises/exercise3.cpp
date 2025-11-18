#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include "exercises/exercise3.h"
#include "graphics/shader.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise3(GLFWwindow* window) {
    gl_log("Running Exercise 3 - Matrix Transformations\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return;
    }

    glViewport(0, 0, g_fb_width, g_fb_height);

    const GLubyte* renderer = glGetString(GL_RENDERER);
    const GLubyte* version = glGetString(GL_VERSION);
    gl_log("Renderer: %s\n", renderer);
    gl_log("OpenGL version supported: %s\n", version);
    std::cout << "Renderer: " << renderer << std::endl;
    std::cout << "OpenGL version: " << version << std::endl;

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    gl_log("Depth testing enabled\n");

    // Triangle vertex positions
    GLfloat points[] = {
         0.0f,  0.5f,  0.0f,
         0.5f, -0.5f,  0.0f,
        -0.5f, -0.5f,  0.0f
    };

    // Triangle vertex colors
    GLfloat colours[] = {
        1.0f, 0.0f, 0.0f,  // Red
        0.0f, 1.0f, 0.0f,  // Green
        0.0f, 0.0f, 1.0f   // Blue
    };

    gl_log("Creating VBOs and VAO\n");

    // Create VBOs and VAO
    GLuint points_vbo, colours_vbo, vao;
    
    glGenBuffers(1, &points_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);

    glGenBuffers(1, &colours_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, colours_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(colours), colours, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    glBindBuffer(GL_ARRAY_BUFFER, colours_vbo);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);

    // Load shaders
    gl_log("Loading shaders\n");
    Shader shader;
    if (!shader.loadFromFiles("shaders/exercises/exercise3/vertex.glsl", 
                               "shaders/exercises/exercise3/fragment.glsl")) {
        gl_log_err("Failed to load shader\n");
        std::cerr << "Failed to load shader" << std::endl;
        return;
    }

    shader.printAll();
    shader.validate();

    shader.use();
    int matrix_location = glGetUniformLocation(shader.programme, "matrix");
    
    if (matrix_location == -1) {
        gl_log_err("ERROR: Could not find 'matrix' uniform!\n");
        std::cerr << "ERROR: Could not find 'matrix' uniform!" << std::endl;
    } else {
        gl_log("Matrix uniform location: %i\n", matrix_location);
        std::cout << "Matrix uniform location: " << matrix_location << std::endl;
    }

    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    std::cout << "\n=== Matrix Transformations Demo ===" << std::endl;
    std::cout << "Triangle: Translate + Rotate + Scale" << std::endl;
    std::cout << "Watch it move, spin, and pulse!" << std::endl;
    std::cout << "Press ESC to exit" << std::endl;

    gl_log("Entering render loop\n");

    // Animation variables
    float speed = 1.0f;
    float last_position = 0.0f;
    float rotation_angle = 0.0f;
    float scale_time = 0.0f;

    while (!glfwWindowShouldClose(window)) {
        // Timer for animation
        static double previous_seconds = glfwGetTime();
        double current_seconds = glfwGetTime();
        double elapsed_seconds = current_seconds - previous_seconds;
        previous_seconds = current_seconds;

        // Update translation (bounce back and forth)
        if (fabs(last_position) > 1.0f) {
            speed = -speed;
        }
        last_position += elapsed_seconds * speed;

        // Update rotation
        rotation_angle += elapsed_seconds * 50.0f;  // 50 degrees per second
        if (rotation_angle > 360.0f) {
            rotation_angle -= 360.0f;
        }

        // Update scale (pulse between 0.5 and 1.0)
        scale_time += elapsed_seconds;
        float scale_factor = 0.5f + 0.25f * (1.0f + sinf(scale_time * 2.0f));  // Oscillates 0.5 to 1.0

        // Build transformation matrix: Translate * Rotate * Scale
        mat4 translation = translate(last_position, 0.0f, 0.0f);
        mat4 rotation = rotate_z(rotation_angle);
        mat4 scaling = scale(scale_factor, scale_factor, 1.0f);  // Animated scale!
        
        // Combine transformations (order matters!)
        mat4 model = translation * rotation * scaling;

        update_fps_counter(window);
        
        updateInput(window);  // Handles ESC and P key (screenshot)
        
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);
        
        shader.use();
        glUniformMatrix4fv(matrix_location, 1, GL_FALSE, model.m);
        
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        
        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    gl_log("Exiting render loop, cleaning up\n");

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &points_vbo);
    glDeleteBuffers(1, &colours_vbo);

    gl_log("Exercise 3 completed\n");
}

REGISTER_EXERCISE("3. Matrix Translation", runExercise3)