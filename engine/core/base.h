// Core types, logging and assertions shared by every engine module.
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace ace {

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using i8 = int8_t;
using i32 = int32_t;
using i64 = int64_t;
using f32 = float;
using f64 = double;

enum class LogLevel { Info, Warn, Error };

// Printf-style logging. Every line also goes to the sink (the editor console).
#if defined(__GNUC__) || defined(__clang__)
#define ACE_PRINTF_ARGS(fmt, first) __attribute__((format(printf, fmt, first))) // compile-time format checking
#else
#define ACE_PRINTF_ARGS(fmt, first)
#endif
void log(LogLevel level, const char* fmt, ...) ACE_PRINTF_ARGS(2, 3);
void setLogSink(std::function<void(LogLevel, const std::string&)> sink);

#define ACE_INFO(...) ::ace::log(::ace::LogLevel::Info, __VA_ARGS__)
#define ACE_WARN(...) ::ace::log(::ace::LogLevel::Warn, __VA_ARGS__)
#define ACE_ERROR(...) ::ace::log(::ace::LogLevel::Error, __VA_ARGS__)

// Always-on assertion: engine invariants are cheap to check and expensive to debug.
#define ACE_ASSERT(cond, ...)                                              \
  do {                                                                     \
    if (!(cond)) {                                                         \
      ::ace::log(::ace::LogLevel::Error, "Assertion failed: %s (%s:%d)", #cond, __FILE__, __LINE__); \
      ::ace::log(::ace::LogLevel::Error, "" __VA_ARGS__);                  \
      std::abort();                                                        \
    }                                                                      \
  } while (0)

// Whole-file read. Returns false (and leaves `out` empty) if the file is missing.
bool readFile(const std::string& path, std::vector<u8>& out);
bool readTextFile(const std::string& path, std::string& out);
bool writeTextFile(const std::string& path, std::string_view text);

// Directory that holds the engine's compiled shaders (next to the executable).
std::string engineDataDir();

} // namespace ace
