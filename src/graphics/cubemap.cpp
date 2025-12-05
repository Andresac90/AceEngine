#include "graphics/cubemap.h"
#include <iostream>

// REMOVE THIS LINE:
// #define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"  // Just include, don't define IMPLEMENTATION

CubeMap::CubeMap() : texture_id(0) {}

CubeMap::~CubeMap() {
    if (texture_id) {
        glDeleteTextures(1, &texture_id);
    }
}

bool CubeMap::loadFromFiles(
    const char* front,
    const char* back,
    const char* top,
    const char* bottom,
    const char* left,
    const char* right
) {
    glGenTextures(1, &texture_id);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture_id);
    
    // Load each face
    if (!loadSide(GL_TEXTURE_CUBE_MAP_NEGATIVE_Z, front)) return false;
    if (!loadSide(GL_TEXTURE_CUBE_MAP_POSITIVE_Z, back)) return false;
    if (!loadSide(GL_TEXTURE_CUBE_MAP_POSITIVE_Y, top)) return false;
    if (!loadSide(GL_TEXTURE_CUBE_MAP_NEGATIVE_Y, bottom)) return false;
    if (!loadSide(GL_TEXTURE_CUBE_MAP_NEGATIVE_X, left)) return false;
    if (!loadSide(GL_TEXTURE_CUBE_MAP_POSITIVE_X, right)) return false;
    
    // Set texture parameters
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    
    std::cout << "Cube map loaded successfully!" << std::endl;
    return true;
}

bool CubeMap::loadSide(GLenum side_target, const char* filename) {
    int width, height, channels;
    int force_channels = 4;
    unsigned char* data = stbi_load(filename, &width, &height, &channels, force_channels);
    
    if (!data) {
        std::cerr << "ERROR: Could not load cube map side: " << filename << std::endl;
        return false;
    }
    
    // Check power of 2
    if ((width & (width - 1)) != 0 || (height & (height - 1)) != 0) {
        std::cerr << "WARNING: Cube map image " << filename << " is not power-of-2" << std::endl;
    }
    
    glTexImage2D(
        side_target,
        0,
        GL_RGBA,
        width,
        height,
        0,
        GL_RGBA,
        GL_UNSIGNED_BYTE,
        data
    );
    
    stbi_image_free(data);
    
    std::cout << "Loaded cube map side: " << filename << " (" << width << "x" << height << ")" << std::endl;
    return true;
}

void CubeMap::bind() const {
    glBindTexture(GL_TEXTURE_CUBE_MAP, texture_id);
}