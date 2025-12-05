#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise11.h"
#include "graphics/shader.h"
#include "graphics/mesh.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise11(GLFWwindow* window) {
    gl_log("Running Exercise 11 - Distance Fog\n");
    
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

    std::cout << "Mesh loaded: " << mesh.getVertexCount() << " vertices" << std::endl;

    // Load shaders
    Shader shader;
    if (!shader.loadFromFiles("shaders/exercises/exercise11/vertex.glsl", 
                               "shaders/exercises/exercise11/fragment.glsl")) {
        std::cerr << "Failed to load shader" << std::endl;
        return;
    }

    shader.use();
    
    int model_loc = glGetUniformLocation(shader.programme, "model");
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    int light_pos_loc = glGetUniformLocation(shader.programme, "light_position_eye");
    int spec_exp_loc = glGetUniformLocation(shader.programme, "specular_exponent");
    int fog_colour_loc = glGetUniformLocation(shader.programme, "fog_colour");
    int min_fog_loc = glGetUniformLocation(shader.programme, "min_fog_radius");
    int max_fog_loc = glGetUniformLocation(shader.programme, "max_fog_radius");
    int use_fog_loc = glGetUniformLocation(shader.programme, "use_fog");

    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);

    // Camera setup
    vec3 cam_pos(0.0f, 0.0f, 5.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = 0.0f;
    
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);

    // Light in eye space
    vec3 light_pos_eye(2.0f, 2.0f, 2.0f);
    
    // Fog parameters
    vec3 fog_colour(0.2f, 0.2f, 0.2f);  // Gray fog
    float min_fog_radius = 3.0f;
    float max_fog_radius = 10.0f;
    bool use_fog = true;
    
    // Mouse control
    double prev_mouse_x = 0.0, prev_mouse_y = 0.0;
    bool first_mouse = true;
    float mouse_sensitivity = 0.2f;

    glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);
    glUniform3fv(light_pos_loc, 1, light_pos_eye.v);
    glUniform1f(spec_exp_loc, 50.0f);

    std::cout << "\n=== Exercise 11 - Distance Fog ===" << std::endl;
    std::cout << "Understanding fog for atmosphere and depth perception!" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  RIGHT CLICK + DRAG - Rotate camera" << std::endl;
    std::cout << "  W/S - Move forward/backward" << std::endl;
    std::cout << "  F - Toggle fog ON/OFF" << std::endl;
    std::cout << "  1/2 - Decrease/Increase MIN fog distance" << std::endl;
    std::cout << "  3/4 - Decrease/Increase MAX fog distance" << std::endl;
    std::cout << "  SPACE - Toggle rotation" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    std::cout << "\nFog enabled: " << (use_fog ? "YES" : "NO") << std::endl;
    std::cout << "Min fog distance: " << min_fog_radius << std::endl;
    std::cout << "Max fog distance: " << max_fog_radius << std::endl;

    float rotation_angle = 0.0f;
    bool rotate = true;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Toggle fog
        static bool f_was_pressed = false;
        bool f_is_pressed = glfwGetKey(window, GLFW_KEY_F) == GLFW_PRESS;
        if (f_is_pressed && !f_was_pressed) {
            use_fog = !use_fog;
            std::cout << "\nFog: " << (use_fog ? "ON" : "OFF") << std::endl;
        }
        f_was_pressed = f_is_pressed;

        // Adjust min fog distance
        if (glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS) {
            min_fog_radius -= 1.0f * elapsed;
            if (min_fog_radius < 0.1f) min_fog_radius = 0.1f;
            std::cout << "Min fog: " << min_fog_radius << std::endl;
        }
        if (glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS) {
            min_fog_radius += 1.0f * elapsed;
            if (min_fog_radius > max_fog_radius - 0.5f) min_fog_radius = max_fog_radius - 0.5f;
            std::cout << "Min fog: " << min_fog_radius << std::endl;
        }

        // Adjust max fog distance
        if (glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS) {
            max_fog_radius -= 1.0f * elapsed;
            if (max_fog_radius < min_fog_radius + 0.5f) max_fog_radius = min_fog_radius + 0.5f;
            std::cout << "Max fog: " << max_fog_radius << std::endl;
        }
        if (glfwGetKey(window, GLFW_KEY_4) == GLFW_PRESS) {
            max_fog_radius += 1.0f * elapsed;
            if (max_fog_radius > 50.0f) max_fog_radius = 50.0f;
            std::cout << "Max fog: " << max_fog_radius << std::endl;
        }

        // Toggle rotation
        static bool space_was_pressed = false;
        bool space_is_pressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space_is_pressed && !space_was_pressed) {
            rotate = !rotate;
            std::cout << "Rotation: " << (rotate ? "ON" : "OFF") << std::endl;
        }
        space_was_pressed = space_is_pressed;

        // Camera movement
        float move_speed = 2.0f * elapsed;
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) cam_pos.v[2] -= move_speed;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) cam_pos.v[2] += move_speed;

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
        glUniform3fv(fog_colour_loc, 1, fog_colour.v);
        glUniform1f(min_fog_loc, min_fog_radius);
        glUniform1f(max_fog_loc, max_fog_radius);
        glUniform1i(use_fog_loc, use_fog ? 1 : 0);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        mesh.draw();

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    gl_log("Exercise 11 completed\n");
}

REGISTER_EXERCISE(11, "Distance Fog", runExercise11)