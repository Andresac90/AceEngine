#ifndef CUBEMAP_H
#define CUBEMAP_H

#include <glad/glad.h>
#include <string>

class CubeMap {
public:
    CubeMap();
    ~CubeMap();
    
    bool loadFromFiles(
        const char* front,
        const char* back,
        const char* top,
        const char* bottom,
        const char* left,
        const char* right
    );
    
    void bind() const;
    GLuint getID() const { return texture_id; }
    
private:
    bool loadSide(GLenum side_target, const char* filename);
    
    GLuint texture_id;
};

#endif