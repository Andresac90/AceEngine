#include "graphics/texture.h"
#include <iostream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

Texture::Texture() : id(0), width(0), height(0), channels(0), loaded(false) {
}

Texture::~Texture() {
    if (loaded && id != 0) {
        glDeleteTextures(1, &id);
    }
}

bool Texture::loadFromFile(const char* filename, bool flip_vertically) {
    stbi_set_flip_vertically_on_load(flip_vertically);
    
    int force_channels = 4;
    unsigned char* image_data = stbi_load(filename, &width, &height, &channels, force_channels);
    
    if (!image_data) {
        std::cerr << "ERROR: Could not load texture: " << filename << std::endl;
        return false;
    }
    
    std::cout << "Loaded texture: " << filename << std::endl;
    std::cout << "  Size: " << width << "x" << height << std::endl;
    std::cout << "  Channels: " << channels << " (forced to 4)" << std::endl;
    
    glGenTextures(1, &id);
    glBindTexture(GL_TEXTURE_2D, id);
    
    glTexImage2D(
        GL_TEXTURE_2D,
        0,
        GL_RGBA,        
        width,
        height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        image_data
    );
    
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);  
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);  
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    
    stbi_image_free(image_data);
    
    loaded = true;
    return true;
}

void Texture::bind(GLuint texture_unit) {
    glActiveTexture(GL_TEXTURE0 + texture_unit);
    glBindTexture(GL_TEXTURE_2D, id);
}

void Texture::unbind() {
    glBindTexture(GL_TEXTURE_2D, 0);
}