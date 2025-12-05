#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <iostream>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include "exercises/exercise18.h"
#include "graphics/shader.h"
#include "graphics/texture.h"
#include "utils/log.h"
#include "utils/utils.h"
#include "exercises/ExerciseRegistry.h"

// Glyph metadata
struct GlyphMetrics {
    float x_min;      // Left edge in atlas (0-1)
    float width;      // Width proportion
    float y_min;      // Top edge in atlas (0-1)
    float height;     // Height proportion
    float y_offset;   // Baseline offset
};

// Global glyph data (indexed by ASCII code)
GlyphMetrics glyph_metrics[256];

bool load_font_metadata(const char* meta_file) {
    std::ifstream file(meta_file);
    if (!file.is_open()) {
        std::cerr << "ERROR: Could not open metadata file: " << meta_file << std::endl;
        return false;
    }

    std::string line;
    // Skip header line
    std::getline(file, line);

    // Read each glyph's data
    int ascii_code;
    float x_min, width, y_min, height, y_offset;
    
    while (file >> ascii_code >> x_min >> width >> y_min >> height >> y_offset) {
        if (ascii_code >= 0 && ascii_code < 256) {
            glyph_metrics[ascii_code].x_min = x_min;
            glyph_metrics[ascii_code].width = width;
            glyph_metrics[ascii_code].y_min = y_min;
            glyph_metrics[ascii_code].height = height;
            glyph_metrics[ascii_code].y_offset = y_offset;
        }
    }

    file.close();
    gl_log("Loaded font metadata from %s\n", meta_file);
    return true;
}

// Generate vertex data for a text string
void generate_text_mesh(const std::string& text, 
                       std::vector<float>& vertices,
                       std::vector<float>& texcoords) {
    vertices.clear();
    texcoords.clear();

    float cursor_x = 0.0f;  // Current position along text baseline
    const float char_height = 1.0f;  // Base character height

    for (char c : text) {
        unsigned char ascii = (unsigned char)c;
        
        // Skip unprintable characters
        if (ascii < 32 || ascii >= 256) continue;

        const GlyphMetrics& glyph = glyph_metrics[ascii];
        
        // Calculate quad dimensions
        float char_width = glyph.width * char_height;
        float quad_height = glyph.height * char_height;
        float y_off = glyph.y_offset * char_height;

        // Generate two triangles for this character
        // Triangle 1
        vertices.push_back(cursor_x);                    // Bottom-left
        vertices.push_back(y_off);
        
        vertices.push_back(cursor_x + char_width);       // Bottom-right
        vertices.push_back(y_off);
        
        vertices.push_back(cursor_x);                    // Top-left
        vertices.push_back(y_off + quad_height);
        
        // Triangle 2
        vertices.push_back(cursor_x);                    // Top-left
        vertices.push_back(y_off + quad_height);
        
        vertices.push_back(cursor_x + char_width);       // Bottom-right
        vertices.push_back(y_off);
        
        vertices.push_back(cursor_x + char_width);       // Top-right
        vertices.push_back(y_off + quad_height);

        // Texture coordinates for this glyph in atlas
        float tex_left = glyph.x_min;
        float tex_right = glyph.x_min + glyph.width;
        float tex_top = glyph.y_min;
        float tex_bottom = glyph.y_min + glyph.height;

        // Triangle 1 tex coords
        texcoords.push_back(tex_left);   texcoords.push_back(tex_bottom);
        texcoords.push_back(tex_right);  texcoords.push_back(tex_bottom);
        texcoords.push_back(tex_left);   texcoords.push_back(tex_top);
        
        // Triangle 2 tex coords
        texcoords.push_back(tex_left);   texcoords.push_back(tex_top);
        texcoords.push_back(tex_right);  texcoords.push_back(tex_bottom);
        texcoords.push_back(tex_right);  texcoords.push_back(tex_top);

        // Advance cursor for next character
        cursor_x += char_width;
    }
}

void runExercise18(GLFWwindow* window) {
    gl_log("Running Exercise 18 - Text Rendering\n");
    
    glfwMakeContextCurrent(window);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        gl_log_err("Failed to initialize GLAD\n");
        return;
    }

    glViewport(0, 0, g_fb_width, g_fb_height);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    std::cout << "OpenGL version: " << glGetString(GL_VERSION) << std::endl;

    // Load font atlas metadata
    if (!load_font_metadata("assets/fonts/font_atlas.meta")) {
        std::cerr << "Failed to load font metadata!" << std::endl;
        std::cerr << "Please generate font atlas first using the font_atlas_generator tool" << std::endl;
        return;
    }

    Texture font_atlas;
    if (!font_atlas.loadFromFile("assets/fonts/font_atlas.png")) {  // ← Remove the false parameter!
        std::cerr << "Failed to load font atlas texture!" << std::endl;
        return;
    }

    // Force proper texture parameters for text
    font_atlas.bind();
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Load shaders
    Shader text_shader;
    if (!text_shader.loadFromFiles(
        "shaders/exercises/exercise18/vertex.glsl",
        "shaders/exercises/exercise18/fragment.glsl"
    )) {
        std::cerr << "Failed to load text shaders" << std::endl;
        return;
    }

    // Get uniform locations
    int position_loc = glGetUniformLocation(text_shader.programme, "position");
    int scale_loc = glGetUniformLocation(text_shader.programme, "scale");
    int font_atlas_loc = glGetUniformLocation(text_shader.programme, "font_atlas");
    int text_color_loc = glGetUniformLocation(text_shader.programme, "text_color");

    glClearColor(0.1f, 0.1f, 0.15f, 1.0f);

    // Text strings to display
    std::vector<std::string> text_lines = {
        "Hello, OpenGL!",
        "Text Rendering with Font Atlas",
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
        "abcdefghijklmnopqrstuvwxyz",
        "0123456789 !@#$%^&*()",
        "The quick brown fox jumps"
    };

    // Generate meshes for each line
    struct TextMesh {
        GLuint vao, vbo_vertices, vbo_texcoords;
        int vertex_count;
        float y_position;
        float scale;
        float color[4];
    };

    std::vector<TextMesh> text_meshes;
    float y_pos = 0.8f;
    float line_spacing = 0.15f;

    for (size_t i = 0; i < text_lines.size(); i++) {
        std::vector<float> vertices;
        std::vector<float> texcoords;
        generate_text_mesh(text_lines[i], vertices, texcoords);

        TextMesh mesh;
        mesh.vertex_count = vertices.size() / 2;
        mesh.y_position = y_pos - i * line_spacing;
        mesh.scale = 0.05f + (i == 0 ? 0.02f : 0.0f);  // First line bigger
        
        // Different colors for variety
        if (i == 0) {
            mesh.color[0] = 1.0f; mesh.color[1] = 1.0f; 
            mesh.color[2] = 0.2f; mesh.color[3] = 1.0f;  // Yellow
        } else if (i == 1) {
            mesh.color[0] = 0.2f; mesh.color[1] = 1.0f; 
            mesh.color[2] = 1.0f; mesh.color[3] = 1.0f;  // Cyan
        } else {
            mesh.color[0] = 1.0f; mesh.color[1] = 1.0f; 
            mesh.color[2] = 1.0f; mesh.color[3] = 1.0f;  // White
        }

        // Create VAO and VBOs
        glGenVertexArrays(1, &mesh.vao);
        glBindVertexArray(mesh.vao);

        // Vertex positions
        glGenBuffers(1, &mesh.vbo_vertices);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_vertices);
        glBufferData(GL_ARRAY_BUFFER, vertices.size() * sizeof(float), 
                     vertices.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(0);

        // Texture coordinates
        glGenBuffers(1, &mesh.vbo_texcoords);
        glBindBuffer(GL_ARRAY_BUFFER, mesh.vbo_texcoords);
        glBufferData(GL_ARRAY_BUFFER, texcoords.size() * sizeof(float), 
                     texcoords.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
        glEnableVertexAttribArray(1);

        text_meshes.push_back(mesh);
    }

    std::cout << "\n=== Exercise 18 - Text Rendering ===" << std::endl;
    std::cout << "Rendering text using a bitmap font atlas!" << std::endl;
    std::cout << "\nDisplaying " << text_lines.size() << " lines of text" << std::endl;
    std::cout << "\nControls:" << std::endl;
    std::cout << "  +/- - Scale text" << std::endl;
    std::cout << "  ARROW KEYS - Move text" << std::endl;
    std::cout << "  P - Take screenshot" << std::endl;
    std::cout << "  ESC - Exit" << std::endl;

    float global_scale = 1.0f;
    float x_offset = -0.9f;
    float y_offset = 0.0f;

    while (!glfwWindowShouldClose(window)) {
        static double prev_time = glfwGetTime();
        double curr_time = glfwGetTime();
        double elapsed = curr_time - prev_time;
        prev_time = curr_time;

        update_fps_counter(window);

        // Scale control
        if (glfwGetKey(window, GLFW_KEY_EQUAL) == GLFW_PRESS) {
            global_scale += 0.5f * elapsed;
            if (global_scale > 3.0f) global_scale = 3.0f;
        }
        if (glfwGetKey(window, GLFW_KEY_MINUS) == GLFW_PRESS) {
            global_scale -= 0.5f * elapsed;
            if (global_scale < 0.2f) global_scale = 0.2f;
        }

        // Position control
        float move_speed = 0.5f * elapsed;
        if (glfwGetKey(window, GLFW_KEY_LEFT) == GLFW_PRESS) x_offset -= move_speed;
        if (glfwGetKey(window, GLFW_KEY_RIGHT) == GLFW_PRESS) x_offset += move_speed;
        if (glfwGetKey(window, GLFW_KEY_UP) == GLFW_PRESS) y_offset += move_speed;
        if (glfwGetKey(window, GLFW_KEY_DOWN) == GLFW_PRESS) y_offset -= move_speed;

        updateInput(window);

        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        glViewport(0, 0, g_fb_width, g_fb_height);

        // Render all text lines
        text_shader.use();
        glActiveTexture(GL_TEXTURE0);
        font_atlas.bind();
        glUniform1i(font_atlas_loc, 0);

        for (const auto& mesh : text_meshes) {
            glUniform2f(position_loc, x_offset, mesh.y_position + y_offset);
            glUniform1f(scale_loc, mesh.scale * global_scale);
            glUniform4f(text_color_loc, mesh.color[0], mesh.color[1], 
                       mesh.color[2], mesh.color[3]);

            glBindVertexArray(mesh.vao);
            glDrawArrays(GL_TRIANGLES, 0, mesh.vertex_count);
        }

        glfwSwapBuffers(window);
        glfwPollEvents();
    }

    // Cleanup
    for (auto& mesh : text_meshes) {
        glDeleteVertexArrays(1, &mesh.vao);
        glDeleteBuffers(1, &mesh.vbo_vertices);
        glDeleteBuffers(1, &mesh.vbo_texcoords);
    }

    gl_log("Exercise 18 completed\n");
}

REGISTER_EXERCISE(18, "Text Rendering", runExercise18)