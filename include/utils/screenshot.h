#ifndef SCREENSHOT_H
#define SCREENSHOT_H

#include <glad/glad.h>

// Take a screenshot and save to PNG file
// Returns true on success, false on failure
bool take_screenshot(int fb_width, int fb_height, const char* custom_name = nullptr);

#endif