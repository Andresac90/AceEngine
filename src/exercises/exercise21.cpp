#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include <vector>
#include "exercises/exercise21.h"
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
    vec3(0.0f, 2.0f, 20.0f),  // position
    -90.0f,                    // yaw
    -10.0f,                    // pitch
    10.0f,                     // speed
    0.1f,                      // sensitivity
    0.0, 0.0,                  // last mouse position
    true                       // first mouse
};

// Create G-Buffer framebuffer
bool create_gbuffer(int width, int height, GLuint* fb, GLuint* fb_tex_p, 
                    GLuint* fb_tex_n, GLuint* depth_tex) {
    glGenFramebuffers(1, fb);
    glBindFramebuffer(GL_FRAMEBUFFER, *fb);
    
    // Position texture (RGB16F for eye-space positions)
    glGenTextures(1, fb_tex_p);
    glBindTexture(GL_TEXTURE_2D, *fb_tex_p);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, 
                 GL_RGB, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, 
                          GL_TEXTURE_2D, *fb_tex_p, 0);
    
    // Normal texture (RGB16F for eye-space normals)
    glGenTextures(1, fb_tex_n);
    glBindTexture(GL_TEXTURE_2D, *fb_tex_n);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB16F, width, height, 0, 
                 GL_RGB, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT1, 
                          GL_TEXTURE_2D, *fb_tex_n, 0);
    
    // Depth texture
    glGenTextures(1, depth_tex);
    glBindTexture(GL_TEXTURE_2D, *depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT32F, width, height, 0,
                 GL_DEPTH_COMPONENT, GL_FLOAT, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, 
                          GL_TEXTURE_2D, *depth_tex, 0);
    
    // Specify which color attachments to use
    GLenum draw_bufs[] = { GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1 };
    glDrawBuffers(2, draw_bufs);
    
    // Check completeness
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        gl_log_err("ERROR: G-Buffer framebuffer not complete!\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    gl_log("Created G-Buffer %dx%d\n", width, height);
    return true;
}

// Recreate G-Buffer when window is resized
void recreate_gbuffer(int width, int height, GLuint* fb, GLuint* fb_tex_p, 
                      GLuint* fb_tex_n, GLuint* depth_tex) {
    // Delete old framebuffer and textures
    if (*fb != 0) {
        glDeleteFramebuffers(1, fb);
        glDeleteTextures(1, fb_tex_p);
        glDeleteTextures(1, fb_tex_n);
        glDeleteTextures(1, depth_tex);
    }
    
    // Create new framebuffer with new dimensions
    create_gbuffer(width, height, fb, fb_tex_p, fb_tex_n, depth_tex);
}

void runExercise21(GLFWwindow* window) {
    gl_log("Running Exercise 21 - Deferred Shading\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        return;
    }

    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Create G-Buffer
    GLuint fb, fb_tex_p, fb_tex_n, depth_tex;
    if (!create_gbuffer(g_fb_width, g_fb_height, &fb, &fb_tex_p, &fb_tex_n, &depth_tex)) {
        return;
    }

    // Load first pass shaders
    Shader first_pass_shader;
    if (!first_pass_shader.loadFromFiles(
        "shaders/exercises/exercise21/first_pass_vertex.glsl",
        "shaders/exercises/exercise21/first_pass_fragment.glsl"
    )) {
        std::cerr << "Failed to load first pass shaders" << std::endl;
        return;
    }

    // Load second pass shaders
    Shader second_pass_shader;
    if (!second_pass_shader.loadFromFiles(
        "shaders/exercises/exercise21/second_pass_vertex.glsl",
        "shaders/exercises/exercise21/second_pass_fragment.glsl"
    )) {
        std::cerr << "Failed to load second pass shaders" << std::endl;
        return;
    }

    // Load meshes
    Mesh suzanne;
    if (!suzanne.loadFromFile("assets/models/suzanne.obj")) {
        std::cerr << "Failed to load Suzanne" << std::endl;
        return;
    }

    // Load sphere for light volumes (or use suzanne if no sphere available)
    Mesh sphere;
    if (!sphere.loadFromFile("assets/models/sphere.obj")) {
        gl_log("Warning: Could not load sphere.obj, using suzanne for light volumes\n");
        sphere = suzanne;  // Fallback
    }

    // Get uniform locations - First Pass
    int first_P_loc = glGetUniformLocation(first_pass_shader.programme, "P");
    int first_V_loc = glGetUniformLocation(first_pass_shader.programme, "V");
    int first_M_loc = glGetUniformLocation(first_pass_shader.programme, "M");

    // Get uniform locations - Second Pass
    int second_P_loc = glGetUniformLocation(second_pass_shader.programme, "P");
    int second_V_loc = glGetUniformLocation(second_pass_shader.programme, "V");
    int second_M_loc = glGetUniformLocation(second_pass_shader.programme, "M");
    int p_tex_loc = glGetUniformLocation(second_pass_shader.programme, "p_tex");
    int n_tex_loc = glGetUniformLocation(second_pass_shader.programme, "n_tex");
    int ls_loc = glGetUniformLocation(second_pass_shader.programme, "ls");
    int ld_loc = glGetUniformLocation(second_pass_shader.programme, "ld");
    int lp_loc = glGetUniformLocation(second_pass_shader.programme, "lp");
    int viewport_loc = glGetUniformLocation(second_pass_shader.programme, "viewport_size");

    // Setup lights (32 lights arranged around the scene)
    const int num_lights = 32;
    const float radius = 5.0f;
    
    vec3 lp[num_lights];  // Light positions
    vec3 ld[num_lights];  // Light diffuse colors
    vec3 ls[num_lights];  // Light specular colors
    mat4 lM[num_lights];  // Light model matrices
    
    for (int i = 0; i < num_lights; i++) {
        // Generate light positions in a CIRCLE around the monkeys
        float angle = ((float)i / (float)num_lights) * 2.0f * 3.14159265f;
        float radius_dist = 8.0f + 4.0f * sinf(i * 0.3f);  // Varying distance
        
        float x = cosf(angle) * radius_dist;
        float y = -2.0f + 6.0f * ((float)(i % 4) / 4.0f);  // Spread vertically
        float z = sinf(angle) * radius_dist;
        lp[i] = vec3(x, y, z);
        
        // Generate vibrant light colors
        float r = 0.5f + 0.5f * sinf(i * 0.3f);
        float g = 0.5f + 0.5f * cosf(i * 0.7f);
        float b = 0.5f + 0.5f * sinf(i * 1.1f);
        ld[i] = vec3(r, g, b);
        // Specular is half of diffuse
        ls[i] = vec3(r * 0.5f, g * 0.5f, b * 0.5f);
        
        // Create light volume matrix (scaled sphere + translation)
        mat4 scale_mat = scale(radius, radius, radius);
        mat4 trans_mat = translate(lp[i]);
        lM[i] = trans_mat * scale_mat;
    }

    // Setup scene objects (monkeys)
    const int num_monkeys = 10;
    mat4 monkey_M[num_monkeys];
    
    for (int i = 0; i < num_monkeys; i++) {
        float x = -10.0f + (i % 5) * 5.0f;
        float y = (i / 5) * 5.0f - 2.5f;
        float z = 0.0f;
        monkey_M[i] = translate(x, y, z);
    }

    std::cout << "\n=== Exercise 21 - Deferred Shading ===" << std::endl;
    std::cout << "G-Buffer based deferred rendering!" << std::endl;
    std::cout << num_lights << " lights illuminating " << num_monkeys << " objects" << std::endl;
    std::cout << "\nFeatures:" << std::endl;
    std::cout << "  • Two-pass rendering (geometry + lighting)" << std::endl;
    std::cout << "  • Multiple render targets (G-Buffer)" << std::endl;
    std::cout << "  • Additive light blending" << std::endl;
    std::cout << "  • Light volume optimization" << std::endl;
    std::cout << "  • Lights arranged in circle around scene" << std::endl;
    std::cout << "\nCamera Controls (Unreal Engine style):" << std::endl;
    std::cout << "  RIGHT MOUSE BUTTON + WASD - Fly camera" << std::endl;
    std::cout << "  RIGHT MOUSE BUTTON + Mouse - Look around" << std::endl;
    std::cout << "  Q/E - Move up/down (while holding RMB)" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    double prev_time = glfwGetTime();
    int prev_fb_width = g_fb_width;
    int prev_fb_height = g_fb_height;
    bool mouse_look_active = false;

    while (!glfwWindowShouldClose(window)) {
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Check if window was resized and recreate G-Buffer
        if (g_fb_width != prev_fb_width || g_fb_height != prev_fb_height) {
            recreate_gbuffer(g_fb_width, g_fb_height, &fb, &fb_tex_p, &fb_tex_n, &depth_tex);
            prev_fb_width = g_fb_width;
            prev_fb_height = g_fb_height;
            glViewport(0, 0, g_fb_width, g_fb_height);
        }

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

        updateInput(window);

        // Update camera matrices
        float aspect = (float)g_fb_width / (float)g_fb_height;
        mat4 P = perspective(67.0f, aspect, 0.1f, 100.0f);
        mat4 V = look_at(camera.position, cam_target, up);

        // ========== FIRST PASS: Render to G-Buffer ==========
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glViewport(0, 0, g_fb_width, g_fb_height);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glDisable(GL_BLEND);
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);

        first_pass_shader.use();
        glUniformMatrix4fv(first_P_loc, 1, GL_FALSE, P.m);
        glUniformMatrix4fv(first_V_loc, 1, GL_FALSE, V.m);

        // Draw all monkeys
        for (int i = 0; i < num_monkeys; i++) {
            glUniformMatrix4fv(first_M_loc, 1, GL_FALSE, monkey_M[i].m);
            suzanne.draw();
        }

        // ========== SECOND PASS: Lighting ==========
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_fb_width, g_fb_height);
        glClearColor(0.2f, 0.2f, 0.2f, 0.0f);  // Ambient light as background
        glClear(GL_COLOR_BUFFER_BIT);
        
        // Setup additive blending
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE);
        glDisable(GL_DEPTH_TEST);
        glDepthMask(GL_FALSE);

        // Bind G-Buffer textures
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fb_tex_p);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, fb_tex_n);

        second_pass_shader.use();
        glUniform1i(p_tex_loc, 0);
        glUniform1i(n_tex_loc, 1);
        glUniformMatrix4fv(second_P_loc, 1, GL_FALSE, P.m);
        glUniformMatrix4fv(second_V_loc, 1, GL_FALSE, V.m);
        glUniform2f(viewport_loc, (float)g_fb_width, (float)g_fb_height);

        // Draw light volumes
        for (int i = 0; i < num_lights; i++) {
            glUniformMatrix4fv(second_M_loc, 1, GL_FALSE, lM[i].m);
            glUniform3f(lp_loc, lp[i].v[0], lp[i].v[1], lp[i].v[2]);
            glUniform3f(ld_loc, ld[i].v[0], ld[i].v[1], ld[i].v[2]);
            glUniform3f(ls_loc, ls[i].v[0], ls[i].v[1], ls[i].v[2]);
            sphere.draw();
        }

        // Re-enable depth test for next frame
        glEnable(GL_DEPTH_TEST);
        glDepthMask(GL_TRUE);
        glDisable(GL_BLEND);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Restore cursor before exiting
    glfwSetInputMode(window, GLFW_CURSOR, GLFW_CURSOR_NORMAL);

    // Cleanup
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &fb_tex_p);
    glDeleteTextures(1, &fb_tex_n);
    glDeleteTextures(1, &depth_tex);

    gl_log("Exercise 21 completed\n");
}

REGISTER_EXERCISE(21, "Deferred Shading", runExercise21)