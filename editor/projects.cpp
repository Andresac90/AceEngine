#include "projects.h"
#include "runtime/project.h"
#include "scene/scene.h"
#include <SDL3/SDL.h>
#include <filesystem>
#include <mutex>

namespace ace {

namespace fs = std::filesystem;

bool isProject(const std::string& dir) {
  std::error_code ec;
  return !dir.empty() && fs::is_regular_file(fs::path(dir) / "project.json", ec);
}

bool createProject(const std::string& dir) {
  std::error_code ec;
  for (const char* sub : {"scenes", "scripts", "assets"}) fs::create_directories(fs::path(dir) / sub, ec);
  if (ec) return false;
  Project p;
  p.dir = dir + "/";
  p.name = fs::path(dir).filename().string();
  if (isProject(dir)) return true; // never overwrite an existing project
  Scene scene;
  Entity cam = scene.create("Camera");
  scene.reg.emplace<Camera>(cam).orthographic = false;
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

std::string editorSettingsDir() {
  char* dir = SDL_GetPrefPath("AceEngine", "Editor"); // created if missing
  std::string out = dir ? dir : "./";
  SDL_free(dir);
  return out;
}

std::string lastProject() {
  std::string dir;
  if (!readTextFile(editorSettingsDir() + "last_project.txt", dir)) return "";
  while (!dir.empty() && (dir.back() == '\n' || dir.back() == '\r')) dir.pop_back();
  return isProject(dir) ? dir : "";
}

void rememberProject(const std::string& dir) {
  std::error_code ec;
  fs::path abs = fs::absolute(dir, ec);
  writeTextFile(editorSettingsDir() + "last_project.txt", (ec ? fs::path(dir) : abs).lexically_normal().string());
}

namespace {
struct Pick {
  std::mutex mutex;
  bool done = false;
  std::string path;
};

// The system folder picker; waits for the answer while keeping events flowing.
std::string askForFolder(SDL_Window* parent, const std::string& start) {
  Pick pick;
  SDL_ShowOpenFolderDialog(
      [](void* user, const char* const* files, int) {
        Pick* p = (Pick*)user;
        std::lock_guard<std::mutex> lock(p->mutex);
        if (files && files[0]) p->path = files[0];
        p->done = true; // also on cancel (empty list) and on error (null)
      },
      &pick, parent, start.empty() ? nullptr : start.c_str(), false);
  for (;;) {
    {
      std::lock_guard<std::mutex> lock(pick.mutex);
      if (pick.done) return pick.path;
    }
    SDL_Event e;
    if (SDL_WaitEventTimeout(&e, 50) && e.type == SDL_EVENT_QUIT) return "";
  }
}
} // namespace

std::string chooseProject(SDL_Window* parent) {
  std::string start = lastProject();
  if (!start.empty()) start = fs::path(start).parent_path().string();
  for (;;) {
    std::string dir = askForFolder(parent, start);
    if (dir.empty()) return "";
    if (isProject(dir)) return dir;
    std::error_code ec;
    if (fs::is_empty(dir, ec) && !ec) {
      if (createProject(dir)) return dir;
      SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Could not create the project", ("Could not write to\n" + dir).c_str(), parent);
      continue;
    }
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "Not an AceEngine project",
                             "That folder has no project.json.\n\nChoose a project folder, or an empty folder to start a new project.",
                             parent);
    start = dir;
  }
}

} // namespace ace
