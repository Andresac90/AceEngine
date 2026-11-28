// Game player: loads a project and runs its start scene. This is the executable
// that ships with an exported game; the editor is not involved.
//
//   ace_player [project_dir] [--backend metal|vulkan|d3d12] [--scene file]
//              [--screenshot out.png] [--frames N] [--validation]
//              [--hold <key>]...   keep a key pressed (scripted play tests)
//              [--fixed-dt <seconds>]   every frame advances exactly this much
//              [--frames-to <prefix>]   save every frame as <prefix>0000.png, ... (offscreen)
//              [--timing <first>]   print median / 90th percentile frame time of the frames
//                                   from <first> on (one frame in flight: max of CPU and GPU)
//
// Without a project_dir it runs the "project" folder next to the executable.
#include "platform/app.h"
#include "render/renderer.h"
#include "runtime/project.h"
#include "script/script.h"
#include <SDL3/SDL_main.h>
#include <algorithm>

using namespace ace;

int main(int argc, char** argv) {
  std::string projectDir, sceneOverride, screenshot, framesPrefix;
  f32 fixedDt = 0.0f;
  int frames = 0, timingFrom = -1;
  std::vector<SDL_Scancode> held;
  AppDesc desc;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--backend") {
      std::string name = next();
      if (!parseBackend(name, desc.backend)) {
        ACE_ERROR("backend '%s' is unknown or not available in this build", name.c_str());
        return 2;
      }
    } else if (a == "--scene") sceneOverride = next();
    else if (a == "--screenshot") screenshot = next();
    else if (a == "--frames") frames = atoi(next().c_str());
    else if (a == "--validation") desc.validation = true;
    else if (a == "--fixed-dt") fixedDt = (f32)atof(next().c_str());
    else if (a == "--frames-to") framesPrefix = next();
    else if (a == "--timing") timingFrom = atoi(next().c_str());
    else if (a == "--hold") {
      std::string name = next();
      SDL_Scancode code = SDL_GetScancodeFromName(name.c_str());
      if (code == SDL_SCANCODE_UNKNOWN) {
        ACE_ERROR("unknown key '%s' (use SDL key names such as W, Space, Left)", name.c_str());
        return 2;
      }
      held.push_back(code);
    }
    else if (a[0] != '-') projectDir = a;
    else {
      ACE_ERROR("unknown option %s", a.c_str());
      return 2;
    }
  }
  if (!screenshot.empty() && frames <= 0) frames = 8;
  if (!framesPrefix.empty() && screenshot.empty()) screenshot = framesPrefix + "last.png";
  bool offscreen = !screenshot.empty();

  Project project;
  // The project is looked up before SDL starts, so resolve the base path by hand.
  if (projectDir.empty()) projectDir = std::string(SDL_GetBasePath() ? SDL_GetBasePath() : "./") + "project";
  if (!project.load(projectDir)) return 1;
  desc.title = project.name;
  desc.width = project.width;
  desc.height = project.height;
  desc.hidden = offscreen;

  App app;
  if (!app.init(desc)) return 1;
  app.fixedDt = fixedDt;
  rhi::Device& device = *app.device;
  ACE_INFO("%s running on %s", project.name.c_str(), rhi::backendName(device.backend()));
  {
    Renderer renderer(device);
    Assets assets(device, project.dir);
    Scene scene;
    if (!scene.load(project.path(sceneOverride.empty() ? project.startScene : sceneOverride))) return 1;
    ScriptSystem scripts(scene, assets, app.input);

    rhi::TextureHandle shot; // offscreen target used only in --screenshot mode
    u32 shotW = 0, shotH = 0;
    int frame = 0;
    std::vector<f64> frameMs;
    u64 lastTick = 0;
    while (app.pump()) {
      if (timingFrom >= 0) { // time from one loop start to the next
        u64 now = SDL_GetTicksNS();
        if (lastTick && frame > timingFrom) frameMs.push_back((f64)(now - lastTick) / 1e6);
        lastTick = now;
      }
      for (SDL_Scancode code : held) app.input.down[code] = true;
      scripts.update(app.dt);
      if (!scripts.pendingScene.empty()) {
        std::string next = std::exchange(scripts.pendingScene, std::string());
        if (scene.load(project.path(next))) scripts.restart(); // on failure the current level keeps running
      }
      scene.updateTransforms();

      rhi::CommandList* cmd = device.beginFrame(!offscreen);
      if (!cmd) continue;
      renderer.beginFrame(app.time);
      u32 w = device.backbufferWidth(), h = device.backbufferHeight();
      CameraView view;
      if (!Renderer::findCamera(scene, view)) {
        view.world = glm::inverse(glm::lookAt(vec3(8, 8, 8), vec3(0), vec3(0, 1, 0)));
      }
      if (!screenshot.empty()) {
        if (!shot) {
          shotW = w;
          shotH = h;
          shot = device.createTexture({w, h, rhi::Format::RGBA8_SRGB, rhi::TextureUsage_RenderTarget | rhi::TextureUsage_Sampled, "screenshot"});
        }
        renderer.render(*cmd, scene, assets, view, shot, rhi::Format::RGBA8_SRGB, shotW, shotH, scripts.texts);
      } else {
        renderer.render(*cmd, scene, assets, view, device.backbuffer(), device.backbufferFormat(), w, h, scripts.texts);
      }
      device.endFrame();
      scripts.gameSize = renderer.gameSize();
      if (!framesPrefix.empty() && shot) { // every frame, for stability measurements
        std::vector<u8> pixels;
        char name[32];
        snprintf(name, sizeof(name), "%04d.png", frame);
        device.waitIdle();
        if (!device.readTexture(shot, pixels) || !savePng(framesPrefix + name, shotW, shotH, pixels.data())) {
          ACE_ERROR("could not write %s%s", framesPrefix.c_str(), name);
          return 1;
        }
      }

      if (frames > 0 && ++frame >= frames) break;
    }
    if (!frameMs.empty()) {
      std::sort(frameMs.begin(), frameMs.end());
      ACE_INFO("timing: %zu frames, median %.2f ms, 90th percentile %.2f ms", frameMs.size(), frameMs[frameMs.size() / 2],
               frameMs[std::min(frameMs.size() - 1, frameMs.size() * 9 / 10)]);
    }
    if (!screenshot.empty() && shot) {
      device.waitIdle();
      std::vector<u8> pixels;
      if (device.readTexture(shot, pixels) && savePng(screenshot, shotW, shotH, pixels.data())) {
        ACE_INFO("wrote %s (%ux%u)", screenshot.c_str(), shotW, shotH);
      } else {
        ACE_ERROR("could not write screenshot %s", screenshot.c_str());
        return 1;
      }
    }
    device.waitIdle();
  }
  return 0;
}
