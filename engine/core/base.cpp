#include "core/base.h"
#include <SDL3/SDL.h>
#include <cstdarg>
#include <fstream>

namespace ace {

static std::function<void(LogLevel, const std::string&)> g_sink;

void setLogSink(std::function<void(LogLevel, const std::string&)> sink) { g_sink = std::move(sink); }

void log(LogLevel level, const char* fmt, ...) {
  char buf[2048];
  va_list args;
  va_start(args, fmt);
  vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  if (!buf[0]) return;
  static const char* names[] = {"info", "warn", "error"};
  fprintf(level == LogLevel::Info ? stdout : stderr, "[%s] %s\n", names[(int)level], buf);
  fflush(level == LogLevel::Info ? stdout : stderr);
  if (g_sink) g_sink(level, buf);
}

// SDL's IO is used so the same code reads from an Android APK or an iOS bundle.
bool readFile(const std::string& path, std::vector<u8>& out) {
  out.clear();
  size_t size = 0;
  void* data = SDL_LoadFile(path.c_str(), &size);
  if (!data) return false;
  out.assign((u8*)data, (u8*)data + size);
  SDL_free(data);
  return true;
}

bool readTextFile(const std::string& path, std::string& out) {
  std::vector<u8> bytes;
  if (!readFile(path, bytes)) return false;
  out.assign(bytes.begin(), bytes.end());
  return true;
}

bool writeTextFile(const std::string& path, std::string_view text) {
  std::ofstream f(path, std::ios::binary | std::ios::trunc);
  if (!f) return false;
  f.write(text.data(), (std::streamsize)text.size());
  return f.good();
}

std::string engineDataDir() {
  const char* base = SDL_GetBasePath(); // ends with a path separator
  return std::string(base ? base : "./") + "ace_data/";
}

} // namespace ace
