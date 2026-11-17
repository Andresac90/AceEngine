// Window, GPU device, input and frame timing. One App per process.
#pragma once
#include "core/base.h"
#include "core/math.h"
#include "rhi/rhi.h"
#include <SDL3/SDL.h>

namespace ace {

// Per-frame input snapshot. Keys are addressed by SDL key names ("W", "Space",
// "Left", "Escape"...), which is also what Lua scripts use. Touches arrive as
// mouse events, so the same game code runs on phones.
struct Input {
  bool down[SDL_SCANCODE_COUNT] = {};
  bool pressed[SDL_SCANCODE_COUNT] = {}; // went down this frame
  vec2 mouse{0.0f};                      // window points (same units as windowSize)
  vec2 windowSize{1.0f};
  vec2 mouseDelta{0.0f};
  f32 wheel = 0.0f;
  bool mouseDown[4] = {};
  bool mousePressed[4] = {};
  bool keyDown(const char* name) const;
  bool keyPressed(const char* name) const;
};

struct AppDesc {
  std::string title = "AceEngine";
  int width = 1280, height = 720;
  rhi::Backend backend = rhi::defaultBackend();
  bool validation = false;
  bool resizable = true;
};

class App {
public:
  bool init(const AppDesc& desc);
  ~App();
  // Processes OS events; returns false once the app should quit. `hook` sees
  // every event first (the editor passes Dear ImGui's handler).
  bool pump(const std::function<void(const SDL_Event&)>& hook = {});

  SDL_Window* window = nullptr;
  std::unique_ptr<rhi::Device> device;
  Input input;
  f32 dt = 1.0f / 60.0f; // seconds, clamped to avoid huge steps after a stall
  f64 time = 0.0;

private:
  u64 lastCounter_ = 0;
};

// Parses "--backend metal|vulkan|d3d12" style values. Returns false if unknown or not compiled in.
bool parseBackend(const std::string& name, rhi::Backend& out);

} // namespace ace
