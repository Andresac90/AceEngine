#include "utils/screenshot.h"
#include <iostream>
#include <ctime>
#include <cstdlib>
#include <cstdio>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

bool take_screenshot(int fb_width, int fb_height, const char* custom_name) {
    // Allocate memory for RGB image (3 bytes per pixel)
    unsigned char* buffer = (unsigned char*)malloc(fb_width * fb_height * 3);
    if (!buffer) {
        std::cerr << "ERROR: Could not allocate memory for screenshot" << std::endl;
        return false;
    }
    
    // Read pixels from framebuffer
    glReadPixels(0, 0, fb_width, fb_height, GL_RGB, GL_UNSIGNED_BYTE, buffer);
    
    // Generate filename with timestamp
    char name[1024];
    if (custom_name != nullptr) {
        snprintf(name, sizeof(name), "%s.png", custom_name);
    } else {
        long int t = time(NULL);
        snprintf(name, sizeof(name), "screenshot_%ld.png", t);
    }
    
    std::cout << "Taking screenshot: " << name << std::endl;
    
    // stb_image_write expects image with origin at top-left,
    // but OpenGL framebuffer has origin at bottom-left.
    // We can flip it by pointing to the last row and using negative stride.
    unsigned char* last_row = buffer + (fb_width * 3 * (fb_height - 1));
    
    // Write PNG with automatic vertical flip
    // Parameters: filename, width, height, components, data, stride_in_bytes
    if (!stbi_write_png(name, fb_width, fb_height, 3, last_row, -3 * fb_width)) {
        std::cerr << "ERROR: Could not write screenshot file: " << name << std::endl;
        free(buffer);
        return false;
    }
    
    std::cout << "Screenshot saved: " << name 
              << " (" << fb_width << "x" << fb_height << ")" << std::endl;
    
    free(buffer);
    return true;
}