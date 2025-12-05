#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <cstdlib>
#include <ctime>
#include "exercises/exercise19.h"
#include "graphics/shader.h"
#include "graphics/texture.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

#define PARTICLE_COUNT 500

// Generate particle attributes (initial velocity and start time)
GLuint generate_particles() {
    std::vector<float> velocities(PARTICLE_COUNT * 3);
    std::vector<float> start_times(PARTICLE_COUNT);
    
    float t_accum = 0.0f;
    
    // Seed random number generator
    srand(static_cast<unsigned int>(time(nullptr)));
    
    for (int i = 0; i < PARTICLE_COUNT; i++) {
        // Stagger start times
        start_times[i] = t_accum;
        t_accum += 0.006f;  // 0.006 seconds between particles
        
        // Random initial velocity (fountain spray pattern)
        float randx = ((float)rand() / (float)RAND_MAX) * 1.0f - 0.5f;
        float randz = ((float)rand() / (float)RAND_MAX) * 1.0f - 0.5f;
        
        velocities[i * 3 + 0] = randx;      // x velocity
        velocities[i * 3 + 1] = 1.0f;       // y velocity (upward)
        velocities[i * 3 + 2] = randz;      // z velocity
    }
    
    // Create velocity VBO
    GLuint velocity_vbo;
    glGenBuffers(1, &velocity_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, velocity_vbo);
    glBufferData(GL_ARRAY_BUFFER, velocities.size() * sizeof(float), 
                 velocities.data(), GL_STATIC_DRAW);
    
    // Create start time VBO
    GLuint time_vbo;
    glGenBuffers(1, &time_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, time_vbo);
    glBufferData(GL_ARRAY_BUFFER, start_times.size() * sizeof(float), 
                 start_times.data(), GL_STATIC_DRAW);
    
    // Create VAO
    GLuint vao;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);
    
    // Bind velocity attribute (location 0, vec3)
    glBindBuffer(GL_ARRAY_BUFFER, velocity_vbo);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(0);
    
    // Bind start time attribute (location 1, float)
    glBindBuffer(GL_ARRAY_BUFFER, time_vbo);
    glVertexAttribPointer(1, 1, GL_FLOAT, GL_FALSE, 0, nullptr);
    glEnableVertexAttribArray(1);
    
    gl_log("Generated %d particles\n", PARTICLE_COUNT);
    
    return vao;
}

void runExercise19(GLFWwindow* window) {
    gl_log("Running Exercise 19 - Particle Systems\n");
    
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
    glEnable(GL_PROGRAM_POINT_SIZE);  // Allow shader to set point size

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Generate particle system
    GLuint particle_vao = generate_particles();

    // Load optional particle texture
    Texture particle_tex;
    bool has_texture = particle_tex.loadFromFile("assets/textures/lava-and-rock_albedo.png");
    
    // Load shaders
    Shader particle_shader;
    if (!particle_shader.loadFromFiles(
        "shaders/exercises/exercise19/vertex.glsl",
        "shaders/exercises/exercise19/fragment.glsl"
    )) {
        std::cerr << "Failed to load particle shaders" << std::endl;
        return;
    }

    // Get uniform locations
    int view_loc = glGetUniformLocation(particle_shader.programme, "view");
    int proj_loc = glGetUniformLocation(particle_shader.programme, "proj");
    int emitter_pos_loc = glGetUniformLocation(particle_shader.programme, "emitter_pos");
    int elapsed_time_loc = glGetUniformLocation(particle_shader.programme, "elapsed_system_time");
    int particle_tex_loc = glGetUniformLocation(particle_shader.programme, "particle_tex");
    int use_texture_loc = glGetUniformLocation(particle_shader.programme, "use_texture");
    int particle_color_loc = glGetUniformLocation(particle_shader.programme, "particle_color");

    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);

    // Camera setup
    vec3 cam_pos(0.0f, 0.5f, 3.0f);
    vec3 cam_target(0.0f, 0.5f, 0.0f);
    vec3 cam_up(0.0f, 1.0f, 0.0f);
    
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);
    mat4 view_mat = look_at(cam_pos, cam_target, cam_up);

    // Emitter position
    vec3 emitter_position(0.0f, 0.0f, 0.0f);
    
    // Particle color (blue-ish)
    float particle_color[4] = {0.5f, 0.7f, 1.0f, 0.8f};

    // Particle system timer
    double start_time = glfwGetTime();

    std::cout << "\n=== Exercise 19 - Particle Systems ===" << std::endl;
    std::cout << "Fountain particle emitter with " << PARTICLE_COUNT << " particles!" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  ARROW KEYS - Move emitter" << std::endl;
    std::cout << "  UP/DOWN (while holding SHIFT) - Move emitter up/down" << std::endl;
    std::cout << "  R - Reset emitter position" << std::endl;
    std::cout << "  1/2/3 - Change particle color" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Calculate elapsed system time
        float system_time = (float)(curr_time - start_time);

        // Move emitter
        float move_speed = 1.0f * elapsed;
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) 
            emitter_position.v[0] -= move_speed;
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) 
            emitter_position.v[0] += move_speed;
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS && 
            (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || 
             glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS))
            emitter_position.v[1] -= move_speed;
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS && 
            (glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || 
             glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS))
            emitter_position.v[1] += move_speed;
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS && 
            glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) != GLFW_PRESS && 
            glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) != GLFW_PRESS)
            emitter_position.v[2] -= move_speed;
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS && 
            glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) != GLFW_PRESS && 
            glfwGetKey(window, GLFW_KEY_RIGHT_SHIFT) != GLFW_PRESS)
            emitter_position.v[2] += move_speed;

        // Reset emitter
        static bool r_was_pressed = false;
        bool r_is_pressed = glfwGetKey(window, GLFW_KEY_R) == GLFW_PRESS;
        if (r_is_pressed && !r_was_pressed) {
            emitter_position = vec3(0.0f, 0.0f, 0.0f);
            std::cout << "Emitter reset to origin" << std::endl;
        }
        r_was_pressed = r_is_pressed;

        // Change particle color
        static bool key1_was_pressed = false;
        bool key1_is_pressed = glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS;
        if (key1_is_pressed && !key1_was_pressed) {
            particle_color[0] = 0.5f; particle_color[1] = 0.7f; 
            particle_color[2] = 1.0f; // Blue
            std::cout << "Particle color: Blue" << std::endl;
        }
        key1_was_pressed = key1_is_pressed;

        static bool key2_was_pressed = false;
        bool key2_is_pressed = glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS;
        if (key2_is_pressed && !key2_was_pressed) {
            particle_color[0] = 1.0f; particle_color[1] = 0.5f; 
            particle_color[2] = 0.2f; // Orange
            std::cout << "Particle color: Orange" << std::endl;
        }
        key2_was_pressed = key2_is_pressed;

        static bool key3_was_pressed = false;
        bool key3_is_pressed = glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS;
        if (key3_is_pressed && !key3_was_pressed) {
            particle_color[0] = 0.2f; particle_color[1] = 1.0f; 
            particle_color[2] = 0.5f; // Green
            std::cout << "Particle color: Green" << std::endl;
        }
        key3_was_pressed = key3_is_pressed;

        updateInput(window);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        // Render particles
        particle_shader.use();
        
        glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);
        glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);
        glUniform3f(emitter_pos_loc, emitter_position.v[0], 
                   emitter_position.v[1], emitter_position.v[2]);
        glUniform1f(elapsed_time_loc, system_time);
        glUniform4f(particle_color_loc, particle_color[0], particle_color[1], 
                   particle_color[2], particle_color[3]);
        
        if (has_texture) {
            glActiveTexture(GL_TEXTURE0);
            particle_tex.bind();
            glUniform1i(particle_tex_loc, 0);
            glUniform1i(use_texture_loc, 1);
        } else {
            glUniform1i(use_texture_loc, 0);
        }

        glBindVertexArray(particle_vao);
        glDrawArrays(GL_POINTS, 0, PARTICLE_COUNT);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDisable(GL_PROGRAM_POINT_SIZE);
    glDeleteVertexArrays(1, &particle_vao);

    gl_log("Exercise 19 completed\n");
}

REGISTER_EXERCISE(19, "Particle Systems", runExercise19)