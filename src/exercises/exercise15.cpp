#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise15.h"
#include "graphics/shader.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise15(GLFWwindow* window) {
    gl_log("Running Exercise 15 - Tessellation Shaders\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        return;
    }

    glViewport(0, 0, g_fb_width, g_fb_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Simple triangle vertices
    GLfloat points[] = {
        -1.0f, -1.0f,  0.0f,
         1.0f, -1.0f,  0.0f,
         0.0f,  1.0f,  0.0f
    };

    // Create VAO and VBO
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);
    
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    // Load shaders (with tessellation shaders!)
    Shader shader;
    if (!shader.loadFromFiles(
        "shaders/exercises/exercise15/vertex.glsl",
        "shaders/exercises/exercise15/fragment.glsl",
        "",  // No geometry shader
        "shaders/exercises/exercise15/tess_control.glsl",   // Tessellation control
        "shaders/exercises/exercise15/tess_eval.glsl"       // Tessellation evaluation
    )) {
        std::cerr << "Failed to load shaders" << std::endl;
        return;
    }

    shader.use();
    
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    int tess_inner_loc = glGetUniformLocation(shader.programme, "tess_level_inner");
    int tess_outer_loc = glGetUniformLocation(shader.programme, "tess_level_outer");
    int color_mode_loc = glGetUniformLocation(shader.programme, "color_mode");

    glClearColor(0.1f, 0.1f, 0.2f, 1.0f);

    // Camera setup
    vec3 cam_pos(0.0f, 0.0f, 5.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = 0.0f;
    
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);

    // Tessellation levels
    float tess_level_inner = 1.0f;
    float tess_level_outer = 1.0f;
    
    bool wireframe = true;
    bool rotate = true;
    float rotation_angle = 0.0f;
    int color_mode = 0;
    
    // Mouse control
    double prev_mouse_x = 0.0, prev_mouse_y = 0.0;
    bool first_mouse = true;
    float mouse_sensitivity = 0.2f;

    glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);

    // Set number of vertices per patch (3 for triangles)
    glPatchParameteri(GL_PATCH_VERTICES, 3);

    std::cout << "\n=== Exercise 15 - Tessellation Shaders ===" << std::endl;
    std::cout << "Dynamically subdividing geometry on the GPU!" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  RIGHT CLICK + DRAG - Rotate camera" << std::endl;
    std::cout << "  1/2 - Decrease/Increase INNER tessellation level" << std::endl;
    std::cout << "  3/4 - Decrease/Increase OUTER tessellation level" << std::endl;
    std::cout << "  W - Toggle wireframe/solid" << std::endl;
    std::cout << "  C - Toggle color mode" << std::endl;
    std::cout << "  SPACE - Toggle rotation" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    std::cout << "\nTessellation levels:" << std::endl;
    std::cout << "  Inner: " << tess_level_inner << std::endl;
    std::cout << "  Outer: " << tess_level_outer << std::endl;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Adjust inner tessellation level
        if (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS) {
            tess_level_inner -= 1.0f * elapsed;
            if (tess_level_inner < 1.0f) tess_level_inner = 1.0f;
            std::cout << "Inner: " << tess_level_inner << std::endl;
        }
        if (glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS) {
            tess_level_inner += 1.0f * elapsed;
            if (tess_level_inner > 64.0f) tess_level_inner = 64.0f;
            std::cout << "Inner: " << tess_level_inner << std::endl;
        }

        // Adjust outer tessellation level
        if (glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS) {
            tess_level_outer -= 1.0f * elapsed;
            if (tess_level_outer < 1.0f) tess_level_outer = 1.0f;
            std::cout << "Outer: " << tess_level_outer << std::endl;
        }
        if (glfwGetKey(window, GLFW_KEY_4) == GLFW_PRESS) {
            tess_level_outer += 1.0f * elapsed;
            if (tess_level_outer > 64.0f) tess_level_outer = 64.0f;
            std::cout << "Outer: " << tess_level_outer << std::endl;
        }

        // Toggle wireframe
        static bool w_was_pressed = false;
        bool w_is_pressed = glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS;
        if (w_is_pressed && !w_was_pressed) {
            wireframe = !wireframe;
            if (wireframe) {
                glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
            } else {
                glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
            }
            std::cout << "Wireframe: " << (wireframe ? "ON" : "OFF") << std::endl;
        }
        w_was_pressed = w_is_pressed;

        // Toggle color mode
        static bool c_was_pressed = false;
        bool c_is_pressed = glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS;
        if (c_is_pressed && !c_was_pressed) {
            color_mode = 1 - color_mode;
            std::cout << "Color mode: " << (color_mode == 0 ? "WHITE" : "GRADIENT") << std::endl;
        }
        c_was_pressed = c_is_pressed;

        // Toggle rotation
        static bool space_was_pressed = false;
        bool space_is_pressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space_is_pressed && !space_was_pressed) {
            rotate = !rotate;
            std::cout << "Rotation: " << (rotate ? "ON" : "OFF") << std::endl;
        }
        space_was_pressed = space_is_pressed;

        // Mouse camera control
        bool right_mouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        
        double mouse_x, mouse_y;
        glfwGetCursorPos(window, &mouse_x, &mouse_y);
        
        if (first_mouse) {
            prev_mouse_x = mouse_x;
            prev_mouse_y = mouse_y;
            first_mouse = false;
        }
        
        double mouse_dx = prev_mouse_x - mouse_x;
        double mouse_dy = prev_mouse_y - mouse_y;
        prev_mouse_x = mouse_x;
        prev_mouse_y = mouse_y;

        if (right_mouse && (mouse_dx != 0.0 || mouse_dy != 0.0)) {
            cam_yaw += mouse_dx * mouse_sensitivity;
            cam_pitch += mouse_dy * mouse_sensitivity;
            if (cam_pitch > 89.0f) cam_pitch = 89.0f;
            if (cam_pitch < -89.0f) cam_pitch = -89.0f;
        }

        updateInput(window);

        // Update rotation
        if (rotate) {
            rotation_angle += 20.0f * elapsed;
            if (rotation_angle > 360.0f) rotation_angle -= 360.0f;
        }

        // View matrix
        mat4 view_mat = rotate_x(-cam_pitch) * rotate_y(-cam_yaw) * 
                        translate(vec3(-cam_pos.v[0], -cam_pos.v[1], -cam_pos.v[2]));
        
        // Apply rotation to the triangle
        mat4 model_mat = rotate_y(rotation_angle);
        mat4 final_view = view_mat * model_mat;

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        shader.use();
        glUniformMatrix4fv(view_loc, 1, GL_FALSE, final_view.m);
        glUniform1f(tess_inner_loc, tess_level_inner);
        glUniform1f(tess_outer_loc, tess_level_outer);
        glUniform1i(color_mode_loc, color_mode);
        
        glBindVertexArray(vao);
        
        // Draw as PATCHES (not triangles!)
        glDrawArrays(GL_PATCHES, 0, 3);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Restore polygon mode
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);

    gl_log("Exercise 15 completed\n");
}

REGISTER_EXERCISE(15, "Tessellation Shaders", runExercise15)