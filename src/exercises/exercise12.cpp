#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise12.h"
#include "graphics/shader.h"
#include "graphics/mesh.h"
#include "graphics/texture.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise12(GLFWwindow* window) {
    gl_log("Running Exercise 12 - Normal Mapping\n");
    
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

    // Load mesh (must have normals for tangent generation!)
    Mesh mesh;
    if (!mesh.loadFromFile("assets/models/suzanne.obj")) {
        std::cerr << "Failed to load mesh!" << std::endl;
        return;
    }

    if (!mesh.hasTangents()) {
        std::cerr << "WARNING: Mesh doesn't have tangents! Normal mapping won't work." << std::endl;
        std::cerr << "Make sure the mesh was exported with normals." << std::endl;
    }

    std::cout << "Mesh loaded: " << mesh.getVertexCount() << " vertices" << std::endl;

    // Load textures
    Texture diffuse_tex;
    if (!diffuse_tex.loadFromFile("assets/textures/suzanne_diffuse.png")) {
        std::cerr << "Failed to load diffuse texture!" << std::endl;
        return;
    }

    Texture normal_tex;
    if (!normal_tex.loadFromFile("assets/textures/suzanne_normal.png")) {
        std::cerr << "WARNING: Failed to load normal texture!" << std::endl;
        std::cerr << "Create assets/textures/suzanne_normal.png for normal mapping." << std::endl;
    }

    // Load shaders
    Shader shader;
    if (!shader.loadFromFiles("shaders/exercises/exercise12/vertex.glsl", 
                               "shaders/exercises/exercise12/fragment.glsl")) {
        std::cerr << "Failed to load shader" << std::endl;
        return;
    }

    shader.use();
    
    int model_loc = glGetUniformLocation(shader.programme, "model");
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    int light_dir_loc = glGetUniformLocation(shader.programme, "light_dir_world");
    int cam_pos_loc = glGetUniformLocation(shader.programme, "cam_pos_world");
    int normal_map_loc = glGetUniformLocation(shader.programme, "normal_map");
    int diffuse_map_loc = glGetUniformLocation(shader.programme, "diffuse_map");
    int use_normal_loc = glGetUniformLocation(shader.programme, "use_normal_map");
    int spec_exp_loc = glGetUniformLocation(shader.programme, "specular_exponent");

    // Set texture sampler uniforms
    glActiveTexture(GL_TEXTURE0);
    diffuse_tex.bind();
    glUniform1i(diffuse_map_loc, 0);
    
    glActiveTexture(GL_TEXTURE1);
    normal_tex.bind();
    glUniform1i(normal_map_loc, 1);

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    // Camera setup
    vec3 cam_pos(0.0f, 0.0f, 3.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = 0.0f;
    
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);

    // Light direction (world space)
    vec3 light_dir_world(0.5f, -1.0f, -0.5f);
    
    bool use_normal_map = true;
    float specular_exponent = 50.0f;
    
    // Mouse control
    double prev_mouse_x = 0.0, prev_mouse_y = 0.0;
    bool first_mouse = true;
    float mouse_sensitivity = 0.2f;

    glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);

    std::cout << "\n=== Exercise 12 - Normal Mapping ===" << std::endl;
    std::cout << "Adding surface detail without extra geometry!" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  RIGHT CLICK + DRAG - Rotate camera" << std::endl;
    std::cout << "  N - Toggle normal mapping ON/OFF" << std::endl;
    std::cout << "  ARROW KEYS - Move light direction" << std::endl;
    std::cout << "  UP/DOWN - Adjust specular exponent" << std::endl;
    std::cout << "  SPACE - Toggle rotation" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    std::cout << "\nNormal mapping: " << (use_normal_map ? "ON" : "OFF") << std::endl;

    float rotation_angle = 0.0f;
    bool rotate = true;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Toggle normal mapping
        static bool n_was_pressed = false;
        bool n_is_pressed = glfwGetKey(window, GLFW_KEY_N) == GLFW_PRESS;
        if (n_is_pressed && !n_was_pressed) {
            use_normal_map = !use_normal_map;
            std::cout << "\nNormal mapping: " << (use_normal_map ? "ON" : "OFF") << std::endl;
        }
        n_was_pressed = n_is_pressed;

        // Toggle rotation
        static bool space_was_pressed = false;
        bool space_is_pressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space_is_pressed && !space_was_pressed) {
            rotate = !rotate;
            std::cout << "Rotation: " << (rotate ? "ON" : "OFF") << std::endl;
        }
        space_was_pressed = space_is_pressed;

        // Adjust light direction
        float light_speed = 1.0f * elapsed;
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) light_dir_world.v[0] -= light_speed;
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) light_dir_world.v[0] += light_speed;
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) light_dir_world.v[1] += light_speed;
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) light_dir_world.v[1] -= light_speed;

        // Adjust specular exponent
        if (glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS) {
            specular_exponent += 20.0f * elapsed;
            if (specular_exponent > 500.0f) specular_exponent = 500.0f;
        }
        if (glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS) {
            specular_exponent -= 20.0f * elapsed;
            if (specular_exponent < 1.0f) specular_exponent = 1.0f;
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
        
        // Bind textures
        glActiveTexture(GL_TEXTURE0);
        diffuse_tex.bind();
        glUniform1i(diffuse_map_loc, 0);
        
        glActiveTexture(GL_TEXTURE1);
        normal_tex.bind();
        glUniform1i(normal_map_loc, 1);
        
        glUniformMatrix4fv(model_loc, 1, GL_FALSE, model_mat.m);
        glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);
        glUniform3fv(light_dir_loc, 1, light_dir_world.v);
        glUniform3fv(cam_pos_loc, 1, cam_pos.v);
        glUniform1i(use_normal_loc, use_normal_map ? 1 : 0);
        glUniform1f(spec_exp_loc, specular_exponent);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        mesh.draw();

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    gl_log("Exercise 12 completed\n");
}

REGISTER_EXERCISE(12, "Normal Mapping", runExercise12)