#include "platform/app.h"

namespace ace {

static bool lookup(const bool* table, const char* name) {
  SDL_Scancode code = SDL_GetScancodeFromName(name);
  return code != SDL_SCANCODE_UNKNOWN && table[code];
}
bool Input::keyDown(const char* name) const { return lookup(down, name); }
bool Input::keyPressed(const char* name) const { return lookup(pressed, name); }

bool parseBackend(const std::string& name, rhi::Backend& out) {
  if (name == "metal") out = rhi::Backend::Metal;
  else if (name == "vulkan") out = rhi::Backend::Vulkan;
  else if (name == "d3d12") out = rhi::Backend::D3D12;
  else return false;
  return rhi::backendAvailable(out);
}

bool App::init(const AppDesc& desc) {
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
    ACE_ERROR("SDL_Init failed: %s", SDL_GetError());
    return false;
  }
  SDL_WindowFlags flags = SDL_WINDOW_HIGH_PIXEL_DENSITY | (SDL_WindowFlags)rhi::windowFlagsFor(desc.backend);
  if (desc.resizable) flags |= SDL_WINDOW_RESIZABLE;
  if (desc.hidden) flags |= SDL_WINDOW_HIDDEN;
  window = SDL_CreateWindow(desc.title.c_str(), desc.width, desc.height, flags);
  if (!window) {
    ACE_ERROR("SDL_CreateWindow failed: %s", SDL_GetError());
    return false;
  }
  device = rhi::createDevice({desc.backend, window, true, desc.validation});
  if (!device) return false;
  lastCounter_ = SDL_GetPerformanceCounter();
  return true;
}

App::~App() {
  device.reset();
  if (window) SDL_DestroyWindow(window);
  SDL_Quit();
}

bool App::pump(const std::function<void(const SDL_Event&)>& hook) {
  u64 now = SDL_GetPerformanceCounter();
  dt = fixedDt > 0.0f ? fixedDt : glm::clamp((f32)((f64)(now - lastCounter_) / (f64)SDL_GetPerformanceFrequency()), 0.0001f, 0.1f);
  lastCounter_ = now;
  time += dt;

  memset(input.pressed, 0, sizeof(input.pressed));
  memset(input.mousePressed, 0, sizeof(input.mousePressed));
  input.mouseDelta = vec2(0.0f);
  input.wheel = 0.0f;

  int windowW = 1, windowH = 1;
  SDL_GetWindowSize(window, &windowW, &windowH);
  input.windowSize = vec2((f32)std::max(windowW, 1), (f32)std::max(windowH, 1));

  bool running = true;
  SDL_Event e;
  while (SDL_PollEvent(&e)) {
    if (hook) hook(e);
    switch (e.type) {
      case SDL_EVENT_QUIT: running = false; break;
      case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        if (e.window.windowID == SDL_GetWindowID(window)) running = false;
        break;
      case SDL_EVENT_KEY_DOWN:
        if (!e.key.repeat) input.pressed[e.key.scancode] = true;
        input.down[e.key.scancode] = true;
        break;
      case SDL_EVENT_KEY_UP: input.down[e.key.scancode] = false; break;
      case SDL_EVENT_MOUSE_MOTION:
        input.mouse = {e.motion.x, e.motion.y};
        input.mouseDelta += vec2(e.motion.xrel, e.motion.yrel);
        break;
      case SDL_EVENT_MOUSE_BUTTON_DOWN:
      case SDL_EVENT_MOUSE_BUTTON_UP:
        if (e.button.button < 4) {
          input.mouseDown[e.button.button] = e.button.down;
          if (e.button.down) input.mousePressed[e.button.button] = true;
        }
        input.mouse = {e.button.x, e.button.y};
        break;
      case SDL_EVENT_MOUSE_WHEEL: input.wheel += e.wheel.y; break;
      case SDL_EVENT_GAMEPAD_ADDED:
        if (!gamepad_) {
          gamepad_ = SDL_OpenGamepad(e.gdevice.which);
          if (gamepad_) ACE_INFO("gamepad connected: %s", SDL_GetGamepadName(gamepad_));
        }
        break;
      case SDL_EVENT_GAMEPAD_REMOVED:
        if (gamepad_ && SDL_GetGamepadID(gamepad_) == e.gdevice.which) {
          SDL_CloseGamepad(gamepad_);
          gamepad_ = nullptr;
          memset(input.gamepadAxis, 0, sizeof(input.gamepadAxis));
          memset(input.gamepadDown, 0, sizeof(input.gamepadDown));
        }
        break;
      default: break;
    }
  }
  bool lock = input.wantMouseLock && allowMouseLock;
  if (lock != input.mouseLocked && SDL_SetWindowRelativeMouseMode(window, lock)) input.mouseLocked = lock;
  memset(input.gamepadPressed, 0, sizeof(input.gamepadPressed));
  if (gamepad_) {
    for (int a = 0; a < SDL_GAMEPAD_AXIS_COUNT; a++) {
      f32 v = (f32)SDL_GetGamepadAxis(gamepad_, (SDL_GamepadAxis)a) / 32767.0f;
      input.gamepadAxis[a] = fabsf(v) < 0.15f ? 0.0f : glm::clamp(v, -1.0f, 1.0f); // dead zone
    }
    for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; b++) {
      bool down = SDL_GetGamepadButton(gamepad_, (SDL_GamepadButton)b);
      input.gamepadPressed[b] = down && !input.gamepadDown[b];
      input.gamepadDown[b] = down;
    }
  }
  return running;
}

} // namespace ace
