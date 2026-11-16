// Game player: loads a project and runs its start scene. This is the executable
// that ships with an exported game; the editor is not involved.
//
//   ace_player [project_dir] [--backend metal|vulkan|d3d12] [--scene file]
//              [--screenshot out.png] [--frames N] [--validation]
//
// Without a project_dir it runs the "project" folder next to the executable.
#include "platform/app.h"
#include "render/renderer.h"
#include "runtime/project.h"
#include "script/script.h"
#include <SDL3/SDL_main.h>

using namespace ace;

int main(int argc, char** argv) {
  std::string projectDir, sceneOverride, screenshot;
  int frames = 0;
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
    else if (a[0] != '-') projectDir = a;
    else {
      ACE_ERROR("unknown option %s", a.c_str());
      return 2;
    }
  }
  if (!screenshot.empty() && frames <= 0) frames = 8;

  Project project;
  // The project is looked up before SDL starts, so resolve the base path by hand.
  if (projectDir.empty()) projectDir = std::string(SDL_GetBasePath() ? SDL_GetBasePath() : "./") + "project";
  if (!project.load(projectDir)) return 1;
  desc.title = project.name;
  desc.width = project.width;
  desc.height = project.height;

  App app;
  if (!app.init(desc)) return 1;
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
    while (app.pump()) {
      scripts.update(app.dt);
      scene.updateTransforms();

      rhi::CommandList* cmd = device.beginFrame();
      if (!cmd) continue;
      renderer.beginFrame();
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
        renderer.render(*cmd, scene, assets, view, shot, rhi::Format::RGBA8_SRGB, shotW, shotH);
        rhi::RenderPassDesc clear;
        clear.color[0].texture = device.backbuffer();
        cmd->beginRenderPass(clear);
        cmd->endRenderPass();
      } else {
        renderer.render(*cmd, scene, assets, view, device.backbuffer(), device.backbufferFormat(), w, h);
      }
      device.endFrame();

      if (frames > 0 && ++frame >= frames) break;
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
