// Level editor entry point (also the executable inside AceEngine.app).
//
//   ace_editor                          reopen the last project, or ask for one (folder picker)
//   ace_editor [project_dir]            open that project
//   ace_editor --new <dir>              create a starter project there and open it
//   options: --backend metal|vulkan|d3d12, --validation,
//            --export macos|ios-simulator|ios-device   run File > Export, exit when done
//            --focus <panel>   bring a panel (e.g. Palette) to the front
//            --autotest   scripted interaction test; exits non-zero on failure (use a project copy)
//            --screenshot out.png [--frames N] [--select <entity>] [--play]   (automated UI capture)
#include "editor.h"
#include "projects.h"
#include <SDL3/SDL_main.h>
#include <imgui_impl_sdl3.h>

using namespace ace;

int main(int argc, char** argv) {
  std::string projectDir, screenshot, select, exportTarget, focusName;
  bool play = false, autotest = false;
  int frames = 0;
  AppDesc desc;
  desc.title = "AceEngine";
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
    else if (a == "--focus") focusName = next();
    else if (a.rfind("-psn_", 0) == 0) continue; // macOS may pass a process serial number to apps opened from Finder
    else if (a[0] != '-') projectDir = a;
    else {
      ACE_ERROR("unknown option %s", a.c_str());
      return 2;
    }
  }
  if (!screenshot.empty() && frames <= 0) frames = 10;
  // Automated runs render offscreen into a hidden window: no window flashes up,
  // and they keep working while the display is asleep. They never ask questions
  // and never touch the remembered project or saved layout.
  bool automated = !screenshot.empty() || autotest || !exportTarget.empty();
  desc.hidden = automated;
  if (automated && projectDir.empty()) projectDir = lastProject(); // read-only: automated runs never change it
  if (automated && projectDir.empty()) {
    ACE_ERROR("automated runs need a project folder argument (no project is remembered)");
    return 2;
  }

  App app;
  if (!app.init(desc)) return 1;
  rhi::Device& device = *app.device;

  if (projectDir.empty()) projectDir = lastProject();
  if (projectDir.empty()) projectDir = chooseProject(app.window);
  while (!projectDir.empty()) {
    Project project;
    if (!project.load(projectDir)) {
      if (automated) return 1;
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Could not open the project", ("Could not read\n" + projectDir + "/project.json").c_str(),
                               app.window);
      projectDir = chooseProject(app.window);
      continue;
    }
    if (!automated) rememberProject(projectDir);
    projectDir.clear();

    Editor editor(app, project, automated);
    if (!select.empty()) editor.select(select);
    if (play) editor.play();
    editor.autotest = autotest;
    editor.focusWindow = focusName;
    if (!exportTarget.empty() && !editor.exportTarget(exportTarget)) {
      ACE_ERROR("unknown export target '%s' (macos, ios-simulator, ios-device)", exportTarget.c_str());
      return 2;
    }
    int result = 0;
    rhi::TextureHandle shot;
    u32 shotW = 0, shotH = 0;
    int frame = 0;
    for (;;) {
      if (!app.pump([&](const SDL_Event& e) { editor.onEvent(e); })) {
        if (editor.confirmLeave()) break; // quit (Cmd+Q, window close)
        continue;
      }
      editor.buildUi();
      if (!editor.requestedProject.empty()) { // File > Open Project
        ImGui::EndFrame();
        projectDir = editor.requestedProject;
        break;
      }
      rhi::CommandList* cmd = device.beginFrame(!automated);
      if (!cmd) {
        ImGui::EndFrame();
        continue;
      }
      if (automated) {
        if (!shot) {
          shotW = device.backbufferWidth();
          shotH = device.backbufferHeight();
          // Same format as the window would use, so the UI pipeline matches.
          shot = device.createTexture({shotW, shotH, device.backbufferFormat(), rhi::TextureUsage_RenderTarget | rhi::TextureUsage_Sampled, "offscreen ui"});
        }
        editor.render(*cmd, shot);
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
    if (shot && !screenshot.empty()) {
      device.waitIdle();
      std::vector<u8> pixels;
      if (device.readTexture(shot, pixels)) {
        rhi::Format f = device.backbufferFormat();
        if (f == rhi::Format::BGRA8 || f == rhi::Format::BGRA8_SRGB)
          for (size_t i = 0; i + 3 < pixels.size(); i += 4) std::swap(pixels[i], pixels[i + 2]); // BGRA -> RGBA
        savePng(screenshot, shotW, shotH, pixels.data());
        ACE_INFO("wrote %s", screenshot.c_str());
      }
    }
    if (shot) device.destroy(shot);
  }
  return 0;
}
