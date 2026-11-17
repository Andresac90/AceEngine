// Level editor entry point.
//
//   ace_editor [project_dir]            open a project (default: projects/pixel_demo)
//   ace_editor --new <dir>              create an empty project there and open it
//   options: --backend metal|vulkan|d3d12, --validation,
//            --export macos|ios-simulator|ios-device   run File > Export, exit when done
//            --autotest   scripted interaction test; exits non-zero on failure (use a project copy)
//            --screenshot out.png [--frames N] [--select <entity>] [--play]   (automated UI capture)
#include "editor.h"
#include <SDL3/SDL_main.h>
#include <filesystem>
#include <imgui_impl_sdl3.h>

using namespace ace;

static bool createProject(const std::string& dir) {
  std::error_code ec;
  std::filesystem::create_directories(dir + "/scenes", ec);
  std::filesystem::create_directories(dir + "/scripts", ec);
  std::filesystem::create_directories(dir + "/assets", ec);
  if (ec) return false;
  Project p;
  p.dir = dir + "/";
  p.name = std::filesystem::path(dir).filename().string();
  if (std::filesystem::exists(p.dir + "project.json")) return true; // never overwrite an existing project
  Scene scene;
  Entity cam = scene.create("Camera");
  scene.reg.emplace<Camera>(cam);
  scene.reg.get<Transform>(cam).position = {10, 10, 10};
  scene.reg.get<Transform>(cam).rotation = quat(glm::radians(vec3(-35.264f, 45.0f, 0.0f)));
  Entity sun = scene.create("Sun");
  scene.reg.emplace<Light>(sun);
  scene.reg.get<Transform>(sun).rotation = quat(glm::radians(vec3(-50.0f, 30.0f, 0.0f)));
  Entity ground = scene.create("Ground");
  scene.reg.emplace<MeshRenderer>(ground).mesh = "builtin:plane";
  scene.reg.get<Transform>(ground).scale = {10, 1, 10};
  return p.save() && scene.save(p.path(p.startScene));
}

int main(int argc, char** argv) {
  std::string projectDir = "projects/pixel_demo", screenshot, select;
  bool play = false, autotest = false;
  std::string exportTarget;
  int frames = 0;
  AppDesc desc;
  desc.title = "AceEngine Editor";
  desc.width = 1600;
  desc.height = 900;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
    if (a == "--new") {
      projectDir = next();
      if (projectDir.empty() || !createProject(projectDir)) {
        ACE_ERROR("could not create a project at '%s'", projectDir.c_str());
        return 1;
      }
    } else if (a == "--backend") {
      std::string name = next();
      if (!parseBackend(name, desc.backend)) {
        ACE_ERROR("backend '%s' is unknown or not available in this build", name.c_str());
        return 2;
      }
    } else if (a == "--validation") desc.validation = true;
    else if (a == "--screenshot") screenshot = next();
    else if (a == "--frames") frames = atoi(next().c_str());
    else if (a == "--select") select = next();
    else if (a == "--play") play = true;
    else if (a == "--autotest") autotest = true;
    else if (a == "--export") exportTarget = next();
    else if (a[0] != '-') projectDir = a;
    else {
      ACE_ERROR("unknown option %s", a.c_str());
      return 2;
    }
  }
  if (!screenshot.empty() && frames <= 0) frames = 10;

  Project project;
  if (!project.load(projectDir)) {
    ACE_ERROR("open a project folder, or create one with: ace_editor --new <dir>");
    return 1;
  }
  App app;
  if (!app.init(desc)) return 1;
  rhi::Device& device = *app.device;
  {
    Editor editor(app, project);
    if (!select.empty()) editor.select(select);
    if (play) editor.play();
    editor.autotest = autotest;
    if (!exportTarget.empty() && !editor.exportTarget(exportTarget)) {
      ACE_ERROR("unknown export target '%s' (macos, ios-simulator, ios-device)", exportTarget.c_str());
      return 2;
    }
    int result = 0;
    rhi::TextureHandle shot;
    u32 shotW = 0, shotH = 0;
    int frame = 0;
    while (app.pump([&](const SDL_Event& e) { editor.onEvent(e); })) {
      editor.buildUi();
      rhi::CommandList* cmd = device.beginFrame();
      if (!cmd) {
        ImGui::EndFrame();
        continue;
      }
      if (!screenshot.empty()) {
        if (!shot) {
          shotW = device.backbufferWidth();
          shotH = device.backbufferHeight();
          shot = device.createTexture({shotW, shotH, rhi::Format::BGRA8_SRGB, rhi::TextureUsage_RenderTarget | rhi::TextureUsage_Sampled, "ui shot"});
        }
        editor.render(*cmd, shot);
        rhi::RenderPassDesc clear;
        clear.color[0].texture = device.backbuffer();
        cmd->beginRenderPass(clear);
        cmd->endRenderPass();
      } else {
        editor.render(*cmd, device.backbuffer());
      }
      device.endFrame();
      if (frames > 0 && ++frame >= frames) break;
      if (autotest && (result = editor.autotestResult()) != 0) break;
      if (!exportTarget.empty() && editor.exportResult() != 0) break;
    }
    if (autotest && result != 1) return 1;
    if (!exportTarget.empty() && editor.exportResult() != 1) return 1;
    if (shot) {
      device.waitIdle();
      std::vector<u8> pixels;
      if (device.readTexture(shot, pixels)) {
        for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]); // BGRA -> RGBA
        savePng(screenshot, shotW, shotH, pixels.data());
        ACE_INFO("wrote %s", screenshot.c_str());
      }
    }
  }
  return 0;
}
