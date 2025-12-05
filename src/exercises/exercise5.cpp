#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cstring>
#include "exercises/exercise5.h"
#include "graphics/shader.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise5(GLFWwindow* window) {
    gl_log("Running Exercise 5 - Phong vs Blinn-Phong (Double-Sided) with UBO\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        return;
    }

    glViewport(0, 0, g_fb_width, g_fb_height);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LESS);

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Double-sided triangle
    GLfloat points[] = {
        // FRONT FACE
         0.0f,  0.5f,  0.0f,
         0.5f, -0.5f,  0.0f,
        -0.5f, -0.5f,  0.0f,
        // BACK FACE
         0.0f,  0.5f,  0.0f,
        -0.5f, -0.5f,  0.0f,
         0.5f, -0.5f,  0.0f
    };

    GLfloat normals[] = {
        // FRONT FACE normals
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        // BACK FACE normals
        0.0f, 0.0f, -1.0f,
        0.0f, 0.0f, -1.0f,
        0.0f, 0.0f, -1.0f
    };

    GLuint points_vbo, normals_vbo, vao;
    
    glGenBuffers(1, &points_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(points), points, GL_STATIC_DRAW);

    glGenBuffers(1, &normals_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, normals_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(normals), normals, GL_STATIC_DRAW);

    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    glBindBuffer(GL_ARRAY_BUFFER, points_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    glBindBuffer(GL_ARRAY_BUFFER, normals_vbo);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);

    Shader shader;
    if (!shader.loadFromFiles("shaders/exercises/exercise5/vertex_ubo.glsl", 
                               "shaders/exercises/exercise5/fragment.glsl")) {
        std::cerr << "Failed to load shader" << std::endl;
        return;
    }

    shader.use();
    
    // ========================================
    // WITH UBO - MODERN INTERFACE (glMapBufferRange)
    // ========================================
    std::cout << "Using UBO with modern buffer mapping interface!" << std::endl;
    
    // Camera matrices in std140 layout
    // std140 layout: mat4 = 16 floats (64 bytes), aligned to 16 bytes
    // Total size: 2 mat4s = 32 floats = 128 bytes
    const size_t CAMERA_UBO_SIZE = sizeof(float) * 32;  // 2 mat4s
    
    GLuint camera_ubo;
    glGenBuffers(1, &camera_ubo);
    glBindBuffer(GL_UNIFORM_BUFFER, camera_ubo);
    glBufferData(GL_UNIFORM_BUFFER, CAMERA_UBO_SIZE, nullptr, GL_DYNAMIC_DRAW);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, camera_ubo);
    
    std::cout << "UBO created: " << CAMERA_UBO_SIZE << " bytes, bound to binding point 0" << std::endl;
    
    // Get uniform block index
    GLuint ubo_index = glGetUniformBlockIndex(shader.programme, "CameraMatrices");
    if (ubo_index == GL_INVALID_INDEX) {
        std::cerr << "ERROR: CameraMatrices uniform block not found!" << std::endl;
        return;
    }
    
    glUniformBlockBinding(shader.programme, ubo_index, 0);
    std::cout << "CameraMatrices block bound to binding point 0" << std::endl;
    
    // Get other uniform locations
    int model_loc = glGetUniformLocation(shader.programme, "model");
    int spec_exp_loc = glGetUniformLocation(shader.programme, "specular_exponent");
    int use_blinn_loc = glGetUniformLocation(shader.programme, "use_blinn");
    
    std::cout << "\nUniform locations:" << std::endl;
    std::cout << "  model: " << model_loc << std::endl;
    std::cout << "  specular_exponent: " << spec_exp_loc << std::endl;
    std::cout << "  use_blinn: " << use_blinn_loc << std::endl;
    std::cout << "  view/proj: in UBO at binding point 0" << std::endl;

    // ========================================
    // WITHOUT UBO (Traditional approach - COMMENTED OUT)
    // ========================================
    // int view_loc = glGetUniformLocation(shader.programme, "view");
    // int proj_loc = glGetUniformLocation(shader.programme, "proj");
    //
    // Traditional update per frame:
    // glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);
    // glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);

    glClearColor(0.1f, 0.1f, 0.1f, 1.0f);

    // Setup camera matrices
    mat4 view_mat = identity_mat4();
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);

    // WITH UBO - MODERN INTERFACE: Map buffer and write directly
    glBindBuffer(GL_UNIFORM_BUFFER, camera_ubo);
    float* cam_ubo_ptr = (float*)glMapBufferRange(
        GL_UNIFORM_BUFFER,
        0,
        CAMERA_UBO_SIZE,
        GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_BUFFER_BIT
    );
    
    if (!cam_ubo_ptr) {
        std::cerr << "ERROR: Failed to map UBO buffer!" << std::endl;
        return;
    }
    
    // Copy matrices directly into mapped memory
    // std140 layout: view at offset 0, proj at offset 16 floats
    memcpy(&cam_ubo_ptr[0], view_mat.m, sizeof(float) * 16);   // View matrix
    memcpy(&cam_ubo_ptr[16], proj_mat.m, sizeof(float) * 16);  // Proj matrix
    
    glUnmapBuffer(GL_UNIFORM_BUFFER);
    
    std::cout << "Camera matrices uploaded to UBO using buffer mapping" << std::endl;
    
    // WITH UBO - OLDER INTERFACE (commented out for reference):
    // glBindBuffer(GL_UNIFORM_BUFFER, camera_ubo);
    // glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(float) * 16, proj_mat.m);
    // glBufferSubData(GL_UNIFORM_BUFFER, sizeof(float) * 16, sizeof(float) * 16, view_mat.m);

    std::cout << "\n=== Exercise 5 - Double-Sided Phong Lighting (UBO) ===" << std::endl;
    std::cout << "Using modern glMapBufferRange interface!" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  B - Toggle Blinn-Phong / Phong" << std::endl;
    std::cout << "  SPACE - Toggle rotation" << std::endl;
    std::cout << "  UP/DOWN - Adjust specular exponent" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    float rotation_angle = 0.0f;
    bool rotate = true;
    float specular_exp = 100.0f;
    bool use_blinn = false;

    std::cout << "\nCurrent mode: PHONG (classic)" << std::endl;
    std::cout << "Specular exponent: " << specular_exp << std::endl;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Toggle Blinn-Phong
        static bool b_was_pressed = false;
        bool b_is_pressed = glfwGetKey(window, GLFW_KEY_B) == GLFW_PRESS;
        if (b_is_pressed && !b_was_pressed) {
            use_blinn = !use_blinn;
            std::cout << "\n=== Switched to " << (use_blinn ? "BLINN-PHONG" : "PHONG") << " ===" << std::endl;
            if (use_blinn) {
                std::cout << "Using half-way vector (faster)" << std::endl;
            } else {
                std::cout << "Using reflection vector (classic)" << std::endl;
            }
        }
        b_was_pressed = b_is_pressed;

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
            specular_exp += 100.0f * elapsed;
            if (specular_exp > 1000.0f) specular_exp = 1000.0f;
            exp_changed = true;
        }
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) {
            specular_exp -= 100.0f * elapsed;
            if (specular_exp < 1.0f) specular_exp = 1.0f;
            exp_changed = true;
        }
        
        static double last_exp_print = 0.0;
        if (exp_changed && (curr_time - last_exp_print > 0.2)) {
            std::cout << "Specular exponent: " << (int)specular_exp << std::endl;
            last_exp_print = curr_time;
        }

        updateInput(window);

        // Update rotation
        if (rotate) {
            rotation_angle += 30.0f * elapsed;
            if (rotation_angle > 360.0f) rotation_angle -= 360.0f;
        }

        // Model matrix
        mat4 T = translate(vec3(0.0f, 0.0f, -5.0f));
        mat4 R = rotate_y(rotation_angle);
        mat4 model_mat = T * R;
        
        // WITH UBO: Only send model matrix (view/proj are in UBO)
        glUniformMatrix4fv(model_loc, 1, GL_FALSE, model_mat.m);
        glUniform1f(spec_exp_loc, specular_exp);
        glUniform1i(use_blinn_loc, use_blinn ? 1 : 0);
        
        // Note: In a real engine, if view/proj changed, you'd update UBO here:
        // glBindBuffer(GL_UNIFORM_BUFFER, camera_ubo);
        // float* ptr = (float*)glMapBufferRange(...);
        // memcpy(ptr, updated_matrices, size);
        // glUnmapBuffer(GL_UNIFORM_BUFFER);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        shader.use();
        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &points_vbo);
    glDeleteBuffers(1, &normals_vbo);
    glDeleteBuffers(1, &camera_ubo);

    gl_log("Exercise 5 completed\n");
}

REGISTER_EXERCISE(5, "Phong Lighting", runExercise5)