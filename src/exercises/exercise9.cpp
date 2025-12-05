#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise9.h"
#include "graphics/shader.h"
#include "graphics/mesh.h"
#include "graphics/texture.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise9(GLFWwindow* window) {
    gl_log("Running Exercise 9 - Multi-Texture Lighting (Diffuse + Specular + Emission)\n");
    
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

    // Load textures
    Texture diffuse_tex;
    if (!diffuse_tex.loadFromFile("assets/textures/suzanne_diffuse.png")) {
        std::cerr << "Failed to load diffuse texture!" << std::endl;
        std::cerr << "Make sure to UV unwrap Suzanne and export the texture!" << std::endl;
        return;
    }

    Texture specular_tex;
    if (!specular_tex.loadFromFile("assets/textures/suzanne_specular.png")) {
        std::cerr << "Failed to load specular texture!" << std::endl;
        std::cerr << "Using a default white specular map" << std::endl;
        // Could create a default 1x1 white texture here
    }

    std::cout << "Textures loaded successfully!" << std::endl;

    // Load shaders
    Shader shader;
    if (!shader.loadFromFiles("shaders/exercises/exercise9/vertex.glsl", 
                               "shaders/exercises/exercise9/fragment.glsl")) {
        std::cerr << "Failed to load shader" << std::endl;
        return;
    }

    shader.use();
    
    // Get uniform locations
    int model_loc = glGetUniformLocation(shader.programme, "model");
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    int light_pos_loc = glGetUniformLocation(shader.programme, "light_position_eye");
    int spec_exp_loc = glGetUniformLocation(shader.programme, "specular_exponent");
    int diffuse_map_loc = glGetUniformLocation(shader.programme, "diffuse_map");
    int specular_map_loc = glGetUniformLocation(shader.programme, "specular_map");
    
    std::cout << "\nUniform locations:" << std::endl;
    std::cout << "  diffuse_map: " << diffuse_map_loc << std::endl;
    std::cout << "  specular_map: " << specular_map_loc << std::endl;
    std::cout << "  light_position_eye: " << light_pos_loc << std::endl;
    std::cout << "  specular_exponent: " << spec_exp_loc << std::endl;

    // Set sampler uniforms (which texture unit to read from)
    glUniform1i(diffuse_map_loc, 0);   // Read from GL_TEXTURE0
    glUniform1i(specular_map_loc, 1);  // Read from GL_TEXTURE1

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    // Camera setup
    vec3 cam_pos(0.0f, 0.0f, 3.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = 0.0f;
    
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);
    
    // Light setup (in eye space, moves with camera)
    vec3 light_pos_eye(0.0f, 0.0f, 2.0f);
    
    // Mouse control
    double prev_mouse_x = 0.0, prev_mouse_y = 0.0;
    bool first_mouse = true;
    float mouse_sensitivity = 0.2f;

    std::cout << "\n=== Exercise 9 - Multi-Texture Lighting ===" << std::endl;
    std::cout << "Using multiple texture maps for advanced lighting!" << std::endl;
    std::cout << "  Diffuse Map (RGB) = Surface color (Kd)" << std::endl;
    std::cout << "  Specular Map (RGB) = Shininess control (Ks)" << std::endl;
    std::cout << "  Emission Map (Alpha) = Self-illumination" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  RIGHT CLICK + DRAG - Rotate camera" << std::endl;
    std::cout << "  SPACE - Toggle rotation" << std::endl;
    std::cout << "  UP/DOWN - Adjust specular exponent" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    float rotation_angle = 0.0f;
    bool rotate = true;
    float specular_exp = 50.0f;

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

        // Adjust specular exponent
        bool exp_changed = false;
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) {
            specular_exp += 50.0f * elapsed;
            if (specular_exp > 500.0f) specular_exp = 500.0f;
            exp_changed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) {
            specular_exp -= 50.0f * elapsed;
            if (specular_exp < 1.0f) specular_exp = 1.0f;
            exp_changed = true;
        }
        
        static double last_exp_print = 0.0;
        if (exp_changed && (curr_time - last_exp_print > 0.2)) {
            std::cout << "Specular exponent: " << (int)specular_exp << std::endl;
            last_exp_print = curr_time;
        }

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
        glUniform3fv(light_pos_loc, 1, light_pos_eye.v);
        glUniform1f(spec_exp_loc, specular_exp);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        // Bind textures to different texture units
        glActiveTexture(GL_TEXTURE0);
        diffuse_tex.bind();
        
        glActiveTexture(GL_TEXTURE1);
        specular_tex.bind();

        mesh.draw();

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    gl_log("Exercise 9 completed\n");
}

REGISTER_EXERCISE(9, "Multi-Texture Lighting", runExercise9)