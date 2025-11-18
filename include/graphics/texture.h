#ifndef TEXTURE_H
#define TEXTURE_H

#include <glad/glad.h>
#include <string>

class Texture {
public:
    GLuint id;
    int width;
    int height;
    int channels;
    
    Texture();
    ~Texture();
    
    bool loadFromFile(const char* filename, bool flip_vertically = true);
    void bind(GLuint texture_unit = 0);
    void unbind();
    
private:
    bool loaded;
};

#endif