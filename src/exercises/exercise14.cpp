#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>
#include "exercises/exercise14.h"
#include "graphics/shader.h"
#include "graphics/texture.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

struct Billboard {
    float x, y, z;    // Position
    float size;       // Size of billboard
};

void runExercise14(GLFWwindow* window) {
    gl_log("Running Exercise 14 - Geometry Shaders: Billboards\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        return;
    }

    glViewport(0, 0, g_fb_width, g_fb_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Generate random billboard positions
    std::srand(static_cast<unsigned int>(std::time(nullptr)));
    const int num_billboards = 50;
    std::vector<Billboard> billboards;
    
    for (int i = 0; i < num_billboards; i++) {
        Billboard bb;
        bb.x = (rand() % 2000 - 1000) / 100.0f;  // -10 to 10
        bb.y = (rand() % 1000) / 100.0f;         // 0 to 10
        bb.z = (rand() % 2000 - 1000) / 100.0f;  // -10 to 10
        bb.size = 0.3f + (rand() % 100) / 200.0f; // 0.3 to 0.8
        billboards.push_back(bb);
    }

    // Create vertex data (positions and sizes)
    std::vector<GLfloat> vertex_data;
    for (const auto& bb : billboards) {
        vertex_data.push_back(bb.x);
        vertex_data.push_back(bb.y);
        vertex_data.push_back(bb.z);
        vertex_data.push_back(bb.size);
    }

    // Create VAO and VBO
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vertex_data.size() * sizeof(GLfloat), 
                 vertex_data.data(), GL_STATIC_DRAW);
    
    // Position attribute (location 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), (void*)0);
    glEnableVertexAttribArray(0);
    
    // Size attribute (location 1)
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), 
                          (void*)(3 * sizeof(GLfloat)));
    glEnableVertexAttribArray(1);

    // Load optional texture
    Texture billboard_tex;
    bool has_texture = billboard_tex.loadFromFile("assets/textures/tree.png");
    if (!has_texture) {
        std::cout << "No billboard texture found, using procedural circles" << std::endl;
    }

    // Load shaders (with geometry shader!)
    Shader shader;
    if (!shader.loadFromFiles(
        "shaders/exercises/exercise14/vertex.glsl",
        "shaders/exercises/exercise14/fragment.glsl",
        "shaders/exercises/exercise14/geometry.glsl"  // Geometry shader!
    )) {
        std::cerr << "Failed to load shaders" << std::endl;
        return;
    }

    shader.use();
    
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    int tex_loc = glGetUniformLocation(shader.programme, "billboard_texture");
    int use_tex_loc = glGetUniformLocation(shader.programme, "use_texture");

    glClearColor(0.2f, 0.3f, 0.5f, 1.0f);

    // Camera setup
    vec3 cam_pos(0.0f, 2.0f, 10.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = 0.0f;
    
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);

    // Mouse control
    double prev_mouse_x = 0.0, prev_mouse_y = 0.0;
    bool first_mouse = true;
    float mouse_sensitivity = 0.2f;

    glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);

    std::cout << "\n=== Exercise 14 - Geometry Shaders: Billboards ===" << std::endl;
    std::cout << "Creating quads from points that always face the camera!" << std::endl;
    std::cout << "\nBillboards generated: " << num_billboards << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  RIGHT CLICK + DRAG - Look around" << std::endl;
    std::cout << "  WASD - Move camera" << std::endl;
    std::cout << "  T - Toggle texture/procedural" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    bool use_texture = has_texture;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Toggle texture
        static bool t_was_pressed = false;
        bool t_is_pressed = glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS;
        if (t_is_pressed && !t_was_pressed) {
            use_texture = !use_texture;
            std::cout << "Mode: " << (use_texture ? "TEXTURE" : "PROCEDURAL") << std::endl;
        }
        t_was_pressed = t_is_pressed;

        // Camera movement
        float move_speed = 5.0f * elapsed;
        vec3 forward(0.0f, 0.0f, -1.0f);
        vec3 right(1.0f, 0.0f, 0.0f);
        
        // Rotate direction vectors by yaw
        float yaw_rad = cam_yaw * M_PI / 180.0f;
        forward.v[0] = -sin(yaw_rad);
        forward.v[2] = -cos(yaw_rad);
        right.v[0] = cos(yaw_rad);
        right.v[2] = -sin(yaw_rad);
        
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
            cam_pos.v[0] += forward.v[0] * move_speed;
            cam_pos.v[2] += forward.v[2] * move_speed;
        }
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
            cam_pos.v[0] -= forward.v[0] * move_speed;
            cam_pos.v[2] -= forward.v[2] * move_speed;
        }
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
            cam_pos.v[0] -= right.v[0] * move_speed;
            cam_pos.v[2] -= right.v[2] * move_speed;
        }
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
            cam_pos.v[0] += right.v[0] * move_speed;
            cam_pos.v[2] += right.v[2] * move_speed;
        }
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
            cam_pos.v[1] -= move_speed;
        }
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {
            cam_pos.v[1] += move_speed;
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

        // View matrix
        mat4 view_mat = rotate_x(-cam_pitch) * rotate_y(-cam_yaw) * 
                        translate(vec3(-cam_pos.v[0], -cam_pos.v[1], -cam_pos.v[2]));

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        shader.use();
        
        if (use_texture && has_texture) {
            glActiveTexture(GL_TEXTURE0);
            billboard_tex.bind();
            glUniform1i(tex_loc, 0);
            glUniform1i(use_tex_loc, 1);
        } else {
            glUniform1i(use_tex_loc, 0);
        }
        
        glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);
        
        glBindVertexArray(vao);
        
        // Draw as POINTS - geometry shader will create quads!
        glDrawArrays(GL_POINTS, 0, num_billboards);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);

    gl_log("Exercise 14 completed\n");
}

REGISTER_EXERCISE(14, "Geometry Shaders: Billboards", runExercise14)