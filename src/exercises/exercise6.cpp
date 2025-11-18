#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise6.h"
#include "graphics/shader.h"
#include "graphics/texture.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise6(GLFWwindow* window) {
    gl_log("Running Exercise 6 - Texture Mapping\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        return;
    }

    glViewport(0, 0, g_fb_width, g_fb_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Triangle with texture coordinates
    GLfloat points[] = {
         0.0f,  0.5f,  0.0f,
         0.5f, -0.5f,  0.0f,
        -0.5f, -0.5f,  0.0f
    };

    GLfloat texcoords[] = {
        0.5f, 1.0f,
        1.0f, 0.0f,
        0.0f, 0.0f
    };

    std::cout << "\nVertex data:" << std::endl;
    std::cout << "  Point 0: (" << points[0] << ", " << points[1] << ", " << points[2] 
              << ") -> TexCoord: (" << texcoords[0] << ", " << texcoords[1] << ")" << std::endl;
    std::cout << "  Point 1: (" << points[3] << ", " << points[4] << ", " << points[5] 
              << ") -> TexCoord: (" << texcoords[2] << ", " << texcoords[3] << ")" << std::endl;
    std::cout << "  Point 2: (" << points[6] << ", " << points[7] << ", " << points[8] 
              << ") -> TexCoord: (" << texcoords[4] << ", " << texcoords[5] << ")" << std::endl;

    GLuint points_vbo, texcoords_vbo, vao;
    
    glGenBuffers(1, &points_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);

    glGenBuffers(1, &texcoords_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, texcoords_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(texcoords), texcoords, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    glBindBuffer(GL_ARRAY_BUFFER, texcoords_vbo);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);

    std::cout << "\nVAO setup:" << std::endl;
    std::cout << "  Attribute 0 (position): 3 floats per vertex" << std::endl;
    std::cout << "  Attribute 1 (texcoord): 2 floats per vertex" << std::endl;

    Texture texture;
    if (!texture.loadFromFile("assets/textures/test_texture.png")) {
        std::cerr << "\nERROR: Failed to load texture!" << std::endl;
        std::cerr << "Make sure 'assets/textures/test_texture.png' exists!" << std::endl;
        return;
    }

    Shader shader;
    if (!shader.loadFromFiles("shaders/exercises/exercise6/vertex.glsl", 
                               "shaders/exercises/exercise6/fragment.glsl")) {
        std::cerr << "Failed to load shader" << std::endl;
        return;
    }

    shader.use();
    
    int model_loc = glGetUniformLocation(shader.programme, "model");
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    int tex_loc = glGetUniformLocation(shader.programme, "basic_texture");
    
    std::cout << "\nUniform locations:" << std::endl;
    std::cout << "  model: " << model_loc << std::endl;
    std::cout << "  view: " << view_loc << std::endl;
    std::cout << "  proj: " << proj_loc << std::endl;
    std::cout << "  basic_texture: " << tex_loc << std::endl;

    if (model_loc == -1 || view_loc == -1 || proj_loc == -1 || tex_loc == -1) {
        std::cerr << "ERROR: One or more uniforms not found in shader!" << std::endl;
        return;
    }

    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);

    mat4 view_mat = identity_mat4();
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);

    glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);
    glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);
    glUniform1i(tex_loc, 0);

    std::cout << "\n=== Exercise 6 - Texture Mapping ===" << std::endl;
    std::cout << "Triangle with texture applied!" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  SPACE - Toggle rotation" << std::endl;
    std::cout << "  P - Take screenshot (global)" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    float rotation_angle = 0.0f;
    bool rotate = true;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);
        updateInput(window);  // Handles ESC and P key globally!

        // Toggle rotation
        static bool space_was_pressed = false;
        bool space_is_pressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space_is_pressed && !space_was_pressed) {
            rotate = !rotate;
            std::cout << "Rotation: " << (rotate ? "ON" : "OFF") << std::endl;
        }
        space_was_pressed = space_is_pressed;

        if (rotate) {
            rotation_angle += 30.0f * elapsed;
            if (rotation_angle > 360.0f) rotation_angle -= 360.0f;
        }

        mat4 T = translate(vec3(0.0f, 0.0f, -5.0f));
        mat4 R = rotate_y(rotation_angle);
        mat4 model_mat = T * R;
        
        glUniformMatrix4fv(model_loc, 1, GL_FALSE, model_mat.m);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        shader.use();
        texture.bind(0);
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &points_vbo);
    glDeleteBuffers(1, &texcoords_vbo);

    gl_log("Exercise 6 completed\n");
}

REGISTER_EXERCISE("6. Texture Mapping", runExercise6)