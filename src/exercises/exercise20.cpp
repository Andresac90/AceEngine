#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include "exercises/exercise20.h"
#include "graphics/shader.h"
#include "graphics/mesh.h"
#include "graphics/texture.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

// Create framebuffer with color and depth textures
bool create_framebuffer(int width, int height, GLuint* fb, GLuint* fb_tex, GLuint* depth_tex) {
    // Create framebuffer
    glGenFramebuffers(1, fb);
    glBindFramebuffer(GL_FRAMEBUFFER, *fb);
    
    // Create color texture
    glGenTextures(1, fb_tex);
    glBindTexture(GL_TEXTURE_2D, *fb_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Attach color texture to framebuffer
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, *fb_tex, 0);
    
    // Create depth texture
    glGenTextures(1, depth_tex);
    glBindTexture(GL_TEXTURE_2D, *depth_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, width, height, 0, 
                 GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    // Attach depth texture to framebuffer
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, *depth_tex, 0);
    
    // Specify draw buffer
    GLenum draw_bufs[] = { GL_COLOR_ATTACHMENT0 };
    glDrawBuffers(1, draw_bufs);
    
    // Check framebuffer completeness
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        gl_log_err("ERROR: Framebuffer incomplete!\n");
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        return false;
    }
    
    // Unbind framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    
    gl_log("Created framebuffer %dx%d\n", width, height);
    return true;
}

// Create screen-space quad VAO
GLuint create_screen_quad() {
    // Vertex positions (clip space)
    float quad_pos[] = {
        -1.0f, -1.0f,
         1.0f, -1.0f,
         1.0f,  1.0f,
         1.0f,  1.0f,
        -1.0f,  1.0f,
        -1.0f, -1.0f
    };
    
    // Texture coordinates
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

void runExercise20(GLFWwindow* window) {
    gl_log("Running Exercise 20 - Framebuffer & Post-Processing\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        return;
    }

    glViewport(0, 0, g_fb_width, g_fb_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Create framebuffer for render-to-texture
    GLuint fb, fb_tex, depth_tex;
    if (!create_framebuffer(g_fb_width, g_fb_height, &fb, &fb_tex, &depth_tex)) {
        std::cerr << "Failed to create framebuffer!" << std::endl;
        return;
    }

    // Load scene shaders
    Shader scene_shader;
    if (!scene_shader.loadFromFiles(
        "shaders/exercises/exercise20/scene_vertex.glsl",
        "shaders/exercises/exercise20/scene_fragment.glsl"
    )) {
        std::cerr << "Failed to load scene shaders" << std::endl;
        return;
    }

    // Load post-processing shaders
    Shader post_shader;
    if (!post_shader.loadFromFiles(
        "shaders/exercises/exercise20/post_vertex.glsl",
        "shaders/exercises/exercise20/post_fragment.glsl"
    )) {
        std::cerr << "Failed to load post-processing shaders" << std::endl;
        return;
    }

    // Load mesh
    Mesh suzanne;
    if (!suzanne.loadFromFile("assets/models/suzanne.obj")) {
        std::cerr << "Failed to load Suzanne mesh" << std::endl;
        return;
    }

    // Load texture
    Texture checkerboard;
    if (!checkerboard.loadFromFile("assets/textures/checkerboard.png")) {
        std::cerr << "Warning: Failed to load texture" << std::endl;
    }

    // Create screen-space quad
    GLuint quad_vao = create_screen_quad();

    // Get uniform locations for scene shader
    int scene_model_loc = glGetUniformLocation(scene_shader.programme, "model");
    int scene_view_loc = glGetUniformLocation(scene_shader.programme, "view");
    int scene_proj_loc = glGetUniformLocation(scene_shader.programme, "proj");
    int scene_light_pos_loc = glGetUniformLocation(scene_shader.programme, "light_pos");
    int scene_view_pos_loc = glGetUniformLocation(scene_shader.programme, "view_pos");
    int scene_tex_loc = glGetUniformLocation(scene_shader.programme, "tex");

    // Get uniform locations for post-processing shader
    int post_tex_loc = glGetUniformLocation(post_shader.programme, "tex");
    int post_effect_loc = glGetUniformLocation(post_shader.programme, "effect_mode");

    glClearColor(0.2f, 0.3f, 0.4f, 1.0f);

    // Camera setup
    vec3 cam_pos(0.0f, 0.0f, 5.0f);
    vec3 cam_target(0.0f, 0.0f, 0.0f);
    vec3 cam_up(0.0f, 1.0f, 0.0f);
    
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);
    mat4 view_mat = look_at(cam_pos, cam_target, cam_up);

    vec3 light_pos(2.0f, 2.0f, 2.0f);

    int effect_mode = 1;  // Start with split-screen invert

    std::cout << "\n=== Exercise 20 - Framebuffer & Post-Processing ===" << std::endl;
    std::cout << "Render-to-texture with post-processing effects!" << std::endl;
    std::cout << "\nPost-Processing Effects:" << std::endl;
    std::cout << "  0 - No effect" << std::endl;
    std::cout << "  1 - Split-screen invert (default)" << std::endl;
    std::cout << "  2 - Grayscale" << std::endl;
    std::cout << "  3 - Sepia tone" << std::endl;
    std::cout << "  4 - Edge detection" << std::endl;
    std::cout << "  5 - Blur" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  0-5 - Change effect" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    double prev_time = glfwGetTime();
    float rotation = 0.0f;

    while (!glfwWindowShouldClose(window)) {
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Rotate suzanne
        rotation += 30.0f * elapsed;
        
        // Create rotation matrix manually (Y-axis rotation)
        float rad = rotation * (3.14159265359f / 180.0f);  // Convert to radians
        mat4 model_mat = identity_mat4();
        model_mat.m[0] = cosf(rad);   // cos(θ)
        model_mat.m[2] = sinf(rad);   // sin(θ)
        model_mat.m[8] = -sinf(rad);  // -sin(θ)
        model_mat.m[10] = cosf(rad);  // cos(θ)

        // Effect selection
        for (int i = 0; i <= 5; i++) {
            static bool keys_were_pressed[6] = {false};
            bool key_is_pressed = glfwGetKey(window, GLFW_KEY_0 + i) == GLFW_PRESS;
            if (key_is_pressed && !keys_were_pressed[i]) {
                effect_mode = i;
                const char* effect_names[] = {"None", "Split Invert", "Grayscale", 
                                             "Sepia", "Edge Detection", "Blur"};
                std::cout << "Effect: " << effect_names[i] << std::endl;
            }
            keys_were_pressed[i] = key_is_pressed;
        }

        updateInput(window);

        // ========== FIRST PASS: Render scene to framebuffer ==========
        glBindFramebuffer(GL_FRAMEBUFFER, fb);
        glViewport(0, 0, g_fb_width, g_fb_height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        scene_shader.use();
        glUniformMatrix4fv(scene_model_loc, 1, GL_FALSE, model_mat.m);
        glUniformMatrix4fv(scene_view_loc, 1, GL_FALSE, view_mat.m);
        glUniformMatrix4fv(scene_proj_loc, 1, GL_FALSE, proj_mat.m);
        glUniform3f(scene_light_pos_loc, light_pos.v[0], light_pos.v[1], light_pos.v[2]);
        glUniform3f(scene_view_pos_loc, cam_pos.v[0], cam_pos.v[1], cam_pos.v[2]);

        glActiveTexture(GL_TEXTURE0);
        checkerboard.bind();
        glUniform1i(scene_tex_loc, 0);

        suzanne.draw();

        // ========== SECOND PASS: Render quad with post-processing ==========
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, g_fb_width, g_fb_height);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        post_shader.use();
        glUniform1i(post_effect_loc, effect_mode);

        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, fb_tex);
        glUniform1i(post_tex_loc, 0);

        glBindVertexArray(quad_vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    glDeleteFramebuffers(1, &fb);
    glDeleteTextures(1, &fb_tex);
    glDeleteTextures(1, &depth_tex);
    glDeleteVertexArrays(1, &quad_vao);

    gl_log("Exercise 20 completed\n");
}

REGISTER_EXERCISE(20, "Framebuffer & Post-Processing", runExercise20)