#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise13.h"
#include "graphics/shader.h"
#include "graphics/mesh.h"
#include "graphics/cubemap.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

// Skybox cube vertices (big cube centered at origin)
static GLfloat skybox_points[] = {
    -10.0f,  10.0f, -10.0f,
    -10.0f, -10.0f, -10.0f,
     10.0f, -10.0f, -10.0f,
     10.0f, -10.0f, -10.0f,
     10.0f,  10.0f, -10.0f,
    -10.0f,  10.0f, -10.0f,
    
    -10.0f, -10.0f,  10.0f,
    -10.0f, -10.0f, -10.0f,
    -10.0f,  10.0f, -10.0f,
    -10.0f,  10.0f, -10.0f,
    -10.0f,  10.0f,  10.0f,
    -10.0f, -10.0f,  10.0f,
    
     10.0f, -10.0f, -10.0f,
     10.0f, -10.0f,  10.0f,
     10.0f,  10.0f,  10.0f,
     10.0f,  10.0f,  10.0f,
     10.0f,  10.0f, -10.0f,
     10.0f, -10.0f, -10.0f,
    
    -10.0f, -10.0f,  10.0f,
    -10.0f,  10.0f,  10.0f,
     10.0f,  10.0f,  10.0f,
     10.0f,  10.0f,  10.0f,
     10.0f, -10.0f,  10.0f,
    -10.0f, -10.0f,  10.0f,
    
    -10.0f,  10.0f, -10.0f,
     10.0f,  10.0f, -10.0f,
     10.0f,  10.0f,  10.0f,
     10.0f,  10.0f,  10.0f,
    -10.0f,  10.0f,  10.0f,
    -10.0f,  10.0f, -10.0f,
    
    -10.0f, -10.0f, -10.0f,
    -10.0f, -10.0f,  10.0f,
     10.0f, -10.0f, -10.0f,
     10.0f, -10.0f, -10.0f,
    -10.0f, -10.0f,  10.0f,
     10.0f, -10.0f,  10.0f
};

void runExercise13(GLFWwindow* window) {
    gl_log("Running Exercise 13 - Cube Maps: Sky Box and Environment Mapping\n");
    
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

    // Create skybox VAO
    GLuint skybox_vbo, skybox_vao;
    glGenBuffers(1, &skybox_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, skybox_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skybox_points), &skybox_points, GL_STATIC_DRAW);
    
    glGenVertexArrays(1, &skybox_vao);
    glBindVertexArray(skybox_vao);
    glEnableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, skybox_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);

    // Load cube map
    CubeMap cubemap;
    if (!cubemap.loadFromFiles(
        "assets/skybox/negz.jpg",
        "assets/skybox/posz.jpg",
        "assets/skybox/posy.jpg",
        "assets/skybox/negy.jpg",
        "assets/skybox/negx.jpg",
        "assets/skybox/posx.jpg"
    )) {
        std::cerr << "Failed to load cube map! Create skybox textures in assets/skybox/" << std::endl;
        std::cerr << "You need: front.png, back.png, top.png, bottom.png, left.png, right.png" << std::endl;
        return;
    }

    // Load Suzanne mesh for reflection/refraction
    Mesh mesh;
    if (!mesh.loadFromFile("assets/models/suzanne.obj")) {
        std::cerr << "Failed to load mesh!" << std::endl;
        return;
    }

    // Load skybox shaders
    Shader skybox_shader;
    if (!skybox_shader.loadFromFiles(
        "shaders/exercises/exercise13/skybox_vertex.glsl",
        "shaders/exercises/exercise13/skybox_fragment.glsl"
    )) {
        std::cerr << "Failed to load skybox shader" << std::endl;
        return;
    }

    // Load reflection shaders
    Shader reflect_shader;
    if (!reflect_shader.loadFromFiles(
        "shaders/exercises/exercise13/reflection_vertex.glsl",
        "shaders/exercises/exercise13/reflection_fragment.glsl"
    )) {
        std::cerr << "Failed to load reflection shader" << std::endl;
        return;
    }

    // Get uniform locations - skybox
    int skybox_proj_loc = glGetUniformLocation(skybox_shader.programme, "proj");
    int skybox_view_loc = glGetUniformLocation(skybox_shader.programme, "view");
    int skybox_tex_loc = glGetUniformLocation(skybox_shader.programme, "cube_texture");

    // Get uniform locations - reflection
    int reflect_model_loc = glGetUniformLocation(reflect_shader.programme, "model");
    int reflect_view_loc = glGetUniformLocation(reflect_shader.programme, "view");
    int reflect_proj_loc = glGetUniformLocation(reflect_shader.programme, "proj");
    int reflect_tex_loc = glGetUniformLocation(reflect_shader.programme, "cube_texture");
    int reflect_mode_loc = glGetUniformLocation(reflect_shader.programme, "mode");

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    // Camera setup
    vec3 cam_pos(0.0f, 0.0f, 5.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = 0.0f;
    
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);

    int reflection_mode = 0;  // 0 = reflection, 1 = refraction
    bool show_skybox = true;
    bool show_mesh = true;
    bool rotate = true;
    float rotation_angle = 0.0f;
    
    // Mouse control
    double prev_mouse_x = 0.0, prev_mouse_y = 0.0;
    bool first_mouse = true;
    float mouse_sensitivity = 0.2f;

    std::cout << "\n=== Exercise 13 - Cube Maps ===" << std::endl;
    std::cout << "Sky boxes and environment mapping!" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  RIGHT CLICK + DRAG - Rotate camera" << std::endl;
    std::cout << "  R - Toggle reflection/refraction" << std::endl;
    std::cout << "  B - Toggle skybox ON/OFF" << std::endl;
    std::cout << "  M - Toggle mesh ON/OFF" << std::endl;
    std::cout << "  SPACE - Toggle rotation" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    std::cout << "\nMode: " << (reflection_mode == 0 ? "REFLECTION" : "REFRACTION") << std::endl;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Toggle reflection/refraction
        static bool r_was_pressed = false;
        bool r_is_pressed = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
        if (r_is_pressed && !r_was_pressed) {
            reflection_mode = 1 - reflection_mode;
            std::cout << "\nMode: " << (reflection_mode == 0 ? "REFLECTION" : "REFRACTION") << std::endl;
        }
        r_was_pressed = r_is_pressed;

        // Toggle skybox
        static bool b_was_pressed = false;
        bool b_is_pressed = glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS;
        if (b_is_pressed && !b_was_pressed) {
            show_skybox = !show_skybox;
            std::cout << "Skybox: " << (show_skybox ? "ON" : "OFF") << std::endl;
        }
        b_was_pressed = b_is_pressed;

        // Toggle mesh
        static bool m_was_pressed = false;
        bool m_is_pressed = glfwGetKey(window, GLFW_KEY_M) == GLFW_PRESS;
        if (m_is_pressed && !m_was_pressed) {
            show_mesh = !show_mesh;
            std::cout << "Mesh: " << (show_mesh ? "ON" : "OFF") << std::endl;
        }
        m_was_pressed = m_is_pressed;

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
            rotation_angle += 30.0f * elapsed;
            if (rotation_angle > 360.0f) rotation_angle -= 360.0f;
        }

        // View matrix
        mat4 view_mat = rotate_x(-cam_pitch) * rotate_y(-cam_yaw) * 
                        translate(vec3(-cam_pos.v[0], -cam_pos.v[1], -cam_pos.v[2]));

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        // 1. Draw skybox FIRST with depth masking disabled
        if (show_skybox) {
            glDepthMask(GL_FALSE);
            
            skybox_shader.use();
            glActiveTexture(GL_TEXTURE0);
            cubemap.bind();
            glUniform1i(skybox_tex_loc, 0);
            glUniformMatrix4fv(skybox_proj_loc, 1, GL_FALSE, proj_mat.m);
            glUniformMatrix4fv(skybox_view_loc, 1, GL_FALSE, view_mat.m);
            
            glBindVertexArray(skybox_vao);
            glDrawArrays(GL_TRIANGLES, 0, 36);
            
            glDepthMask(GL_TRUE);
        }

        // 2. Draw reflective/refractive mesh
        if (show_mesh) {
            mat4 model_mat = rotate_y(rotation_angle);
            
            reflect_shader.use();
            glActiveTexture(GL_TEXTURE0);
            cubemap.bind();
            glUniform1i(reflect_tex_loc, 0);
            glUniformMatrix4fv(reflect_model_loc, 1, GL_FALSE, model_mat.m);
            glUniformMatrix4fv(reflect_view_loc, 1, GL_FALSE, view_mat.m);
            glUniformMatrix4fv(reflect_proj_loc, 1, GL_FALSE, proj_mat.m);
            glUniform1i(reflect_mode_loc, reflection_mode);
            
            mesh.draw();
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &skybox_vao);
    glDeleteBuffers(1, &skybox_vbo);

    gl_log("Exercise 13 completed\n");
}

REGISTER_EXERCISE(13, "Cube Maps & Environment Mapping", runExercise13)