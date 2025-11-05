#ifndef LOG_H
#define LOG_H

#include <string>

bool restart_gl_log();
bool gl_log(const char* message, ...);
bool gl_log_err(const char* message, ...);
void log_gl_params();  

#endif