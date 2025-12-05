#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <cmath>
#include "exercises/exercise10.h"
#include "graphics/shader.h"
#include "math/mat4.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

void runExercise10(GLFWwindow* window) {
    gl_log("Running Exercise 10 - Spotlights and Directional Lights\n");
    
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

    GLfloat points[] = {
         0.0f,  0.5f,  0.0f,
        -0.5f, -0.5f,  0.0f,
         0.5f, -0.5f,  0.0f
    };

    GLfloat normals[] = {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f
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
    if (!shader.loadFromFiles("shaders/exercises/exercise10/vertex.glsl", 
                               "shaders/exercises/exercise10/fragment.glsl")) {
        std::cerr << "Failed to load shader" << std::endl;
        return;
    }

    shader.use();
    
    int model_loc = glGetUniformLocation(shader.programme, "model");
    int view_loc = glGetUniformLocation(shader.programme, "view");
    int proj_loc = glGetUniformLocation(shader.programme, "proj");
    int light_pos_loc = glGetUniformLocation(shader.programme, "light_position_eye");
    int light_dir_loc = glGetUniformLocation(shader.programme, "light_direction_eye");
    int cone_angle_loc = glGetUniformLocation(shader.programme, "spotlight_cone_angle");
    int attenuation_loc = glGetUniformLocation(shader.programme, "spotlight_attenuation");
    int light_type_loc = glGetUniformLocation(shader.programme, "light_type");
    int spec_exp_loc = glGetUniformLocation(shader.programme, "specular_exponent");

    glClearColor(0.05f, 0.05f, 0.05f, 1.0f);

    vec3 cam_pos(0.0f, 0.0f, 3.0f);
    mat4 view_mat = translate(vec3(-cam_pos.v[0], -cam_pos.v[1], -cam_pos.v[2]));
    
    float aspect = (float)g_fb_width / (float)g_fb_height;
    mat4 proj_mat = perspective(67.0f, aspect, 0.1f, 100.0f);

    vec3 light_pos_world(1.0f, 0.0f, 1.0f);
    vec3 light_dir_world(-0.5f, 0.0f, -1.0f);
    
    glUniformMatrix4fv(view_loc, 1, GL_FALSE, view_mat.m);
    glUniformMatrix4fv(proj_loc, 1, GL_FALSE, proj_mat.m);
    glUniform1f(spec_exp_loc, 50.0f);

    std::cout << "\n=== Exercise 10 - Spotlights and Directional Lights ===" << std::endl;
    std::cout << "Understanding different light types!" << std::endl;
    std::cout << "\nLight Types:" << std::endl;
    std::cout << "  1 - POINT LIGHT (omnidirectional)" << std::endl;
    std::cout << "  2 - DIRECTIONAL LIGHT (like the sun)" << std::endl;
    std::cout << "  3 - SPOTLIGHT (cone of light)" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  WASD - Move triangle" << std::endl;
    std::cout << "  ARROW KEYS - Move light/change direction" << std::endl;
    std::cout << "  1/2/3 - Switch light type" << std::endl;
    std::cout << "  +/- - Adjust spotlight cone angle" << std::endl;
    std::cout << "  T - Toggle spotlight attenuation" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    vec3 triangle_pos(0.0f, 0.0f, 0.0f);
    
    int light_type = 2;
    float spotlight_cone_angle = 0.785f;
    float spotlight_attenuation = 1.0f;
    
    const char* light_type_names[] = {"POINT", "DIRECTIONAL", "SPOTLIGHT"};

    std::cout << "\nCurrent light type: " << light_type_names[light_type] << std::endl;
    std::cout << "Spotlight cone angle: " << (spotlight_cone_angle * 180.0f / M_PI) << " degrees" << std::endl;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        float move_speed = 1.0f * elapsed;
        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) triangle_pos.v[1] += move_speed;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) triangle_pos.v[1] -= move_speed;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) triangle_pos.v[0] -= move_speed;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) triangle_pos.v[0] += move_speed;

        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) light_pos_world.v[1] += move_speed;
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) light_pos_world.v[1] -= move_speed;
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) light_pos_world.v[0] -= move_speed;
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) light_pos_world.v[0] += move_speed;

        static bool key1_was_pressed = false;
        bool key1_is_pressed = glfwGetKey(window, GLFW_KEY_1) == GLFW_PRESS;
        if (key1_is_pressed && !key1_was_pressed) {
            light_type = 0;
            std::cout << "\nLight type: " << light_type_names[light_type] << std::endl;
        }
        key1_was_pressed = key1_is_pressed;

        static bool key2_was_pressed = false;
        bool key2_is_pressed = glfwGetKey(window, GLFW_KEY_2) == GLFW_PRESS;
        if (key2_is_pressed && !key2_was_pressed) {
            light_type = 1;
            std::cout << "\nLight type: " << light_type_names[light_type] << std::endl;
        }
        key2_was_pressed = key2_is_pressed;

        static bool key3_was_pressed = false;
        bool key3_is_pressed = glfwGetKey(window, GLFW_KEY_3) == GLFW_PRESS;
        if (key3_is_pressed && !key3_was_pressed) {
            light_type = 2;
            std::cout << "\nLight type: " << light_type_names[light_type] << std::endl;
        }
        key3_was_pressed = key3_is_pressed;

        static bool plus_was_pressed = false;
        bool plus_is_pressed = glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS;
        if (plus_is_pressed && !plus_was_pressed) {
            spotlight_cone_angle += 0.1f;
            if (spotlight_cone_angle > 1.57f) spotlight_cone_angle = 1.57f;
            std::cout << "Cone angle: " << (spotlight_cone_angle * 180.0f / M_PI) << " degrees" << std::endl;
        }
        plus_was_pressed = plus_is_pressed;

        static bool minus_was_pressed = false;
        bool minus_is_pressed = glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS;
        if (minus_is_pressed && !minus_was_pressed) {
            spotlight_cone_angle -= 0.1f;
            if (spotlight_cone_angle < 0.1f) spotlight_cone_angle = 0.1f;
            std::cout << "Cone angle: " << (spotlight_cone_angle * 180.0f / M_PI) << " degrees" << std::endl;
        }
        minus_was_pressed = minus_is_pressed;

        static bool t_was_pressed = false;
        bool t_is_pressed = glfwGetKey(window, GLFW_KEY_T) == GLFW_PRESS;
        if (t_is_pressed && !t_was_pressed) {
            spotlight_attenuation = (spotlight_attenuation > 0.5f) ? 0.0f : 1.0f;
            std::cout << "Attenuation: " << (spotlight_attenuation > 0.5f ? "SMOOTH" : "HARD EDGE") << std::endl;
        }
        t_was_pressed = t_is_pressed;

        updateInput(window);

        float lpx = view_mat.m[0]*light_pos_world.v[0] + view_mat.m[4]*light_pos_world.v[1] + view_mat.m[8]*light_pos_world.v[2] + view_mat.m[12];
        float lpy = view_mat.m[1]*light_pos_world.v[0] + view_mat.m[5]*light_pos_world.v[1] + view_mat.m[9]*light_pos_world.v[2] + view_mat.m[13];
        float lpz = view_mat.m[2]*light_pos_world.v[0] + view_mat.m[6]*light_pos_world.v[1] + view_mat.m[10]*light_pos_world.v[2] + view_mat.m[14];
        vec3 light_pos_eye(lpx, lpy, lpz);
        
        float ldx = view_mat.m[0]*light_dir_world.v[0] + view_mat.m[4]*light_dir_world.v[1] + view_mat.m[8]*light_dir_world.v[2];
        float ldy = view_mat.m[1]*light_dir_world.v[0] + view_mat.m[5]*light_dir_world.v[1] + view_mat.m[9]*light_dir_world.v[2];
        float ldz = view_mat.m[2]*light_dir_world.v[0] + view_mat.m[6]*light_dir_world.v[1] + view_mat.m[10]*light_dir_world.v[2];
        vec3 light_dir_eye(ldx, ldy, ldz);

        mat4 model_mat = translate(triangle_pos);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        shader.use();
        glUniformMatrix4fv(model_loc, 1, GL_FALSE, model_mat.m);
        glUniform3fv(light_pos_loc, 1, light_pos_eye.v);
        glUniform3fv(light_dir_loc, 1, light_dir_eye.v);
        glUniform1f(cone_angle_loc, spotlight_cone_angle);
        glUniform1f(attenuation_loc, spotlight_attenuation);
        glUniform1i(light_type_loc, light_type);

        glBindVertexArray(vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    glDeleteVertexArrays(1, &vao);
    glDeleteBuffers(1, &points_vbo);
    glDeleteBuffers(1, &normals_vbo);

    gl_log("Exercise 10 completed\n");
}

REGISTER_EXERCISE(10, "Spotlights & Directional Lights", runExercise10)