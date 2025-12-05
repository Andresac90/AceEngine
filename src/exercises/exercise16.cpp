#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise16.h"
#include "graphics/shader.h"
#include "graphics/texture.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

// Full-screen quad in clip space (-1 to 1)
static GLfloat panel_points[] = {
    -1.0f, -1.0f,
     1.0f, -1.0f,
    -1.0f,  1.0f,
    -1.0f,  1.0f,
     1.0f, -1.0f,
     1.0f,  1.0f
};

struct Color {
    float r, g, b, a;
};

struct GUIPanel {
    float width_px;   // Width in pixels
    float height_px;  // Height in pixels
    float x_pos;      // Position in clip space (-1 to 1)
    float y_pos;      // Position in clip space (-1 to 1)
    Color color;      // RGBA color if no texture
    bool visible;
};

void runExercise16(GLFWwindow* window) {
    gl_log("Running Exercise 16 - GUI Panels\n");
    
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

    // Create VAO and VBO for panel quad
    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(panel_points), panel_points, GL_STATIC_DRAW);
    
    // 2D positions (not 3D!)
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    // Load optional texture
    Texture panel_tex;
    bool has_texture = panel_tex.loadFromFile("assets/textures/alley-brick-wall_albedo.png");
    if (!has_texture) {
        std::cout << "No GUI texture, using colored panels" << std::endl;
    }

    // Load GUI panel shaders (NO view/projection matrices!)
    Shader gui_shader;
    if (!gui_shader.loadFromFiles(
        "shaders/exercises/exercise16/vertex.glsl",
        "shaders/exercises/exercise16/fragment.glsl"
    )) {
        std::cerr << "Failed to load GUI shaders" << std::endl;
        return;
    }

    // Load regular 3D scene shaders for background
    Shader scene_shader;
    if (!scene_shader.loadFromFiles(
        "shaders/exercises/exercise16/scene_vertex.glsl",
        "shaders/exercises/exercise16/scene_fragment.glsl"
    )) {
        std::cerr << "Failed to load scene shaders" << std::endl;
        return;
    }

    // Simple 3D triangle for background
    GLfloat scene_points[] = {
         0.0f,  0.5f,  0.0f,
        -0.5f, -0.5f,  0.0f,
         0.5f, -0.5f,  0.0f
    };
    GLfloat scene_colors[] = {
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
        0.0f, 0.0f, 1.0f
    };
    
    GLuint scene_vao, scene_vbo_pos, scene_vbo_col;
    glGenVertexArrays(1, &scene_vao);
    glBindVertexArray(scene_vao);
    
    glGenBuffers(1, &scene_vbo_pos);
    glBindBuffer(GL_ARRAY_BUFFER, scene_vbo_pos);
    glBufferData(GL_ARRAY_BUFFER, sizeof(scene_points), scene_points, GL_STATIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    glGenBuffers(1, &scene_vbo_col);
    glBindBuffer(GL_ARRAY_BUFFER, scene_vbo_col);
    glBufferData(GL_ARRAY_BUFFER, sizeof(scene_colors), scene_colors, GL_STATIC_DRAW);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);

    // Get uniform locations
    int gui_scale_loc = glGetUniformLocation(gui_shader.programme, "gui_scale");
    int gui_pos_loc = glGetUniformLocation(gui_shader.programme, "gui_position");
    int gui_tex_loc = glGetUniformLocation(gui_shader.programme, "gui_texture");
    int gui_has_tex_loc = glGetUniformLocation(gui_shader.programme, "has_texture");
    int gui_color_loc = glGetUniformLocation(gui_shader.programme, "panel_color");

    glClearColor(0.2f, 0.2f, 0.3f, 1.0f);

    // Define GUI panels
    GUIPanel panels[4];
    
    // Panel 1: Top-left corner (red)
    panels[0].width_px = 256.0f;
    panels[0].height_px = 256.0f;
    panels[0].x_pos = -0.7f;
    panels[0].y_pos = 0.5f;
    panels[0].color = {1.0f, 0.2f, 0.2f, 0.8f};
    panels[0].visible = true;
    
    // Panel 2: Top-right corner (green)
    panels[1].width_px = 200.0f;
    panels[1].height_px = 150.0f;
    panels[1].x_pos = 0.6f;
    panels[1].y_pos = 0.6f;
    panels[1].color = {0.2f, 1.0f, 0.2f, 0.8f};
    panels[1].visible = true;
    
    // Panel 3: Bottom center (blue)
    panels[2].width_px = 300.0f;
    panels[2].height_px = 100.0f;
    panels[2].x_pos = 0.0f;
    panels[2].y_pos = -0.7f;
    panels[2].color = {0.2f, 0.5f, 1.0f, 0.8f};
    panels[2].visible = true;
    
    // Panel 4: Center (textured or yellow)
    panels[3].width_px = 256.0f;
    panels[3].height_px = 256.0f;
    panels[3].x_pos = 0.0f;
    panels[3].y_pos = 0.0f;
    panels[3].color = {1.0f, 1.0f, 0.2f, 0.9f};
    panels[3].visible = true;

    float camera_rotation = 0.0f;
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);
    mat4 view_mat = translate(vec3(0.0f, 0.0f, -2.0f));

    std::cout << "\n=== Exercise 16 - GUI Panels ===" << std::endl;
    std::cout << "Rendering 2D UI elements in clip space!" << std::endl;
    std::cout << "\nGUI panels maintain pixel dimensions regardless of resolution." << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  1/2/3/4 - Toggle panel visibility" << std::endl;
    std::cout << "  +/- - Scale Panel 1" << std::endl;
    std::cout << "  ARROW KEYS - Move Panel 1" << std::endl;
    std::cout << "  R - Reset Panel 1 position" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Toggle panel visibility
        static bool key1_was_pressed = false;
        bool key1_is_pressed = glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS;
        if (key1_is_pressed && !key1_was_pressed) {
            panels[0].visible = !panels[0].visible;
            std::cout << "Panel 1: " << (panels[0].visible ? "ON" : "OFF") << std::endl;
        }
        key1_was_pressed = key1_is_pressed;

        static bool key2_was_pressed = false;
        bool key2_is_pressed = glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS;
        if (key2_is_pressed && !key2_was_pressed) {
            panels[1].visible = !panels[1].visible;
            std::cout << "Panel 2: " << (panels[1].visible ? "ON" : "OFF") << std::endl;
        }
        key2_was_pressed = key2_is_pressed;

        static bool key3_was_pressed = false;
        bool key3_is_pressed = glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS;
        if (key3_is_pressed && !key3_was_pressed) {
            panels[2].visible = !panels[2].visible;
            std::cout << "Panel 3: " << (panels[2].visible ? "ON" : "OFF") << std::endl;
        }
        key3_was_pressed = key3_is_pressed;

        static bool key4_was_pressed = false;
        bool key4_is_pressed = glfwGetKey(window, GLFW_KEY_4) == GLFW_PRESS;
        if (key4_is_pressed && !key4_was_pressed) {
            panels[3].visible = !panels[3].visible;
            std::cout << "Panel 4: " << (panels[3].visible ? "ON" : "OFF") << std::endl;
        }
        key4_was_pressed = key4_is_pressed;

        // Scale Panel 1
        if (glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS) {
            panels[0].width_px += 50.0f * elapsed;
            panels[0].height_px += 50.0f * elapsed;
        }
        if (glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS) {
            panels[0].width_px -= 50.0f * elapsed;
            panels[0].height_px -= 50.0f * elapsed;
            if (panels[0].width_px < 50.0f) panels[0].width_px = 50.0f;
            if (panels[0].height_px < 50.0f) panels[0].height_px = 50.0f;
        }

        // Move Panel 1
        float move_speed = 1.0f * elapsed;
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) panels[0].x_pos -= move_speed;
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) panels[0].x_pos += move_speed;
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) panels[0].y_pos += move_speed;
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) panels[0].y_pos -= move_speed;

        // Reset Panel 1
        static bool r_was_pressed = false;
        bool r_is_pressed = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
        if (r_is_pressed && !r_was_pressed) {
            panels[0].x_pos = -0.7f;
            panels[0].y_pos = 0.5f;
            panels[0].width_px = 256.0f;
            panels[0].height_px = 256.0f;
            std::cout << "Panel 1 reset" << std::endl;
        }
        r_was_pressed = r_is_pressed;

        updateInput(window);

        // Rotate background triangle
        camera_rotation += 30.0f * elapsed;
        if (camera_rotation > 360.0f) camera_rotation -= 360.0f;
        mat4 rotation_mat = rotate_z(camera_rotation);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        // 1. Draw 3D scene (background)
        scene_shader.use();
        int scene_proj_loc = glGetUniformLocation(scene_shader.programme, "proj");
        int scene_view_loc = glGetUniformLocation(scene_shader.programme, "view");
        int scene_model_loc = glGetUniformLocation(scene_shader.programme, "model");
        
        glUniformMatrix4fv(scene_proj_loc, 1, GL_FALSE, proj_mat.m);
        glUniformMatrix4fv(scene_view_loc, 1, GL_FALSE, view_mat.m);
        glUniformMatrix4fv(scene_model_loc, 1, GL_FALSE, rotation_mat.m);
        
        glBindVertexArray(scene_vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        // 2. Draw GUI panels (NO depth test for GUI!)
        glDisable(GL_DEPTH_TEST);
        
        gui_shader.use();
        glBindVertexArray(vao);

        for (int i = 0; i < 4; i++) {
            if (!panels[i].visible) continue;

            // Calculate scale factors (pixel size to clip space)
            float x_scale = panels[i].width_px / (float)g_fb_width;
            float y_scale = panels[i].height_px / (float)g_fb_height;

            glUniform2f(gui_scale_loc, x_scale, y_scale);
            glUniform2f(gui_pos_loc, panels[i].x_pos, panels[i].y_pos);
            glUniform4f(gui_color_loc, panels[i].color.r, panels[i].color.g, 
                       panels[i].color.b, panels[i].color.a);

            // Use texture only for center panel if available
            if (has_texture && i == 3) {
                glActiveTexture(GL_TEXTURE0);
                panel_tex.bind();
                glUniform1i(gui_tex_loc, 0);
                glUniform1i(gui_has_tex_loc, 1);
            } else {
                glUniform1i(gui_has_tex_loc, 0);
            }

            glDrawArrays(GL_TRIANGLES, 0, 6);
        }

        glEnable(GL_DEPTH_TEST);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);
    glDeleteVertexArrays(1, &scene_vao);
    glDeleteBuffers(1, &scene_vbo_pos);
    glDeleteBuffers(1, &scene_vbo_col);

    gl_log("Exercise 16 completed\n");
}

REGISTER_EXERCISE(16, "GUI Panels", runExercise16)