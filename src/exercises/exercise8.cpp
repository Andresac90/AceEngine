#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise8.h"
#include "graphics/shader.h"
#include "graphics/texture.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise8(GLFWwindow* window) {
    gl_log("Running Exercise 8 - Texture Blending\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        return;
    }

    glViewport(0, 0, g_fb_width, g_fb_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);
    glCullFace(GL_BACK);

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Quad with texture coordinates
    GLfloat points[] = {
        -1.0f, -1.0f,  0.0f,
         1.0f, -1.0f,  0.0f,
         1.0f,  1.0f,  0.0f,
         1.0f,  1.0f,  0.0f,
        -1.0f,  1.0f,  0.0f,
        -1.0f, -1.0f,  0.0f
    };

    GLfloat texcoords[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
        0.0f, 0.0f
    };

    GLuint points_vbo, texcoords_vbo, vao;
    
    glGenBuffers(1, &points_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);

    glGenBuffers(1, &texcoords_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, texcoords_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(texcoords), texcoords, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    // Position attribute (location 0)
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    // Texture coordinate attribute (location 2)
    glBindBuffer(GL_ARRAY_BUFFER, texcoords_vbo);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(2);

    // Load TWO textures
    Texture first_tex;
    if (!first_tex.loadFromFile("assets/textures/geyser-rock1_albedo.png")) {
        std::cerr << "Failed to load first texture!" << std::endl;
        return;
    }

    Texture second_tex;
    if (!second_tex.loadFromFile("assets/textures/lava-and-rock_albedo.png")) {
        std::cerr << "Failed to load second texture!" << std::endl;
        return;
    }

    std::cout << "Both textures loaded successfully!" << std::endl;

    // Load shaders
    Shader shader;
    if (!shader.loadFromFiles("shaders/exercises/exercise8/vertex.glsl", 
                               "shaders/exercises/exercise8/fragment.glsl")) {
        std::cerr << "Failed to load shader" << std::endl;
        return;
    }

    shader.use();
    
    // Get uniform locations
    int model_loc = glGetUniformLocation(shader.programme, "model");
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    int first_sampler_loc = glGetUniformLocation(shader.programme, "first_texture");
    int second_sampler_loc = glGetUniformLocation(shader.programme, "second_texture");
    
    std::cout << "\nUniform locations:" << std::endl;
    std::cout << "  model: " << model_loc << std::endl;
    std::cout << "  view: " << view_loc << std::endl;
    std::cout << "  proj: " << proj_loc << std::endl;
    std::cout << "  first_texture: " << first_sampler_loc << std::endl;
    std::cout << "  second_texture: " << second_sampler_loc << std::endl;

    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);

    // View matrix
    mat4 view_mat = translate(vec3(0.0f, 0.0f, -3.0f));
    
    // Projection matrix
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);

    // Send matrices once
    glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);
    glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);

    std::cout << "\n=== Exercise 8 - Texture Blending ===" << std::endl;
    std::cout << "Blending two textures horizontally!" << std::endl;
    std::cout << "Left side = First texture (100%)" << std::endl;
    std::cout << "Right side = Second texture (100%)" << std::endl;
    std::cout << "Middle = Smooth blend" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  SPACE - Toggle rotation" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    float rotation_angle = 0.0f;
    bool rotate = false;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Toggle rotation
        static bool space_was_pressed = false;
        bool space_is_pressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space_is_pressed && !space_was_pressed) {
            rotate = !rotate;
            std::cout << "Rotation: " << (rotate ? "ON" : "OFF") << std::endl;
        }
        space_was_pressed = space_is_pressed;

        updateInput(window);

        // Update rotation
        if (rotate) {
            rotation_angle += 30.0f * elapsed;
            if (rotation_angle > 360.0f) rotation_angle -= 360.0f;
        }

        // Model matrix
        mat4 R = rotate_y(rotation_angle);
        mat4 model_mat = R;
        
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        shader.use();
        
        // ========================================
        // CRITICAL FIX FOR MACOS METAL:
        // Bind textures AND set sampler uniforms EVERY FRAME
        // ========================================
        glActiveTexture(GL_TEXTURE0);
        first_tex.bind();
        glUniform1i(first_sampler_loc, 0);  // ← ADD THIS LINE!
        
        glActiveTexture(GL_TEXTURE1);
        second_tex.bind();
        glUniform1i(second_sampler_loc, 1); // ← ADD THIS LINE!
        
        glUniformMatrix4fv(model_loc, 1, GL_FALSE, model_mat.m);

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &points_vbo);
    glDeleteBuffers(1, &texcoords_vbo);

    gl_log("Exercise 8 completed\n");
}

REGISTER_EXERCISE(8, "Texture Blending", runExercise8)