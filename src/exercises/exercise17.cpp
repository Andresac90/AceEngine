#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include "exercises/exercise17.h"
#include "graphics/shader.h"
#include "graphics/texture.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

// Calculate sprite offset in texture atlas
void calculate_sprite_offset(int sprite_index, int num_cols, int num_rows, 
                             float& s_offset, float& t_offset) {
    // Calculate column and row from sprite index
    int col = sprite_index % num_cols;
    int row = sprite_index / num_cols;
    
    // Calculate offset (0,0 = top-left, not bottom-left)
    s_offset = (float)col / (float)num_cols;
    t_offset = (float)row / (float)num_rows;
}

void runExercise17(GLFWwindow* window) {
    gl_log("Running Exercise 17 - Sprite Sheets\n");
    
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

    // Create a simple quad
    GLfloat quad_points[] = {
        -1.0f, -1.0f,  0.0f,
         1.0f, -1.0f,  0.0f,
        -1.0f,  1.0f,  0.0f,
        -1.0f,  1.0f,  0.0f,
         1.0f, -1.0f,  0.0f,
         1.0f,  1.0f,  0.0f
    };

    GLuint vao, vbo;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad_points), quad_points, GL_STATIC_DRAW);
    
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);

    // Load sprite sheet texture
    Texture sprite_sheet;
    if (!sprite_sheet.loadFromFile("assets/textures/geyser-rock1_albedo.png")) {
        std::cerr << "Failed to load sprite sheet!" << std::endl;
        std::cerr << "Create a sprite sheet at assets/textures/spritesheet.png" << std::endl;
        return;
    }

    // Load shaders
    Shader shader;
    if (!shader.loadFromFiles(
        "shaders/exercises/exercise17/vertex.glsl",
        "shaders/exercises/exercise17/fragment.glsl"
    )) {
        std::cerr << "Failed to load shaders" << std::endl;
        return;
    }

    shader.use();
    
    int model_loc = glGetUniformLocation(shader.programme, "model");
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    int st_offset_loc = glGetUniformLocation(shader.programme, "st_offset");
    int sprite_scale_loc = glGetUniformLocation(shader.programme, "sprite_scale");
    int sprite_sheet_loc = glGetUniformLocation(shader.programme, "sprite_sheet");

    glClearColor(0.2f, 0.2f, 0.2f, 1.0f);

    // Camera setup
    vec3 cam_pos(0.0f, 0.0f, 3.0f);
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);
    mat4 view_mat = translate(vec3(-cam_pos.v[0], -cam_pos.v[1], -cam_pos.v[2]));

    // Sprite sheet configuration
    const int NUM_COLS = 4;
    const int NUM_ROWS = 4;
    const int TOTAL_SPRITES = NUM_COLS * NUM_ROWS;
    
    int current_sprite = 0;
    float sprite_s_offset = 0.0f;
    float sprite_t_offset = 0.0f;
    
    // Animation
    bool animate = false;
    float animation_time = 0.0f;
    float animation_speed = 10.0f;  // Frames per second

    std::cout << "\n=== Exercise 17 - Sprite Sheets ===" << std::endl;
    std::cout << "Rendering animated sprites from a texture atlas!" << std::endl;
    std::cout << "\nSprite sheet: " << NUM_COLS << "x" << NUM_ROWS << " grid (" << TOTAL_SPRITES << " sprites)" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  0-9 - Select sprite by number" << std::endl;
    std::cout << "  LEFT/RIGHT - Previous/Next sprite" << std::endl;
    std::cout << "  SPACE - Toggle animation" << std::endl;
    std::cout << "  +/- - Increase/Decrease animation speed" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    std::cout << "\nCurrent sprite: " << current_sprite << std::endl;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Number key sprite selection
        for (int i = 0; i < 10; i++) {
            static bool key_was_pressed[10] = {false};
            bool key_is_pressed = glfwGetKey(window, GLFW_KEY_0 + i) == GLFW_PRESS;
            if (key_is_pressed && !key_was_pressed[i]) {
                current_sprite = i % TOTAL_SPRITES;
                calculate_sprite_offset(current_sprite, NUM_COLS, NUM_ROWS, 
                                      sprite_s_offset, sprite_t_offset);
                std::cout << "Sprite: " << current_sprite << std::endl;
            }
            key_was_pressed[i] = key_is_pressed;
        }

        // Arrow key navigation
        static bool left_was_pressed = false;
        bool left_is_pressed = glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS;
        if (left_is_pressed && !left_was_pressed) {
            current_sprite = (current_sprite - 1 + TOTAL_SPRITES) % TOTAL_SPRITES;
            calculate_sprite_offset(current_sprite, NUM_COLS, NUM_ROWS, 
                                  sprite_s_offset, sprite_t_offset);
            std::cout << "Sprite: " << current_sprite << std::endl;
        }
        left_was_pressed = left_is_pressed;

        static bool right_was_pressed = false;
        bool right_is_pressed = glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS;
        if (right_is_pressed && !right_was_pressed) {
            current_sprite = (current_sprite + 1) % TOTAL_SPRITES;
            calculate_sprite_offset(current_sprite, NUM_COLS, NUM_ROWS, 
                                  sprite_s_offset, sprite_t_offset);
            std::cout << "Sprite: " << current_sprite << std::endl;
        }
        right_was_pressed = right_is_pressed;

        // Toggle animation
        static bool space_was_pressed = false;
        bool space_is_pressed = glfwGetKey(window, GLFW_KEY_SPACE) == GLFW_PRESS;
        if (space_is_pressed && !space_was_pressed) {
            animate = !animate;
            animation_time = 0.0f;
            std::cout << "Animation: " << (animate ? "ON" : "OFF") << std::endl;
        }
        space_was_pressed = space_is_pressed;

        // Animation speed control
        if (glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS) {
            animation_speed += 5.0f * elapsed;
            if (animation_speed > 30.0f) animation_speed = 30.0f;
            std::cout << "Animation speed: " << animation_speed << " fps" << std::endl;
        }
        if (glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS) {
            animation_speed -= 5.0f * elapsed;
            if (animation_speed < 1.0f) animation_speed = 1.0f;
            std::cout << "Animation speed: " << animation_speed << " fps" << std::endl;
        }

        // Update animation
        if (animate) {
            animation_time += elapsed;
            float frame_duration = 1.0f / animation_speed;
            if (animation_time >= frame_duration) {
                animation_time -= frame_duration;
                current_sprite = (current_sprite + 1) % TOTAL_SPRITES;
                calculate_sprite_offset(current_sprite, NUM_COLS, NUM_ROWS, 
                                      sprite_s_offset, sprite_t_offset);
            }
        }

        updateInput(window);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        shader.use();
        
        // Set uniforms
        mat4 model_mat = identity_mat4();
        glUniformMatrix4fv(model_loc, 1, GL_FALSE, model_mat.m);
        glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);
        glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);
        
        // Sprite offset and scale
        glUniform2f(st_offset_loc, sprite_s_offset, sprite_t_offset);
        glUniform2f(sprite_scale_loc, 1.0f / NUM_COLS, 1.0f / NUM_ROWS);
        
        // Bind sprite sheet texture
        glActiveTexture(GL_TEXTURE0);
        sprite_sheet.bind();
        glUniform1i(sprite_sheet_loc, 0);

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 6);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &vbo);

    gl_log("Exercise 17 completed\n");
}

REGISTER_EXERCISE(17, "Sprite Sheets", runExercise17)