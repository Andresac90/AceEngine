#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include "exercises/exercise4.h"
#include "graphics/shader.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

struct Triangle {
    vec3 position;  // Center position
    vec3 color;
};

void runExercise4(GLFWwindow* window) {
    gl_log("Running Exercise 4 - Virtual Camera with Frustum Culling\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        return;
    }

    glViewport(0, 0, g_fb_width, g_fb_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Base triangle template (at origin)
    GLfloat base_points[] = {
         0.0f,  1.0f,  0.0f,
         1.0f, -1.0f,  0.0f,
        -1.0f, -1.0f,  0.0f
    };

    GLfloat base_colours[] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f
    };

    // Create list of triangle positions
    std::vector<Triangle> triangles;
    
    // One big red triangle in front
    triangles.push_back({vec3(0, 0, -5), vec3(1, 0, 0)});
    
    // Grid of triangles
    for (int x = -15; x <= 15; x += 5) {
        for (int z = -10; z >= -50; z -= 5) {
            float r = (x + 15) / 30.0f;
            float g = 0.5f;
            float b = (-z - 10) / 40.0f;
            triangles.push_back({vec3(x, 0, z), vec3(r, g, b)});
        }
    }
    
    std::cout << "Created " << triangles.size() << " triangles in the scene" << std::endl;

    // Create VBOs with base triangle
    GLuint points_vbo, colours_vbo, vao;
    
    glGenBuffers(1, &points_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(base_points), base_points, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &colours_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, colours_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(base_colours), base_colours, GL_DYNAMIC_DRAW);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    glBindBuffer(GL_ARRAY_BUFFER, colours_vbo);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);

    Shader shader;
    if (!shader.loadFromFiles("shaders/exercises/exercise4/vertex.glsl", 
                               "shaders/exercises/exercise4/fragment.glsl")) {
        std::cerr << "Failed to load shader" << std::endl;
        return;
    }

    shader.use();
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    
    std::cout << "view_loc: " << view_loc << ", proj_loc: " << proj_loc << std::endl;

    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);

    // Camera
    vec3 cam_pos(0.0f, 0.0f, 2.0f);
    float cam_yaw = 0.0f;
    float cam_pitch = 0.0f;

    // Mouse variables
    double prev_mouse_x = 0.0, prev_mouse_y = 0.0;
    bool first_mouse = true;
    float mouse_sensitivity = 0.2f;

    // Frustum culling toggle
    bool culling_enabled = false;

    // Matrices
    mat4 proj_mat = perspective(67.0f, (float)g_fb_width / (float)g_fb_height, 0.1f, 100.0f);
    mat4 view_mat = rotate_x(-cam_pitch) * rotate_y(-cam_yaw) * translate(vec3(-cam_pos.v[0], -cam_pos.v[1], -cam_pos.v[2]));

    // SEND BOTH MATRICES NOW
    glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);
    glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);

    std::cout << "\n=== CONTROLS ===" << std::endl;
    std::cout << "RIGHT CLICK + DRAG - Look around" << std::endl;
    std::cout << "RIGHT CLICK + WASD - Move" << std::endl;
    std::cout << "RIGHT CLICK + Q/E - Up/Down" << std::endl;
    std::cout << "MIDDLE CLICK + DRAG - Pan" << std::endl;
    std::cout << "C - Toggle frustum culling (starts OFF)" << std::endl;
    std::cout << "ESC - Exit" << std::endl;

    float cam_speed = 5.0f;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double dt = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        bool moved = false;

        // Toggle culling with C key
        static bool c_was_pressed = false;
        bool c_is_pressed = glfwGetKey(window, GLFW_KEY_C) == GLFW_PRESS;
        if (c_is_pressed && !c_was_pressed) {
            culling_enabled = !culling_enabled;
            std::cout << "\nFrustum culling: " << (culling_enabled ? "ON" : "OFF") << std::endl;
        }
        c_was_pressed = c_is_pressed;

        bool right_mouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        bool middle_mouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_MIDDLE) == GLFW_PRESS;

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
            moved = true;
        }

        if (middle_mouse && (mouse_dx != 0.0 || mouse_dy != 0.0)) {
            float pan_speed = 0.01f;
            float yaw_rad = cam_yaw * ONE_DEG_IN_RAD;
            vec3 right(cosf(yaw_rad), 0.0f, sinf(yaw_rad));
            cam_pos.v[0] -= right.v[0] * mouse_dx * pan_speed;
            cam_pos.v[2] -= right.v[2] * mouse_dx * pan_speed;
            cam_pos.v[1] += mouse_dy * pan_speed;
            moved = true;
        }

        if (right_mouse) {
            float yaw_rad = cam_yaw * ONE_DEG_IN_RAD;
            vec3 forward(sinf(yaw_rad), 0.0f, -cosf(yaw_rad));
            vec3 right(cosf(yaw_rad), 0.0f, sinf(yaw_rad));
            
            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
                cam_pos.v[0] += forward.v[0] * cam_speed * dt;
                cam_pos.v[2] += forward.v[2] * cam_speed * dt;
                moved = true;
            }
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
                cam_pos.v[0] -= forward.v[0] * cam_speed * dt;
                cam_pos.v[2] -= forward.v[2] * cam_speed * dt;
                moved = true;
            }
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
                cam_pos.v[0] -= right.v[0] * cam_speed * dt;
                cam_pos.v[2] -= right.v[2] * cam_speed * dt;
                moved = true;
            }
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
                cam_pos.v[0] += right.v[0] * cam_speed * dt;
                cam_pos.v[2] += right.v[2] * cam_speed * dt;
                moved = true;
            }
            if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
                cam_pos.v[1] -= cam_speed * dt;
                moved = true;
            }
            if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {
                cam_pos.v[1] += cam_speed * dt;
                moved = true;
            }
        }

        updateInput(window);  // Handles ESC and P key (screenshot)
        
        if (moved) {
            view_mat = rotate_x(-cam_pitch) * rotate_y(-cam_yaw) * translate(vec3(-cam_pos.v[0], -cam_pos.v[1], -cam_pos.v[2]));
            glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);
        }

        // Extract frustum for culling
        mat4 proj_view = proj_mat * view_mat;
        Frustum frustum = extract_frustum(proj_view);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        shader.use();
        glBindVertexArray(vao);

        int triangles_drawn = 0;
        GLfloat points[9];
        GLfloat colours[9];

        // Draw all triangles with optional frustum culling
        for (const auto& tri : triangles) {
            // Frustum culling check
            if (culling_enabled) {
                float bounding_radius = 2.0f;  // Approximate radius of triangle
                if (!sphere_in_frustum(frustum, tri.position, bounding_radius)) {
                    continue;  // Skip this triangle, it's outside the frustum
                }
            }

            triangles_drawn++;

            // Update triangle position and color
            for (int i = 0; i < 3; i++) {
                points[i*3+0] = base_points[i*3+0] + tri.position.v[0];
                points[i*3+1] = base_points[i*3+1] + tri.position.v[1];
                points[i*3+2] = base_points[i*3+2] + tri.position.v[2];
                colours[i*3+0] = tri.color.v[0];
                colours[i*3+1] = tri.color.v[1];
                colours[i*3+2] = tri.color.v[2];
            }
            
            glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(points), points);
            glBindBuffer(GL_ARRAY_BUFFER, colours_vbo);
            glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(colours), colours);
            glDrawArrays(GL_TRIANGLES, 0, 3);
        }

        // Display stats every second
        static double last_print = 0.0;
        if (curr_time - last_print > 1.0) {
            int percent = triangles.size() > 0 ? (triangles_drawn * 100 / triangles.size()) : 0;
            std::cout << "Drawing " << triangles_drawn << " / " << triangles.size() 
                      << " (" << percent << "%) - Culling: " 
                      << (culling_enabled ? "ON" : "OFF") << std::endl;
            last_print = curr_time;
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &points_vbo);
    glDeleteBuffers(1, &colours_vbo);

    gl_log("Exercise 4 completed\n");
}

REGISTER_EXERCISE("4. Virtual Camera", runExercise4)