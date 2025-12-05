#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include "exercises/exercise22.h"
#include "graphics/shader.h"
#include "graphics/mesh.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

// Camera state
struct Camera {
    vec3 position;
    float yaw;
    float pitch;
    float speed;
    float sensitivity;
    double last_mouse_x;
    double last_mouse_y;
    bool first_mouse;
};

static Camera camera = {
    vec3(0.0f, 5.0f, 15.0f),  // position
    -90.0f,                    // yaw
    -20.0f,                    // pitch
    10.0f,                     // speed
    0.1f,                      // sensitivity
    0.0, 0.0,                  // last mouse position
    true                       // first mouse
};

// Create shadow map framebuffer
bool create_shadow_map(int size, GLuint* fb, GLuint* depth_tex) {
    glGenFramebuffers(1, fb);
    glBindFramebuffer(GL_FRAMEBUFFER, *fb);
    
    // Create depth texture
    glGenTextures(1, depth_tex);
    glBindTexture(GL_TEXTURE_2D, *depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, size, size, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    
    // Bilinear filtering for smooth shadows
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Attach depth texture to framebuffer
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, 
                          GL_TEXTURE_2D, *depth_tex, 0);
    
    // No color output
    GLenum draw_bufs[] = { GL_NONE };
    glDrawBuffers(1, draw_bufs);
    glReadBuffer(GL_NONE);
    
    // Check completeness
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        gl_log_err("ERROR: Shadow map framebuffer not complete!\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_log("Created shadow map %dx%d\n", size, size);
    return true;
}

// Create debug quad for displaying shadow map
GLuint create_debug_quad() {
    float quad_pos[] = {
        0.5f,  0.5f,
        1.0f,  0.5f,
        1.0f,  1.0f,
        1.0f,  1.0f,
        0.5f,  1.0f,
        0.5f,  0.5f
    };
    
    float quad_st[] = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        1.0f, 1.0f,
        1.0f, 1.0f,
        0.0f, 1.0f,
        0.0f, 0.0f
    };
    
    GLuint vbo_pos, vbo_st, vao;
    
    glGenBuffers(1, &vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_pos), quad_pos, GL_STATIC_DRAW);
    
    glGenBuffers(1, &vbo_st);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_st);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_st), quad_st, GL_STATIC_DRAW);
    
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_st);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);
    
    return vao;
}

// Create ground plane
GLuint create_ground_plane(int* point_count) {
    float plane_pos[] = {
        -20.0f, -1.0f, -20.0f,
        -20.0f, -1.0f,  20.0f,
         20.0f, -1.0f,  20.0f,
         20.0f, -1.0f,  20.0f,
         20.0f, -1.0f, -20.0f,
        -20.0f, -1.0f, -20.0f
    };
    
    float plane_normals[] = {
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 1.0f, 0.0f
    };
    
    *point_count = 6;
    
    GLuint vbo_pos, vbo_norm, vao;
    
    glGenBuffers(1, &vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(plane_pos), plane_pos, GL_STATIC_DRAW);
    
    glGenBuffers(1, &vbo_norm);
    glBindBuffer(GL_ARRAY_BUFFER, vbo_norm);
    glBufferData(GL_ARRAY_BUFFER, sizeof(plane_normals), plane_normals, GL_STATIC_DRAW);
    
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_pos);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    glBindBuffer(GL_ARRAY_BUFFER, vbo_norm);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);
    
    return vao;
}

void runExercise22(GLFWwindow* window) {
    gl_log("Running Exercise 22 - Shadow Mapping\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        return;
    }

    glViewport(0, 0, g_fb_width, g_fb_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Create shadow map
    const int shadow_size = 1024;
    GLuint shadow_fb, shadow_depth_tex;
    if (!create_shadow_map(shadow_size, &shadow_fb, &shadow_depth_tex)) {
        return;
    }

    // Load depth shaders (for shadow map generation)
    Shader depth_shader;
    if (!depth_shader.loadFromFiles(
        "shaders/exercises/exercise22/depth_vertex.glsl",
        "shaders/exercises/exercise22/depth_fragment.glsl"
    )) {
        std::cerr << "Failed to load depth shaders" << std::endl;
        return;
    }

    // Load scene shaders (with shadow receiving)
    Shader scene_shader;
    if (!scene_shader.loadFromFiles(
        "shaders/exercises/exercise22/scene_vertex.glsl",
        "shaders/exercises/exercise22/scene_fragment.glsl"
    )) {
        std::cerr << "Failed to load scene shaders" << std::endl;
        return;
    }

    // Load debug quad shaders
    Shader quad_shader;
    if (!quad_shader.loadFromFiles(
        "shaders/exercises/exercise22/quad_vertex.glsl",
        "shaders/exercises/exercise22/quad_fragment.glsl"
    )) {
        std::cerr << "Failed to load quad shaders" << std::endl;
        return;
    }

    // Load monkey mesh
    Mesh suzanne;
    if (!suzanne.loadFromFile("assets/models/suzanne.obj")) {
        std::cerr << "Failed to load Suzanne" << std::endl;
        return;
    }

    // Create ground plane
    int ground_point_count;
    GLuint ground_vao = create_ground_plane(&ground_point_count);

    // Create debug quad
    GLuint quad_vao = create_debug_quad();

    // Get uniform locations - Depth shader
    int depth_P_loc = glGetUniformLocation(depth_shader.programme, "P");
    int depth_V_loc = glGetUniformLocation(depth_shader.programme, "V");
    int depth_M_loc = glGetUniformLocation(depth_shader.programme, "M");

    // Get uniform locations - Scene shader
    int scene_P_loc = glGetUniformLocation(scene_shader.programme, "P");
    int scene_V_loc = glGetUniformLocation(scene_shader.programme, "V");
    int scene_M_loc = glGetUniformLocation(scene_shader.programme, "M");
    int scene_caster_P_loc = glGetUniformLocation(scene_shader.programme, "caster_P");
    int scene_caster_V_loc = glGetUniformLocation(scene_shader.programme, "caster_V");
    int scene_depth_map_loc = glGetUniformLocation(scene_shader.programme, "depth_map");
    int scene_colour_loc = glGetUniformLocation(scene_shader.programme, "colour");
    int scene_light_dir_loc = glGetUniformLocation(scene_shader.programme, "light_dir");

    // Get uniform locations - Quad shader
    int quad_depth_tex_loc = glGetUniformLocation(quad_shader.programme, "depth_tex");

    // Light setup (directional spotlight from above)
    vec3 light_pos(0.0f, 10.0f, 0.0f);
    vec3 light_target(0.0f, 0.0f, 0.0f);
    vec3 light_up(0.0f, 0.0f, -1.0f);
    
    // Create light's camera matrices
    float near_plane = 5.0f;
    float far_plane = 20.0f;
    mat4 caster_P = perspective(45.0f, 1.0f, near_plane, far_plane);
    mat4 caster_V = look_at(light_pos, light_target, light_up);

    glClearColor(0.2f, 0.3f, 0.4f, 1.0f);

    std::cout << "\n=== Exercise 22 - Shadow Mapping ===" << std::endl;
    std::cout << "Texture projection shadows!" << std::endl;
    std::cout << "\nFeatures:" << std::endl;
    std::cout << "  • Shadow map: " << shadow_size << "x" << shadow_size << std::endl;
    std::cout << "  • Bilinear filtering for smooth shadows" << std::endl;
    std::cout << "  • Self-shadowing prevention (epsilon)" << std::endl;
    std::cout << "  • Debug view in top-right corner" << std::endl;
    std::cout << "\nCamera Controls (Unreal Engine style):" << std::endl;
    std::cout << "  RIGHT MOUSE BUTTON + WASD - Fly camera" << std::endl;
    std::cout << "  RIGHT MOUSE BUTTON + Mouse - Look around" << std::endl;
    std::cout << "  Q/E - Move up/down" << std::endl;
    std::cout << "  SPACE - Animate light position" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    double prev_time = glfwGetTime();
    bool animate_light = false;
    bool mouse_look_active = false;

    while (!glfwWindowShouldClose(window)) {
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Toggle light animation
        static bool space_was_pressed = false;
        bool space_is_pressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space_is_pressed && !space_was_pressed) {
            animate_light = !animate_light;
            std::cout << "Light animation: " << (animate_light ? "ON" : "OFF") << std::endl;
        }
        space_was_pressed = space_is_pressed;

        // Check if right mouse button is pressed (Unreal Engine style)
        bool right_mouse = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        
        if (right_mouse && !mouse_look_active) {
            // Just started pressing right mouse
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            camera.first_mouse = true;
            mouse_look_active = true;
        } else if (!right_mouse && mouse_look_active) {
            // Just released right mouse
            glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            mouse_look_active = false;
        }

        // Mouse look (only when right mouse button is held)
        if (mouse_look_active) {
            double mouse_x, mouse_y;
            glfwGetCursorPos(window, &mouse_x, &mouse_y);

            if (camera.first_mouse) {
                camera.last_mouse_x = mouse_x;
                camera.last_mouse_y = mouse_y;
                camera.first_mouse = false;
            }

            double x_offset = mouse_x - camera.last_mouse_x;
            double y_offset = camera.last_mouse_y - mouse_y;  // Reversed
            camera.last_mouse_x = mouse_x;
            camera.last_mouse_y = mouse_y;

            camera.yaw += x_offset * camera.sensitivity;
            camera.pitch += y_offset * camera.sensitivity;

            // Clamp pitch
            if (camera.pitch > 89.0f) camera.pitch = 89.0f;
            if (camera.pitch < -89.0f) camera.pitch = -89.0f;
        }

        // Calculate camera direction
        float yaw_rad = camera.yaw * ONE_DEG_IN_RAD;
        float pitch_rad = camera.pitch * ONE_DEG_IN_RAD;
        
        vec3 forward(
            cosf(pitch_rad) * cosf(yaw_rad),
            sinf(pitch_rad),
            cosf(pitch_rad) * sinf(yaw_rad)
        );
        
        vec3 right(
            cosf(yaw_rad - 3.14159f / 2.0f),
            0.0f,
            sinf(yaw_rad - 3.14159f / 2.0f)
        );
        
        vec3 up(0.0f, 1.0f, 0.0f);

        // Camera movement (only when right mouse button is held)
        if (mouse_look_active) {
            float velocity = camera.speed * elapsed;

            if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) {
                camera.position.v[0] += forward.v[0] * velocity;
                camera.position.v[1] += forward.v[1] * velocity;
                camera.position.v[2] += forward.v[2] * velocity;
            }
            if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) {
                camera.position.v[0] -= forward.v[0] * velocity;
                camera.position.v[1] -= forward.v[1] * velocity;
                camera.position.v[2] -= forward.v[2] * velocity;
            }
            if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) {
                camera.position.v[0] -= right.v[0] * velocity;
                camera.position.v[1] -= right.v[1] * velocity;
                camera.position.v[2] -= right.v[2] * velocity;
            }
            if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) {
                camera.position.v[0] += right.v[0] * velocity;
                camera.position.v[1] += right.v[1] * velocity;
                camera.position.v[2] += right.v[2] * velocity;
            }
            if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) {
                camera.position.v[1] += velocity;
            }
            if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) {
                camera.position.v[1] -= velocity;
            }
        }

        // Calculate view matrix
        vec3 cam_target(
            camera.position.v[0] + forward.v[0],
            camera.position.v[1] + forward.v[1],
            camera.position.v[2] + forward.v[2]
        );
        
        float aspect = (float)g_fb_width / (float)g_fb_height;
        mat4 P = perspective(67.0f, aspect, 0.1f, 100.0f);
        mat4 V = look_at(camera.position, cam_target, up);

        // Animate light position
        if (animate_light) {
            float time = (float)curr_time;
            light_pos.v[0] = 5.0f * sinf(time * 0.5f);
            light_pos.v[2] = 5.0f * cosf(time * 0.5f);
            caster_V = look_at(light_pos, light_target, light_up);
        }

        // Calculate light direction for shading
        vec3 light_dir(
            light_target.v[0] - light_pos.v[0],
            light_target.v[1] - light_pos.v[1],
            light_target.v[2] - light_pos.v[2]
        );
        float len = sqrtf(light_dir.v[0]*light_dir.v[0] + 
                         light_dir.v[1]*light_dir.v[1] + 
                         light_dir.v[2]*light_dir.v[2]);
        light_dir.v[0] /= len;
        light_dir.v[1] /= len;
        light_dir.v[2] /= len;

        updateInput(window);

        // ========== PASS 1: Render shadow map ==========
        glBindFramebuffer(GL_FRAMEBUFFER, shadow_fb);
        glViewport(0, 0, shadow_size, shadow_size);
        glClear(GL_DEPTH_BUFFER_BIT);

        depth_shader.use();
        glUniformMatrix4fv(depth_P_loc, 1, GL_FALSE, caster_P.m);
        glUniformMatrix4fv(depth_V_loc, 1, GL_FALSE, caster_V.m);

        // Draw monkey (casts shadow)
        mat4 monkey_M = identity_mat4();
        glUniformMatrix4fv(depth_M_loc, 1, GL_FALSE, monkey_M.m);
        suzanne.draw();

        // ========== PASS 2: Render scene with shadows ==========
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_fb_width, g_fb_height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        scene_shader.use();
        glUniformMatrix4fv(scene_P_loc, 1, GL_FALSE, P.m);
        glUniformMatrix4fv(scene_V_loc, 1, GL_FALSE, V.m);
        glUniformMatrix4fv(scene_caster_P_loc, 1, GL_FALSE, caster_P.m);
        glUniformMatrix4fv(scene_caster_V_loc, 1, GL_FALSE, caster_V.m);
        glUniform3f(scene_light_dir_loc, light_dir.v[0], light_dir.v[1], light_dir.v[2]);

        // Bind shadow map
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, shadow_depth_tex);
        glUniform1i(scene_depth_map_loc, 0);

        // Draw monkey (receives shadow)
        glUniformMatrix4fv(scene_M_loc, 1, GL_FALSE, monkey_M.m);
        glUniform3f(scene_colour_loc, 0.8f, 0.3f, 0.3f);
        suzanne.draw();

        // Draw ground plane (receives shadow)
        mat4 ground_M = identity_mat4();
        glUniformMatrix4fv(scene_M_loc, 1, GL_FALSE, ground_M.m);
        glUniform3f(scene_colour_loc, 0.3f, 0.8f, 0.3f);
        glBindVertexArray(ground_vao);
        glDrawArrays(GL_TRIANGLES, 0, ground_point_count);

        // ========== Draw debug quad (shadow map visualization) ==========
        quad_shader.use();
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, shadow_depth_tex);
        glUniform1i(quad_depth_tex_loc, 0);
        glBindVertexArray(quad_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Restore cursor before exiting
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    // Cleanup
    glDeleteFramebuffers(1, &shadow_fb);
    glDeleteTextures(1, &shadow_depth_tex);
    glDeleteVertexArrays(1, &quad_vao);
    glDeleteVertexArrays(1, &ground_vao);

    gl_log("Exercise 22 completed\n");
}

REGISTER_EXERCISE(22, "Shadow Mapping", runExercise22)