#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise7.h"
#include "graphics/shader.h"
#include "graphics/mesh.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise7(GLFWwindow* window) {
    gl_log("Running Exercise 7 - Mesh Loading (Suzanne)\n");
    
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

    // Load Suzanne mesh
    Mesh mesh;
    if (!mesh.loadFromFile("assets/models/suzanne.obj")) {
        std::cerr << "Failed to load mesh!" << std::endl;
        return;
    }

    std::cout << "Mesh loaded successfully!" << std::endl;
    std::cout << "  Vertices: " << mesh.getVertexCount() << std::endl;
    std::cout << "  Triangles: " << mesh.getTriangleCount() << std::endl;

    // Load shaders (exercise 7 specific shaders with normal debug mode)
    Shader shader;
    if (!shader.loadFromFiles("shaders/exercises/exercise7/vertex.glsl", 
                               "shaders/exercises/exercise7/fragment.glsl")) {
        std::cerr << "Failed to load shader" << std::endl;
        return;
    }

    shader.use();
    
    int model_loc = glGetUniformLocation(shader.programme, "model");
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    int spec_exp_loc = glGetUniformLocation(shader.programme, "specular_exponent");
    int use_blinn_loc = glGetUniformLocation(shader.programme, "use_blinn");
    int show_normals_loc = glGetUniformLocation(shader.programme, "show_normals");

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    // Camera setup
    vec3 cam_pos(0.0f, 0.0f, 3.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = 0.0f;
    
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);
    
    // Mouse control
    double prev_mouse_x = 0.0, prev_mouse_y = 0.0;
    bool first_mouse = true;
    float mouse_sensitivity = 0.2f;

    std::cout << "\n=== Exercise 7 - Mesh Loading (Suzanne) ===" << std::endl;
    std::cout << "Meet Suzanne, Blender's mascot!" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  RIGHT CLICK + DRAG - Rotate camera" << std::endl;
    std::cout << "  SPACE - Toggle rotation" << std::endl;
    std::cout << "  N - Toggle normal visualization (flat/smooth shading debug)" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    float rotation_angle = 0.0f;
    bool rotate = true;
    bool show_normals = false;

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

        // Toggle normal visualization
        static bool n_was_pressed = false;
        bool n_is_pressed = glfwGetKey(window, GLFW_KEY_N) == GLFW_PRESS;
        if (n_is_pressed && !n_was_pressed) {
            show_normals = !show_normals;
            std::cout << "\n=== Normal Visualization: " << (show_normals ? "ON" : "OFF") << " ===" << std::endl;
            if (show_normals) {
                std::cout << "Showing normals as colors (RGB = XYZ)" << std::endl;
                std::cout << "Flat colors per face = FLAT SHADING" << std::endl;
                std::cout << "Smooth color gradients = SMOOTH SHADING" << std::endl;
            } else {
                std::cout << "Back to Phong lighting" << std::endl;
            }
        }
        n_was_pressed = n_is_pressed;

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
            rotation_angle += 30.0f * elapsed;
            if (rotation_angle > 360.0f) rotation_angle -= 360.0f;
        }

        // Matrices
        mat4 R = rotate_y(rotation_angle);
        mat4 model_mat = R;
        
        mat4 view_mat = rotate_x(-cam_pitch) * rotate_y(-cam_yaw) * 
                        translate(vec3(-cam_pos.v[0], -cam_pos.v[1], -cam_pos.v[2]));
        
        shader.use();
        glUniformMatrix4fv(model_loc, 1, GL_FALSE, model_mat.m);
        glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);
        glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);
        glUniform1f(spec_exp_loc, 50.0f);
        glUniform1i(use_blinn_loc, 1);
        glUniform1i(show_normals_loc, show_normals ? 1 : 0);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        mesh.draw();

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    gl_log("Exercise 7 completed\n");
}

REGISTER_EXERCISE(7, "Mesh Loading (Suzanne)", runExercise7)