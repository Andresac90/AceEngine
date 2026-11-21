#include "editor.h"
#include "projects.h"

#include <ImGuizmo.h>
#include <algorithm>
#include <filesystem>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#ifdef __APPLE__
#include <spawn.h>
#endif

namespace ace {

using json = nlohmann::json;

namespace {

constexpr size_t kMaxUndoSteps = 100;
constexpr size_t kMaxConsoleLines = 500;

// Requests raised while iterating the hierarchy, applied after the loop.
bool g_deleteRequest = false;
bool g_duplicateRequest = false;

std::vector<Entity> orderedEntities(Scene& scene) {
  std::vector<Entity> out;
  for (Entity e : scene.reg.view<Name>()) out.push_back(e);
  std::sort(out.begin(), out.end());
  return out;
}

bool endsWith(const std::string& s, const char* suffix) {
  size_t n = strlen(suffix);
  return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

} // namespace

CameraView EditorCamera::view() const {
  quat rot = glm::angleAxis(glm::radians(yaw), vec3(0, 1, 0)) * glm::angleAxis(glm::radians(pitch), vec3(1, 0, 0));
  CameraView v;
  v.world = glm::translate(mat4(1.0f), target + rot * vec3(0, 0, distance)) * glm::mat4_cast(rot);
  v.camera.orthographic = orthographic;
  v.camera.orthoSize = orthoSize;
  v.camera.farPlane = 500.0f;
  return v;
}

Editor::Editor(App& app, Project project, bool automated)
    : app_(app), project_(std::move(project)), renderer_(*app.device), assets_(*app.device, project_.dir) {
  setLogSink([this](LogLevel level, const std::string& line) {
    console_.emplace_back(level, line);
    if (console_.size() > kMaxConsoleLines) console_.erase(console_.begin());
  });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
  // The layout lives in the user's settings folder: an app opened from Finder
  // starts in "/", which is not writable. Automated runs use the default layout.
  automated_ = automated;
  iniPath_ = editorSettingsDir() + "imgui.ini";
  io.IniFilename = automated ? nullptr : iniPath_.c_str();
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  ImGui::StyleColorsDark();
  ImGuiStyle& style = ImGui::GetStyle();
  style.WindowRounding = style.FrameRounding = style.TabRounding = 3.0f;
  style.FramePadding = ImVec2(6, 4);
  ImGui_ImplSDL3_InitForOther(app_.window);
  uiFormat = app_.device->backbufferFormat();
  ui_ = std::make_unique<ImGuiRhi>(*app_.device, renderer_.loadShader("imgui", rhi::ShaderStage::Vertex),
                                   renderer_.loadShader("imgui", rhi::ShaderStage::Fragment), uiFormat);
  openScene(project_.startScene);
  ACE_INFO("Editor ready: %s (%s)", project_.name.c_str(), rhi::backendName(app_.device->backend()));
}

void Editor::flushExportLog() {
  std::lock_guard<std::mutex> lock(exportMutex_);
  for (const std::string& line : exportLines_) {
    if (line.find("FAILED") != std::string::npos || line.find("error") != std::string::npos) ACE_ERROR("%s", line.c_str());
    else ACE_INFO("%s", line.c_str());
  }
  exportLines_.clear();
}

Editor::~Editor() {
  if (exportThread_.joinable()) exportThread_.join(); // let a running export finish rather than leave a half-built app
  flushExportLog();
  setLogSink({});
  app_.device->waitIdle();
  scripts_.reset();
  ui_.reset();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
}

void Editor::onEvent(const SDL_Event& e) { ImGui_ImplSDL3_ProcessEvent(&e); }

// ---------------------------------------------------------------- documents

void Editor::commit() {
  undoStack_.resize(undoStack_.empty() ? 0 : undoIndex_ + 1); // drop any redo tail
  undoStack_.push_back(scene_.toJson().dump());
  if (undoStack_.size() > kMaxUndoSteps) undoStack_.erase(undoStack_.begin());
  undoIndex_ = undoStack_.size() - 1;
  dirty_ = true;
}

void Editor::restore(const std::string& snapshot) {
  std::vector<Entity> before = orderedEntities(scene_);
  auto it = std::find(before.begin(), before.end(), selected_);
  size_t index = it == before.end() ? SIZE_MAX : (size_t)(it - before.begin());
  scene_.fromJson(json::parse(snapshot, nullptr, false));
  std::vector<Entity> after = orderedEntities(scene_);
  selected_ = index < after.size() ? after[index] : kNoEntity;
}

void Editor::undo() {
  if (playing_ || undoIndex_ == 0) return;
  restore(undoStack_[--undoIndex_]);
  dirty_ = true;
}

void Editor::redo() {
  if (playing_ || undoIndex_ + 1 >= undoStack_.size()) return;
  restore(undoStack_[++undoIndex_]);
  dirty_ = true;
}

void Editor::newScene() {
  setPlaying(false);
  int n = 1;
  do {
    scenePath_ = "scenes/untitled_" + std::to_string(n++) + ".scene.json";
  } while (std::filesystem::exists(project_.path(scenePath_)));
  scene_.clear();
  scene_.settings = RenderSettings{};
  Entity cam = scene_.create("Camera");
  scene_.reg.emplace<Camera>(cam);
  scene_.reg.get<Transform>(cam).position = {10, 10, 10};
  scene_.reg.get<Transform>(cam).rotation = quat(glm::radians(vec3(-35.264f, 45.0f, 0.0f)));
  Entity sun = scene_.create("Sun");
  scene_.reg.emplace<Light>(sun);
  scene_.reg.get<Transform>(sun).rotation = quat(glm::radians(vec3(-50.0f, 30.0f, 0.0f)));
  selected_ = kNoEntity;
  undoStack_.clear();
  commit();
}

void Editor::openScene(const std::string& relativePath) {
  setPlaying(false);
  if (!scene_.load(project_.path(relativePath))) {
    if (scenePath_.empty()) newScene(); // nothing loaded yet: start with an empty scene
    return;
  }
  scenePath_ = relativePath;
  selected_ = kNoEntity;
  undoStack_.clear();
  commit();
  dirty_ = false;
}

void Editor::saveScene() {
  if (playing_) {
    ACE_WARN("stop the game before saving (play-mode changes are temporary)");
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(project_.path(scenePath_)).parent_path(), ec);
  if (scene_.save(project_.path(scenePath_))) {
    dirty_ = false;
    ACE_INFO("saved %s", scenePath_.c_str());
  } else {
    ACE_ERROR("could not write %s", scenePath_.c_str());
  }
  assetsRefreshed_ = -10.0;
}

void Editor::setPlaying(bool playing) {
  if (playing == playing_) return;
  playing_ = playing;
  if (playing) {
    playSnapshot_ = scene_.toJson().dump();
    scripts_ = std::make_unique<ScriptSystem>(scene_, assets_, app_.input);
  } else {
    scripts_.reset();
    restore(playSnapshot_);
  }
}

Entity Editor::createEntity(const char* name, const char* mesh) {
  Entity e = scene_.create(name);
  if (mesh) scene_.reg.emplace<MeshRenderer>(e).mesh = mesh;
  if (mesh && strcmp(mesh, "builtin:plane") != 0) scene_.reg.get<Transform>(e).position.y = 0.5f;
  selected_ = e;
  commit();
  return e;
}

void Editor::startExport(const char* script, const char* extraArg) {
#ifdef _WIN32
  ACE_ERROR("exporting from the editor is not supported on Windows yet; see docs/HANDOFF.md");
  (void)script;
  (void)extraArg;
#else
  if (exporting_) return;
  if (exportThread_.joinable()) exportThread_.join();
  auto quote = [](const std::string& s) { // single-quote for sh, escaping embedded quotes
    std::string out = "'";
    for (char c : s) out += c == '\'' ? std::string("'\\''") : std::string(1, c);
    return out + "'";
  };
  std::string command = quote(std::string(ACE_SOURCE_DIR) + "/tools/" + script) + " " + quote(project_.dir);
  if (extraArg) command += std::string(" ") + extraArg;
  command += " 2>&1";
  ACE_INFO("export: %s", command.c_str());
  exporting_ = true;
  exportResult_ = 0;
  exportThread_ = std::thread([this, command] {
    auto emit = [this](std::string line) {
      std::lock_guard<std::mutex> lock(exportMutex_);
      exportLines_.push_back(std::move(line));
    };
    FILE* pipe = popen(command.c_str(), "r");
    if (!pipe) {
      emit("export: could not start the export script");
      exportResult_ = -1;
    } else {
      char buffer[1024];
      while (fgets(buffer, sizeof(buffer), pipe)) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        emit(line);
      }
      int status = pclose(pipe);
      emit(status == 0 ? "export: done" : "export: FAILED (see the lines above)");
      exportResult_ = status == 0 ? 1 : -1;
    }
    exporting_ = false;
  });
#endif
}

// Open (or create, then open) the entity's Lua script in a code editor.
bool Editor::drawScriptButtons(Script& script) {
  std::error_code ec;
  bool exists = !script.path.empty() && std::filesystem::is_regular_file(project_.path(script.path), ec);
  if (exists) {
    if (ImGui::Button("Open script", ImVec2(-1, 0))) openInCodeEditor(project_.path(script.path));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Edit %s. Saving it while the game runs reloads it.", script.path.c_str());
    return false;
  }
  if (!ImGui::Button(script.path.empty() ? "New script" : "Create this script", ImVec2(-1, 0))) return false;
  std::string path = script.path.empty() ? newScriptPath(scene_.reg.get<Name>(selected_).value) : script.path;
  if (!writeScriptTemplate(path, scene_.reg.get<Name>(selected_).value)) return false;
  script.path = path;
  assetsRefreshed_ = -10.0;
  openInCodeEditor(project_.path(path));
  return true;
}

// scripts/<name>.lua, made file-name safe and unique.
std::string Editor::newScriptPath(const std::string& entityName) {
  std::string base;
  for (char c : entityName) base += isalnum((unsigned char)c) ? (char)tolower((unsigned char)c) : '_';
  if (base.empty()) base = "script";
  std::string path = "scripts/" + base + ".lua";
  for (int n = 2; std::filesystem::exists(project_.path(path)); n++) path = "scripts/" + base + "_" + std::to_string(n) + ".lua";
  return path;
}

bool Editor::writeScriptTemplate(const std::string& relativePath, const std::string& entityName) {
  std::error_code ec;
  std::filesystem::path file = project_.path(relativePath);
  if (std::filesystem::exists(file, ec)) return true; // never overwrite someone's script
  std::filesystem::create_directories(file.parent_path(), ec);
  std::string text = "-- " + entityName + R"(: what this script does.
-- Functions: docs/SCRIPTING.md in the engine (ace.position, ace.key, ace.move, ...).
-- Saving this file while the game runs reloads it.
local Script = {}

function Script:start()
  -- Runs once, when play starts.
end

function Script:update(dt)
  -- Runs every frame; dt is the time since the last frame, in seconds.
  -- local x, y, z = ace.position(self.entity)
end

return Script
)";
  if (!writeTextFile(file.string(), text)) {
    ACE_ERROR("could not write %s", relativePath.c_str());
    return false;
  }
  ACE_INFO("created %s", relativePath.c_str());
  return true;
}

void Editor::openInCodeEditor(const std::string& file) {
  if (automated_) { // tests must not launch apps on the user's desktop
    ACE_INFO("would open %s", file.c_str());
    return;
  }
#ifdef __APPLE__
  // VS Code if it is installed (it knows Lua); otherwise the app macOS associates with .lua.
  std::error_code ec;
  if (std::filesystem::exists("/Applications/Visual Studio Code.app", ec)) {
    pid_t pid;
    const char* args[] = {"open", "-a", "Visual Studio Code", file.c_str(), nullptr};
    if (posix_spawnp(&pid, "open", nullptr, nullptr, (char* const*)args, nullptr) == 0) return;
  }
#endif
  // file:///C:/dir/x.lua on Windows, file:///dir/x.lua elsewhere.
  std::string path = std::filesystem::absolute(file).generic_string();
  if (path.empty() || path[0] != '/') path = "/" + path;
  std::string url = "file://";
  for (unsigned char c : path) { // percent-encode all but safe characters
    if (isalnum(c) || strchr("/-_.~:", c)) url += (char)c;
    else {
      char hex[4];
      snprintf(hex, sizeof(hex), "%%%02X", c);
      url += hex;
    }
  }
  if (!SDL_OpenURL(url.c_str())) ACE_ERROR("could not open %s: %s", file.c_str(), SDL_GetError());
}

bool Editor::confirmLeave() {
  if (automated_ || (!dirty_ && !paletteDirty_)) return true;
  if (playing_) setPlaying(false); // play-mode changes are temporary anyway
  if (!dirty_ && !paletteDirty_) return true;
  std::string what = dirty_ && paletteDirty_ ? scenePath_ + " and the palette" : (dirty_ ? scenePath_ : std::string("the palette"));
  std::string text = "Save changes to " + what + "?";
  const SDL_MessageBoxButtonData buttons[] = {
      {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
      {0, 1, "Don't Save"},
      {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 2, "Save"},
  };
  SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, app_.window, "Unsaved changes", text.c_str(), 3, buttons, nullptr};
  int choice = 0;
  if (!SDL_ShowMessageBox(&box, &choice)) return true; // no dialog available: do not trap the user
  if (choice == 0) return false;
  if (choice == 2) {
    if (dirty_) saveScene();
    if (paletteDirty_) savePalette();
    return !dirty_ && !paletteDirty_; // a failed save keeps the editor open
  }
  return true;
}

bool Editor::isSelected(Entity e) const {
  return e != kNoEntity && (e == selected_ || std::find(extraSelection_.begin(), extraSelection_.end(), e) != extraSelection_.end());
}

void Editor::toggleSelected(Entity e) {
  if (e == selected_) { // drop the primary; the most recent extra takes over
    selected_ = extraSelection_.empty() ? kNoEntity : extraSelection_.back();
    if (!extraSelection_.empty()) extraSelection_.pop_back();
  } else if (isSelected(e)) {
    std::erase(extraSelection_, e);
  } else {
    if (selected_ != kNoEntity) extraSelection_.push_back(selected_);
    selected_ = e;
  }
  lastPrimary_ = selected_; // keep the extras
}

std::vector<Entity> Editor::selectionRoots() {
  std::vector<Entity> all;
  if (selected_ != kNoEntity) all.push_back(selected_);
  all.insert(all.end(), extraSelection_.begin(), extraSelection_.end());
  std::vector<Entity> roots;
  for (Entity e : all) {
    bool inside = false;
    for (Entity p = scene_.reg.get<Transform>(e).parent; scene_.reg.valid(p) && !inside; p = scene_.reg.get<Transform>(p).parent)
      inside = isSelected(p);
    if (!inside) roots.push_back(e);
  }
  return roots;
}

void Editor::selectAll(const std::vector<Entity>& entities) {
  selected_ = entities.empty() ? kNoEntity : entities[0];
  extraSelection_.assign(entities.size() > 1 ? entities.begin() + 1 : entities.end(), entities.end());
  lastPrimary_ = selected_;
}

void Editor::deleteSelection() {
  std::vector<Entity> roots = selectionRoots();
  if (roots.empty()) return;
  for (Entity e : roots) scene_.destroy(e);
  selectAll({});
  commit();
}

void Editor::duplicateSelection() {
  std::vector<Entity> copies;
  for (Entity e : selectionRoots()) copies.push_back(scene_.duplicate(e));
  if (copies.empty()) return;
  selectAll(copies);
  commit();
}

void Editor::copySelection() {
  std::vector<Entity> roots = selectionRoots();
  if (roots.empty()) return;
  nlohmann::json j = scene_.copyEntities(roots);
  j["ace_clipboard"] = 1; // marks clipboard text as ours
  SDL_SetClipboardText(j.dump().c_str());
  ACE_INFO("copied %zu entit%s", roots.size(), roots.size() == 1 ? "y" : "ies");
}

void Editor::paste() {
  if (playing_) return;
  char* text = SDL_GetClipboardText();
  nlohmann::json j = nlohmann::json::parse(text ? text : "", nullptr, false);
  SDL_free(text);
  if (j.is_discarded() || !j.is_object() || !j.contains("ace_clipboard")) return; // not something we copied
  std::vector<Entity> roots = scene_.pasteEntities(j);
  if (roots.empty()) return;
  selectAll(roots);
  commit();
}

Entity Editor::placePrefab(const std::string& relativePath) {
  Entity root = scene_.instantiatePrefab(project_.path(relativePath));
  if (root == kNoEntity) return kNoEntity;
  // At the point the editor camera orbits around, snapped to the move grid.
  vec3 at = camera_.target;
  if (snap_ && snapMove_ > 0.0f) at = glm::round(at / snapMove_) * snapMove_;
  scene_.reg.get<Transform>(root).position = vec3(at.x, scene_.reg.get<Transform>(root).position.y, at.z);
  selected_ = root;
  commit();
  return root;
}

bool Editor::exportTarget(const std::string& target) {
  if (target == "macos") startExport("export_macos.sh", nullptr);
  else if (target == "ios-simulator") startExport("export_ios.sh", "simulator");
  else if (target == "ios-device") startExport("export_ios.sh", "device");
  else return false;
  return true;
}

void Editor::refreshAssets() {
  assetFiles_.clear();
  std::error_code ec;
  std::filesystem::path root(project_.dir);
  for (auto it = std::filesystem::recursive_directory_iterator(root, ec); !ec && it != std::filesystem::recursive_directory_iterator();
       it.increment(ec)) {
    std::string name = it->path().filename().string();
    if (!name.empty() && name[0] == '.') {
      if (it->is_directory(ec)) it.disable_recursion_pending();
      continue;
    }
    if (it->is_regular_file(ec)) assetFiles_.push_back(std::filesystem::relative(it->path(), root, ec).generic_string());
  }
  std::sort(assetFiles_.begin(), assetFiles_.end());
}

// ---------------------------------------------------------------- frame

void Editor::buildUi() {
  // Resize the viewport texture before the UI references it this frame.
  if (wantedW_ && wantedH_ && (wantedW_ != viewportW_ || wantedH_ != viewportH_)) {
    app_.device->waitIdle();
    if (viewportTexture_) {
      ui_->forgetTexture(viewportTexture_);
      app_.device->destroy(viewportTexture_);
    }
    viewportW_ = wantedW_;
    viewportH_ = wantedH_;
    viewportTexture_ = app_.device->createTexture({viewportW_, viewportH_, rhi::Format::RGBA8_SRGB,
                                                   rhi::TextureUsage_RenderTarget | rhi::TextureUsage_Sampled, "editor viewport"});
  }
  flushExportLog();
  if (app_.time - assetsRefreshed_ > 2.0) {
    refreshAssets();
    assetsRefreshed_ = app_.time;
  }

  ImGui_ImplSDL3_NewFrame();
  if (autotest) autotestStep(); // after the backend, so injected input wins over the real mouse
  ImGui::NewFrame();
  ImGuizmo::BeginFrame();

  ImGuiID dock = ImGui::DockSpaceOverViewport(0, ImGui::GetMainViewport());
  if (firstFrame_) {
    firstFrame_ = false;
    ImGuiDockNode* node = ImGui::DockBuilderGetNode(dock);
    if (!node || !node->IsSplitNode()) { // no saved layout: build the default one
      ImGui::DockBuilderRemoveNode(dock);
      ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
      ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->Size);
      ImGuiID center = dock;
      ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.18f, nullptr, &center);
      ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
      ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.26f, nullptr, &center);
      ImGui::DockBuilderDockWindow("Hierarchy", left);
      ImGui::DockBuilderDockWindow("Inspector", right);
      ImGui::DockBuilderDockWindow("Render Settings", right);
      ImGui::DockBuilderDockWindow("Palette", right);
      ImGui::DockBuilderDockWindow("Assets", bottom);
      ImGui::DockBuilderDockWindow("Console", bottom);
      ImGui::DockBuilderDockWindow("Viewport", center);
      ImGui::DockBuilderFinish(dock);
    }
  }

  if (!scene_.reg.valid(selected_)) selected_ = kNoEntity;
  if (selected_ != lastPrimary_) { // a plain click, pick, undo...: the selection is just the new primary
    extraSelection_.clear();
    lastPrimary_ = selected_;
  }
  std::erase_if(extraSelection_, [&](Entity e) { return !scene_.reg.valid(e) || e == selected_; });
  handleShortcuts();
  drawMenu();
  drawHierarchy();
  drawInspector();
  drawSettings();
  drawPalette();
  drawAssets();
  drawConsole();
  drawViewport();

  if (g_deleteRequest) {
    g_deleteRequest = false;
    deleteSelection();
  }
  if (g_duplicateRequest) {
    g_duplicateRequest = false;
    duplicateSelection();
  }

  if (!focusWindow.empty() && ImGui::GetFrameCount() < 5) ImGui::SetWindowFocus(focusWindow.c_str());

  if (playing_) {
    scripts_->update(app_.dt);
    if (!scripts_->pendingScene.empty()) { // level change during play: temporary, like every play-mode change
      std::string next = std::exchange(scripts_->pendingScene, std::string());
      if (scene_.load(project_.path(next))) {
        scripts_->restart();
        selected_ = kNoEntity;
      }
    }
  }
  if (!scene_.reg.valid(selected_)) selected_ = kNoEntity; // a script may have destroyed it
  scene_.updateTransforms();

  static std::string lastTitle;
  std::string title = project_.name + " - " + scenePath_ + (dirty_ ? " *" : "") + " - AceEngine";
  if (title != lastTitle) SDL_SetWindowTitle(app_.window, (lastTitle = title).c_str());
}

void Editor::render(rhi::CommandList& cmd, rhi::TextureHandle target) {
  ImGui::Render();
  renderer_.beginFrame(app_.time);
  if (viewportTexture_) {
    CameraView view = camera_.view();
    if (playing_) Renderer::findCamera(scene_, view); // falls back to the editor camera
    std::span<const TextDraw> texts;
    if (playing_) texts = scripts_->texts;
    renderer_.render(cmd, scene_, assets_, view, viewportTexture_, rhi::Format::RGBA8_SRGB, viewportW_, viewportH_, texts);
    if (playing_) scripts_->gameSize = renderer_.gameSize();
  }
  rhi::RenderPassDesc pass;
  pass.name = "editor ui";
  pass.color[0].texture = target;
  pass.color[0].clear[0] = pass.color[0].clear[1] = pass.color[0].clear[2] = 0.02f;
  cmd.beginRenderPass(pass);
  ui_->render(cmd, ImGui::GetDrawData());
  cmd.endRenderPass();
}

void Editor::handleShortcuts() {
  ImGuiIO& io = ImGui::GetIO();
  if (io.WantTextInput) return;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) saveScene();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) redo();
  else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
  if (ImGui::IsKeyPressed(ImGuiKey_F5)) setPlaying(!playing_);
  if (playing_) return; // the keys below belong to the game while it runs
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) paste();
  if (selected_ != kNoEntity) {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) g_duplicateRequest = true;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copySelection();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Backspace)) g_deleteRequest = true;
    if (ImGui::IsKeyPressed(ImGuiKey_F)) camera_.target = vec3(scene_.reg.get<Transform>(selected_).world[3]);
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !io.KeyCtrl) {
    if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmoOp_ = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmoOp_ = 1;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmoOp_ = 2;
  }
}

// ---------------------------------------------------------------- panels

void Editor::drawMenu() {
  static std::string saveAsPath;
  bool openSaveAs = false;
  if (ImGui::BeginMainMenuBar()) {
    bool fileMenu = ImGui::BeginMenu("File");
    testRect("menu:File");
    if (fileMenu) {
      if (ImGui::MenuItem("Open Project...")) {
        // An empty folder starts a new project.
        if (confirmLeave()) {
          std::string dir = chooseProject(app_.window);
          if (!dir.empty()) requestedProject = dir;
        }
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Pick a project folder, or an empty folder for a new project");
      ImGui::Separator();
      if (ImGui::MenuItem("New Scene")) newScene();
      testRect("item:New Scene");
      if (ImGui::BeginMenu("Open Scene")) {
        for (const std::string& f : assetFiles_)
          if (endsWith(f, ".scene.json") && ImGui::MenuItem(f.c_str(), nullptr, f == scenePath_)) openScene(f);
        ImGui::EndMenu();
      }
      if (ImGui::MenuItem("Save Scene", "Ctrl+S")) saveScene();
      if (ImGui::MenuItem("Save Scene As...")) {
        saveAsPath = scenePath_;
        openSaveAs = true;
      }
      testRect("item:Save Scene As...");
      ImGui::Separator();
      if (ImGui::MenuItem("Use As Start Scene", nullptr, project_.startScene == scenePath_)) {
        project_.startScene = scenePath_;
        project_.save();
      }
      if (ImGui::MenuItem("Reload Assets")) {
        assets_.reload();
        renderer_.clearTextureCaches();
        paletteRows_.clear(); // re-read from the file
        palettePath_.clear();
      }
      if (ImGui::BeginMenu("Export", !exporting_)) {
        if (ImGui::MenuItem("macOS app")) exportTarget("macos");
        if (ImGui::MenuItem("iOS Simulator app")) exportTarget("ios-simulator");
        if (ImGui::MenuItem("iOS device app (unsigned)")) exportTarget("ios-device");
        ImGui::EndMenu();
      }
      if (exporting_) ImGui::TextDisabled("Exporting... (see Console)");
      ImGui::Separator();
      if (ImGui::MenuItem("Quit")) {
        SDL_Event quit{};
        quit.type = SDL_EVENT_QUIT;
        SDL_PushEvent(&quit);
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
      if (ImGui::MenuItem("Undo", "Ctrl+Z", false, !playing_ && undoIndex_ > 0)) undo();
      if (ImGui::MenuItem("Redo", "Ctrl+Shift+Z", false, !playing_ && undoIndex_ + 1 < undoStack_.size())) redo();
      ImGui::Separator();
      if (ImGui::MenuItem("Copy", "Ctrl+C", false, selected_ != kNoEntity)) copySelection();
      if (ImGui::MenuItem("Paste", "Ctrl+V")) paste();
      if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selected_ != kNoEntity)) g_duplicateRequest = true;
      if (ImGui::MenuItem("Delete", "Del", false, selected_ != kNoEntity)) g_deleteRequest = true;
      ImGui::EndMenu();
    }
    bool entityMenu = ImGui::BeginMenu("Entity");
    testRect("menu:Entity");
    if (entityMenu) {
      if (ImGui::MenuItem("Empty")) createEntity("Entity", nullptr);
      if (ImGui::MenuItem("Block (textured, solid)")) { // level-building brick: world-mapped texture + collider
        Entity e = createEntity("Block", "builtin:cube");
        scene_.reg.get<MeshRenderer>(e).worldTexels = true;
        scene_.reg.emplace<Collider>(e);
      }
      if (ImGui::MenuItem("Cube")) createEntity("Cube", "builtin:cube");
      testRect("item:Cube");
      if (ImGui::MenuItem("Sphere")) createEntity("Sphere", "builtin:sphere");
      if (ImGui::MenuItem("Plane")) createEntity("Plane", "builtin:plane");
      if (ImGui::MenuItem("Sprite")) scene_.reg.emplace<SpriteRenderer>(createEntity("Sprite", nullptr));
      if (ImGui::MenuItem("Camera")) scene_.reg.emplace<Camera>(createEntity("Camera", nullptr));
      if (ImGui::MenuItem("Light")) scene_.reg.emplace<Light>(createEntity("Light", nullptr));
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
  if (openSaveAs) ImGui::OpenPopup("Save Scene As");
  if (ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("Path inside the project:");
    ImGui::SetNextItemWidth(360);
    ImGui::InputText("##path", &saveAsPath);
    testRect("saveas:path");
    // Keep saves inside the project folder.
    bool valid = !saveAsPath.empty() && saveAsPath[0] != '/' && saveAsPath.find("..") == std::string::npos;
    ImGui::BeginDisabled(!valid);
    bool save = ImGui::Button("Save");
    testRect("saveas:save");
    if (save) {
      if (!endsWith(saveAsPath, ".scene.json")) saveAsPath += ".scene.json";
      scenePath_ = saveAsPath;
      saveScene();
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

void Editor::drawHierarchyNode(Entity e) {
  std::vector<Entity> kids = scene_.children(e);
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen;
  if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
  if (isSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;
  ImGui::PushID((int)(u32)e);
  bool open = ImGui::TreeNodeEx("##node", flags, "%s", scene_.reg.get<Name>(e).value.c_str());
  testRect("node:" + scene_.reg.get<Name>(e).value);
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeyShift) toggleSelected(e); // Cmd-click on macOS arrives as Ctrl
    else selected_ = e;
  }
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("ACE_ENTITY", &e, sizeof(e));
    ImGui::TextUnformatted(scene_.reg.get<Name>(e).value.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ACE_ENTITY")) {
      scene_.setParent(*(const Entity*)p->Data, e);
      commit();
    }
    ImGui::EndDragDropTarget();
  }
  if (ImGui::BeginPopupContextItem()) {
    if (!isSelected(e)) selected_ = e; // right-click inside a multi-selection keeps it
    if (ImGui::MenuItem("Add Child")) {
      Entity child = scene_.create("Entity", e);
      selected_ = child;
      commit();
    }
    if (ImGui::MenuItem("Copy")) copySelection();
    if (ImGui::MenuItem("Duplicate")) g_duplicateRequest = true;
    if (ImGui::MenuItem("Save as Prefab")) {
      // prefabs/<name>.prefab.json, with the name reduced to safe file characters.
      std::string file;
      for (char c : scene_.reg.get<Name>(e).value) file += isalnum((unsigned char)c) || c == '-' || c == '_' ? c : '_';
      if (file.empty()) file = "prefab";
      std::error_code ec;
      std::filesystem::create_directories(project_.path("prefabs"), ec);
      std::string relative = "prefabs/" + file + ".prefab.json";
      if (scene_.savePrefab(e, project_.path(relative))) ACE_INFO("saved %s (double-click it in Assets to place a copy)", relative.c_str());
      else ACE_ERROR("could not write %s", relative.c_str());
      assetsRefreshed_ = -10.0;
    }
    if (ImGui::MenuItem("Delete")) g_deleteRequest = true;
    ImGui::EndPopup();
  }
  if (open) {
    for (Entity c : kids) drawHierarchyNode(c);
    ImGui::TreePop();
  }
  ImGui::PopID();
}

void Editor::drawHierarchy() {
  ImGui::Begin("Hierarchy");
  for (Entity root : scene_.children(kNoEntity)) drawHierarchyNode(root);
  // Empty space: click to deselect, drop to un-parent.
  ImVec2 rest = ImGui::GetContentRegionAvail();
  ImGui::InvisibleButton("##blank", ImVec2(std::max(rest.x, 1.0f), std::max(rest.y, 40.0f)));
  testRect("hierarchy:blank");
  if (ImGui::IsItemClicked()) selected_ = kNoEntity;
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ACE_ENTITY")) {
      scene_.setParent(*(const Entity*)p->Data, kNoEntity);
      commit();
    }
    ImGui::EndDragDropTarget();
  }
  ImGui::End();
}

bool Editor::editFields(void* object, const std::vector<Field>& fields) {
  bool finished = false;
  for (const Field& f : fields) {
    u8* p = (u8*)object + f.offset;
    ImGui::PushID(f.name);
    bool changed = false, instant = false;
    switch (f.type) {
      case FieldType::Bool: changed = instant = ImGui::Checkbox(f.name, (bool*)p); break;
      case FieldType::Int: changed = ImGui::DragInt(f.name, (int*)p, 1.0f, 0, 100000); break;
      case FieldType::Float: changed = ImGui::DragFloat(f.name, (f32*)p, 0.02f); break;
      case FieldType::Vec2: changed = ImGui::DragFloat2(f.name, (f32*)p, 0.05f); break;
      case FieldType::Vec3: changed = ImGui::DragFloat3(f.name, (f32*)p, 0.05f); break;
      case FieldType::Color3: changed = ImGui::ColorEdit3(f.name, (f32*)p, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR); break;
      case FieldType::Color4: changed = ImGui::ColorEdit4(f.name, (f32*)p); break;
      case FieldType::Quat: {
        // shortcut: Euler angles are re-derived from the quaternion every frame,
        // so values can jump near +-90 degrees pitch; store Euler alongside if it annoys.
        quat& q = *(quat*)p;
        vec3 euler = glm::degrees(glm::eulerAngles(q));
        if ((changed = ImGui::DragFloat3(f.name, &euler.x, 0.5f))) q = glm::normalize(quat(glm::radians(euler)));
        break;
      }
      case FieldType::String:
      case FieldType::Asset: {
        if (f.type == FieldType::Asset) {
          if (assetPicker(f, *(std::string*)p)) changed = instant = true;
          ImGui::SameLine(0.0f, 2.0f);
        }
        changed |= ImGui::InputText(f.name, (std::string*)p);
        testRect(std::string("field:") + f.name);
        if (f.type == FieldType::Asset && ImGui::BeginDragDropTarget()) {
          if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ACE_ASSET")) {
            *(std::string*)p = (const char*)payload->Data;
            changed = instant = true;
          }
          ImGui::EndDragDropTarget();
        }
        break;
      }
    }
    if (changed) dirty_ = true;
    if (instant || ImGui::IsItemDeactivatedAfterEdit()) finished = true;
    ImGui::PopID();
  }
  return finished;
}

// A small "v" button that lists the project files suited to this field (decided
// by the field's name), so paths rarely need typing.
bool Editor::assetPicker(const Field& field, std::string& value) {
  std::vector<const char*> extensions, builtins;
  if (strcmp(field.name, "mesh") == 0) {
    extensions = {".gltf", ".glb"};
    builtins = {"builtin:cube", "builtin:sphere", "builtin:plane", "builtin:quad"};
  } else if (strcmp(field.name, "path") == 0) {
    extensions = {".lua"};
  } else {
    extensions = {".png", ".jpg", ".jpeg"};
  }
  bool picked = false;
  if (ImGui::ArrowButton("##pick", ImGuiDir_Down)) ImGui::OpenPopup("pick_asset");
  if (ImGui::BeginPopup("pick_asset")) {
    if (ImGui::Selectable("(none)", value.empty())) {
      value.clear();
      picked = true;
    }
    for (const char* b : builtins)
      if (ImGui::Selectable(b, value == b)) {
        value = b;
        picked = true;
      }
    for (const std::string& file : assetFiles_) {
      bool match = false;
      for (const char* ext : extensions) match |= endsWith(file, ext);
      if (match && ImGui::Selectable(file.c_str(), value == file)) {
        value = file;
        picked = true;
      }
    }
    ImGui::EndPopup();
  }
  return picked;
}

void Editor::drawInspector() {
  ImGui::Begin("Inspector");
  if (selected_ == kNoEntity) {
    ImGui::TextDisabled("Select an entity in the Hierarchy or Viewport.");
    ImGui::End();
    return;
  }
  bool finished = false;
  if (!extraSelection_.empty())
    ImGui::TextDisabled("%zu more selected: the gizmo, delete, duplicate and copy apply to all", extraSelection_.size());
  ImGui::InputText("Name", &scene_.reg.get<Name>(selected_).value);
  testRect("field:Name");
  finished |= ImGui::IsItemDeactivatedAfterEdit();
  for (const ComponentType& type : componentTypes()) {
    void* component = type.get(scene_.reg, selected_);
    if (!component) continue;
    bool keep = true;
    bool removable = strcmp(type.name, "Transform") != 0;
    if (ImGui::CollapsingHeader(type.name, removable ? &keep : nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::PushID(type.name);
      finished |= editFields(component, type.fields);
      if (strcmp(type.name, "Script") == 0) finished |= drawScriptButtons(*(Script*)component);
      ImGui::PopID();
    }
    if (!keep) {
      type.remove(scene_.reg, selected_);
      finished = true;
    }
  }
  ImGui::Spacing();
  if (ImGui::Button("Add Component", ImVec2(-1, 0))) ImGui::OpenPopup("add_component");
  if (ImGui::BeginPopup("add_component")) {
    for (const ComponentType& type : componentTypes()) {
      if (type.get(scene_.reg, selected_)) continue;
      if (ImGui::MenuItem(type.name)) {
        type.add(scene_.reg, selected_);
        finished = true;
      }
    }
    ImGui::EndPopup();
  }
  if (finished && !playing_) commit();
  ImGui::End();
}

void Editor::drawSettings() {
  ImGui::Begin("Render Settings");
  ImGui::TextDisabled("Saved with the scene. Hover a setting for what it does.");
  RenderSettings& rs = scene_.settings;
  struct Item {
    const char* name;
    const char* tip;
  };
  struct Group {
    const char* title;
    std::vector<Item> items;
  };
  // Every RenderSettings field, grouped and explained. Fields missing here still
  // show up under "Other", so adding a setting never hides it.
  static const std::vector<Group> groups = {
      {"Pixels",
       {{"worldPixels", "Pixels belong to the 3D world (texel splatting): they stay put when a perspective camera turns or moves. "
                        "Orthographic cameras use screen pixels instead."},
        {"probeResolution", "World pixels: pixels per cube-map face (90 degrees of view). Lower = bigger, chunkier pixels."},
        {"probeCell", "World pixels: how far (world units) the camera moves before the pixel grid re-centres, with a short dissolve."},
        {"pixelArt", "Screen pixels: render a low-resolution image and scale it up with hard edges."},
        {"pixelHeight", "Screen pixels: height of that image in pixels. Also sets the size of on-screen text in every mode."},
        {"integerScale", "Screen pixels: make every game pixel the same whole number of screen pixels (adjusts the height slightly)."},
        {"pixelSnap", "Screen pixels with an orthographic camera: snap the camera to the pixel grid so nothing shimmers."}}},
      {"Shading",
       {{"toonBands", "Light comes in this many hard steps (0 = smooth)."},
        {"dither", "Ordered dithering between light steps and palette colors (0 = none)."},
        {"texelLighting", "Light and shadow follow the texture's pixel grid instead of the screen's."},
        {"texelsPerUnit", "Texture pixels per world unit, for meshes with worldTexels ticked."},
        {"shadows", "Sun shadows (raster). Path tracing always has shadows."},
        {"outlines", "Dark outlines on silhouettes."},
        {"outlineStrength", "How dark outlines are (0..1)."},
        {"highlightStrength", "Light edge on convex creases (0 = off)."}}},
      {"Palette",
       {{"palette", "Image whose colors are the only ones allowed on screen (up to 64). Edit it in the Palette tab."},
        {"rampShading", "Shade by stepping along the palette's rows (color ramps) instead of darkening colors."},
        {"rampShadowSteps", "How many ramp colors darker a surface gets in full shadow."}}},
      {"Sky and light",
       {{"ambient", "Light that reaches everything, including shadows."},
        {"skyTop", "Sky color overhead."},
        {"skyBottom", "Sky color at the horizon."},
        {"sunSize", "Size of the sun disc in the sky, in degrees (0 = none). The sun is the first Light without 'point'."},
        {"sunGlow", "Glow around the sun, in the sky and in the haze."},
        {"fogDensity", "Distance haze: how quickly far things fade into the haze color (0 = none, 0.02 = gone after ~50 units)."},
        {"fogColor", "Color of the haze. Usually close to skyBottom so the horizon blends."},
        {"cloudCoverage", "Pixel clouds: 0 = clear sky, 1 = overcast."},
        {"cloudScale", "Cloud size: larger numbers = smaller clouds."},
        {"cloudSpeed", "How fast the clouds drift."},
        {"stars", "Stars where the sky is dark (0..1)."},
        {"exposure", "Overall brightness before colors are snapped to the palette."}}},
      {"Path tracing",
       {{"pathTraced", "Trace light rays: soft shadows, bounce light, glowing objects light their surroundings. Slower. "
                       "Works with world pixels (the image settles while the camera is still)."},
        {"ptSamples", "Rays per pixel per frame (screen pixels). More = less noise, slower."},
        {"ptBounces", "How many times light bounces. More = brighter, softer indirect light, slower."},
        {"denoise", "Smooth the path-traced light while there are few samples (textures stay sharp). The smoothing fades as the image settles."}}},
  };
  // Settings that do nothing with the current choices are greyed out.
  auto active = [&](const std::string& n) {
    bool world = rs.worldPixels;
    if (n == "probeResolution" || n == "probeCell") return world;
    if (n == "pixelHeight" || n == "integerScale") return rs.pixelArt || n == "pixelHeight";
    if (n == "pixelSnap") return rs.pixelArt;
    if (n == "rampShading" || n == "rampShadowSteps") return !rs.palette.empty() && (n == "rampShading" || rs.rampShading);
    if (n == "outlineStrength") return rs.outlines;
    if (n == "ptSamples" || n == "ptBounces" || n == "denoise") return rs.pathTraced;
    if (n == "shadows") return !rs.pathTraced;
    return true;
  };
  const std::vector<Field>& fields = renderSettingsFields();
  std::vector<bool> shown(fields.size(), false);
  bool finished = false;
  auto drawField = [&](size_t i, const char* tip) {
    shown[i] = true;
    ImGui::BeginDisabled(!active(fields[i].name));
    finished |= editFields(&rs, {fields[i]});
    ImGui::EndDisabled();
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
  };
  for (const Group& g : groups) {
    if (!ImGui::CollapsingHeader(g.title, ImGuiTreeNodeFlags_DefaultOpen)) {
      for (const Item& it : g.items)
        for (size_t i = 0; i < fields.size(); i++)
          if (it.name == std::string(fields[i].name)) shown[i] = true;
      continue;
    }
    for (const Item& it : g.items)
      for (size_t i = 0; i < fields.size(); i++)
        if (it.name == std::string(fields[i].name)) drawField(i, it.tip);
  }
  bool other = false;
  for (size_t i = 0; i < fields.size(); i++) {
    if (shown[i]) continue;
    if (!other) other = ImGui::CollapsingHeader("Other", ImGuiTreeNodeFlags_DefaultOpen);
    if (other) drawField(i, nullptr);
  }
  if (finished && !playing_) commit();
  ImGui::End();
}

// ------------------------------------------------------------- palette

// The palette image as rows of colors (each row is a ramp, darkest first).
// Edits apply to the GPU texture at once; Save writes the PNG.
void Editor::loadPalette() {
  palettePath_ = scene_.settings.palette;
  paletteRows_.clear();
  paletteDirty_ = false;
  if (palettePath_.empty()) return;
  const Texture* t = assets_.texture(palettePath_);
  for (u32 y = 0; y < t->height; y++) {
    std::vector<vec4> row;
    for (u32 x = 0; x < t->width; x++) {
      const u8* p = &t->pixels[((size_t)y * t->width + x) * 4];
      if (p[3] < 128) break; // a transparent texel ends the ramp
      row.push_back(vec4(p[0], p[1], p[2], 255) / 255.0f);
    }
    paletteRows_.push_back(row);
  }
}

void Editor::applyPalette() {
  u32 width = 1;
  for (const auto& row : paletteRows_) width = std::max(width, (u32)row.size());
  u32 height = std::max<u32>(1, (u32)paletteRows_.size());
  std::vector<u8> pixels((size_t)width * height * 4, 0);
  for (u32 y = 0; y < paletteRows_.size(); y++)
    for (u32 x = 0; x < paletteRows_[y].size(); x++)
      for (int c = 0; c < 4; c++) pixels[((size_t)y * width + x) * 4 + c] = (u8)(glm::clamp(paletteRows_[y][x][c], 0.0f, 1.0f) * 255.0f + 0.5f);
  if (assets_.setTexturePixels(palettePath_, width, height, pixels.data())) renderer_.clearTextureCaches();
  paletteDirty_ = true;
}

bool Editor::savePalette() {
  const Texture* t = assets_.texture(palettePath_);
  if (!savePng(project_.path(palettePath_), t->width, t->height, t->pixels.data())) {
    ACE_ERROR("could not write %s", palettePath_.c_str());
    return false;
  }
  paletteDirty_ = false;
  ACE_INFO("saved %s", palettePath_.c_str());
  return true;
}

void Editor::createPalette() {
  // A new file (never overwrite an existing image), with four starter ramps.
  std::string path = "assets/palette.png";
  for (int n = 2; std::filesystem::exists(project_.path(path)); n++) path = "assets/palette_" + std::to_string(n) + ".png";
  std::error_code ec;
  std::filesystem::create_directories(std::filesystem::path(project_.path(path)).parent_path(), ec);
  static const u8 ramps[4][5][3] = {
      {{31, 59, 58}, {44, 90, 63}, {63, 125, 67}, {98, 160, 74}, {156, 195, 90}},     // green
      {{58, 42, 51}, {90, 61, 58}, {125, 90, 68}, {166, 125, 85}, {207, 167, 111}},   // earth
      {{43, 42, 61}, {71, 74, 94}, {107, 112, 131}, {149, 154, 166}, {196, 200, 200}}, // stone
      {{61, 90, 156}, {91, 132, 196}, {134, 176, 223}, {184, 214, 238}, {230, 242, 247}}, // sky
  };
  std::vector<u8> pixels;
  for (auto& ramp : ramps)
    for (auto& c : ramp) pixels.insert(pixels.end(), {c[0], c[1], c[2], 255});
  if (!savePng(project_.path(path), 5, 4, pixels.data())) {
    ACE_ERROR("could not write %s", path.c_str());
    return;
  }
  scene_.settings.palette = path;
  commit();
  loadPalette();
  assetsRefreshed_ = -10.0;
}

void Editor::drawPalette() {
  // A saved layout (imgui.ini) from before this panel existed has no place for
  // it; put it with the Inspector the first time instead of floating.
  if (ImGuiWindow* inspector = ImGui::FindWindowByName("Inspector"); inspector && inspector->DockId)
    ImGui::SetNextWindowDockID(inspector->DockId, ImGuiCond_FirstUseEver);
  ImGui::Begin("Palette");
  if (scene_.settings.palette != palettePath_) loadPalette();
  if (palettePath_.empty()) {
    ImGui::TextWrapped("This scene has no palette. With one, every color on screen snaps to it, and "
                       "ramp shading steps along its rows.");
    if (ImGui::Button("Create palette")) createPalette();
    ImGui::End();
    return;
  }
  ImGui::Text("%s%s", palettePath_.c_str(), paletteDirty_ ? " (unsaved)" : "");
  ImGui::TextDisabled("Each row is a ramp, darkest on the left. Click a swatch to edit it.");
  bool changed = false;
  int removeRow = -1;
  for (int r = 0; r < (int)paletteRows_.size(); r++) {
    ImGui::PushID(r);
    auto& row = paletteRows_[r];
    for (int c = 0; c < (int)row.size(); c++) {
      ImGui::PushID(c);
      if (c > 0) ImGui::SameLine(0.0f, 2.0f);
      changed |= ImGui::ColorEdit3("##c", &row[c].x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
      ImGui::PopID();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("+")) {
      row.push_back(row.empty() ? vec4(0.5f, 0.5f, 0.5f, 1.0f) : row.back());
      changed = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("-") && !row.empty()) {
      row.pop_back();
      changed = true;
      if (row.empty()) removeRow = r;
    }
    ImGui::PopID();
  }
  if (removeRow >= 0) paletteRows_.erase(paletteRows_.begin() + removeRow);
  if (ImGui::Button("Add ramp")) {
    paletteRows_.push_back({vec4(0.2f, 0.2f, 0.25f, 1), vec4(0.45f, 0.45f, 0.5f, 1), vec4(0.75f, 0.75f, 0.78f, 1)});
    changed = true;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!paletteDirty_ && !changed);
  if (ImGui::Button("Save")) savePalette();
  ImGui::SameLine();
  if (ImGui::Button("Revert")) {
    assets_.reload(); // re-read every file, including the palette
    renderer_.clearTextureCaches();
    loadPalette();
  }
  ImGui::EndDisabled();
  if (changed) applyPalette();
  ImGui::End();
}

void Editor::drawAssets() {
  ImGui::Begin("Assets");
  static std::string filter;
  ImGui::SetNextItemWidth(220);
  ImGui::InputTextWithHint("##filter", "filter", &filter);
  ImGui::SameLine();
  ImGui::TextDisabled("drag onto mesh / texture / script fields; double-click a scene to open it or a prefab to place it");
  ImGui::BeginChild("files");
  for (const std::string& f : assetFiles_) {
    if (!filter.empty() && f.find(filter) == std::string::npos) continue;
    ImGui::Selectable(f.c_str(), f == scenePath_);
    testRect("asset:" + f);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      if (endsWith(f, ".scene.json")) openScene(f);
      else if (endsWith(f, ".prefab.json") && !playing_) placePrefab(f);
    }
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("ACE_ASSET", f.c_str(), f.size() + 1);
      ImGui::TextUnformatted(f.c_str());
      ImGui::EndDragDropSource();
    }
  }
  ImGui::EndChild();
  ImGui::End();
}

void Editor::drawConsole() {
  ImGui::Begin("Console");
  if (ImGui::SmallButton("Clear")) console_.clear();
  ImGui::Separator();
  ImGui::BeginChild("lines");
  static const ImVec4 colors[] = {{0.82f, 0.82f, 0.82f, 1}, {1.0f, 0.8f, 0.3f, 1}, {1.0f, 0.4f, 0.4f, 1}};
  for (auto& [level, line] : console_) ImGui::TextColored(colors[(int)level], "%s", line.c_str());
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
  ImGui::End();
}

bool Editor::raycast(vec2 uv, Entity& entity, vec3& point, vec3& normal) {
  CameraView cv = camera_.view();
  mat4 invViewProj = glm::inverse(cv.camera.projection((f32)viewportW_ / (f32)viewportH_) * glm::inverse(cv.world));
  vec2 ndc{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f};
  vec4 nearP = invViewProj * vec4(ndc, 0.0f, 1.0f), farP = invViewProj * vec4(ndc, 1.0f, 1.0f);
  vec3 origin = vec3(nearP) / nearP.w, end = vec3(farP) / farP.w;
  entity = kNoEntity;
  f32 bestT = 2.0f; // t is in 0..1 along origin->end
  // Tests against mesh bounds, not triangles.
  // shortcut: exact for boxes and planes (what levels are built from); loose for
  // spheres and imported models. Trace triangles if picking them feels sloppy.
  auto test = [&](Entity e, const mat4& world, Aabb box) {
    mat4 inv = glm::inverse(world);
    vec3 o = vec3(inv * vec4(origin, 1.0f)), d = vec3(inv * vec4(end, 1.0f)) - o;
    vec3 pad = glm::max(vec3(0.02f) - (box.max - box.min), vec3(0.0f)) * 0.5f; // give flat meshes some thickness
    f32 t0 = 0.0f, t1 = 1.0f;
    int entryAxis = 1;
    for (int a = 0; a < 3; a++) {
      f32 lo = (box.min[a] - pad[a] - o[a]) / d[a], hi = (box.max[a] + pad[a] - o[a]) / d[a];
      if (lo > hi) std::swap(lo, hi);
      if (lo > t0) {
        t0 = lo;
        entryAxis = a;
      }
      t1 = std::min(t1, hi);
    }
    if (t0 > t1 || t0 >= bestT) return;
    bestT = t0;
    entity = e;
    vec3 local(0.0f);
    local[entryAxis] = d[entryAxis] > 0.0f ? -1.0f : 1.0f; // the face the ray entered through
    normal = glm::normalize(glm::transpose(mat3(inv)) * local);
    point = origin + (end - origin) * t0;
  };
  for (auto [e, t, m] : scene_.reg.view<Transform, MeshRenderer>().each()) test(e, t.world, assets_.mesh(m.mesh)->bounds);
  for (auto [e, t, s] : scene_.reg.view<Transform, SpriteRenderer>().each())
    test(e, t.world, Aabb{vec3(-s.size * 0.5f, 0.0f), vec3(s.size * 0.5f, 0.0f)});
  return entity != kNoEntity;
}

Entity Editor::pick(vec2 uv) {
  Entity e;
  vec3 point, normal;
  raycast(uv, e, point, normal);
  return e;
}

// Centre of the block a click at `uv` would create: against the clicked face,
// aligned to a grid whose cell is the brush size.
bool Editor::blockPlacement(vec2 uv, vec3& center) {
  Entity hit;
  vec3 point, normal;
  if (!raycast(uv, hit, point, normal)) return false;
  vec3 size = glm::max(brushSize_, vec3(0.0625f));
  vec3 a = glm::abs(normal);
  int axis = a.x > a.y ? (a.x > a.z ? 0 : 2) : (a.y > a.z ? 1 : 2);
  for (int i = 0; i < 3; i++) {
    if (i == axis) {
      f32 face = point[i] + (normal[i] > 0.0f ? 0.0f : -size[i]); // low side of the new block
      center[i] = roundf(face / size[i]) * size[i] + size[i] * 0.5f;
    } else {
      center[i] = floorf(point[i] / size[i] + 1e-4f) * size[i] + size[i] * 0.5f;
    }
  }
  return true;
}

void Editor::buildModeClick(vec2 uv, bool erase, ImVec2, ImVec2) {
  if (erase) {
    Entity hit;
    vec3 point, normal;
    // Only blocks made by this tool can be erased with it, so a stray click
    // cannot delete the ground or hand-placed entities.
    if (raycast(uv, hit, point, normal) && scene_.reg.get<Name>(hit).value == "Block") {
      scene_.destroy(hit);
      commit();
    }
    return;
  }
  vec3 center;
  if (!blockPlacement(uv, center)) return;
  Entity root = scene_.find("Blocks");
  if (root == kNoEntity) root = scene_.create("Blocks");
  Entity e = scene_.create("Block", root);
  Transform& t = scene_.reg.get<Transform>(e);
  t.position = center - vec3(scene_.reg.get<Transform>(root).world[3]);
  t.scale = glm::max(brushSize_, vec3(0.0625f));
  MeshRenderer& m = scene_.reg.emplace<MeshRenderer>(e);
  m.texture = brushTexture_;
  m.worldTexels = true;
  scene_.reg.emplace<Collider>(e);
  commit();
}

void Editor::drawToolbar() {
  if (ImGui::Button(playing_ ? "Stop (F5)" : "Play (F5)")) setPlaying(!playing_);
  ImGui::SameLine();
  ImGui::BeginDisabled(playing_);
  ImGui::RadioButton("Move (W)", &gizmoOp_, 0);
  ImGui::SameLine();
  ImGui::RadioButton("Rotate (E)", &gizmoOp_, 1);
  ImGui::SameLine();
  ImGui::RadioButton("Scale (R)", &gizmoOp_, 2);
  ImGui::SameLine();
  ImGui::Checkbox("Snap", &snap_);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(70);
  ImGui::DragFloat("##snapstep", &snapMove_, 0.0125f, 0.0125f, 10.0f, "%.4g");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Move step in world units (rotate snaps to 15 degrees, scale to 0.25)");
  ImGui::SameLine();
  ImGui::Checkbox("Orthographic", &camera_.orthographic);
  ImGui::SameLine();
  ImGui::Checkbox("Build", &buildMode_);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Click a surface to add a block there; Shift+click removes a block");
  if (buildMode_) {
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    if (ImGui::BeginCombo("##brush", brushTexture_.empty() ? "(no texture)" : brushTexture_.c_str())) {
      if (ImGui::Selectable("(no texture)", brushTexture_.empty())) brushTexture_.clear();
      for (const std::string& f : assetFiles_)
        if ((endsWith(f, ".png") || endsWith(f, ".jpg")) && ImGui::Selectable(f.c_str(), f == brushTexture_)) brushTexture_ = f;
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130);
    ImGui::DragFloat3("##brushsize", &brushSize_.x, 0.0625f, 0.0625f, 64.0f, "%.4g");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Block size (also the placement grid)");
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  ImGui::TextDisabled("%s | %.0f fps", rhi::backendName(app_.device->backend()), ImGui::GetIO().Framerate);
}

void Editor::drawViewport() {
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
  ImGui::Begin("Viewport");
  drawToolbar();
  ImVec2 size = ImGui::GetContentRegionAvail();
  ImVec2 scale = ImGui::GetIO().DisplayFramebufferScale;
  wantedW_ = (u32)std::max(size.x * scale.x, 16.0f);
  wantedH_ = (u32)std::max(size.y * scale.y, 16.0f);
  ImVec2 pos = ImGui::GetCursorScreenPos();
  viewPos_ = pos;
  viewSize_ = size;
  if (viewportTexture_ && size.x > 1 && size.y > 1) {
    ImGui::Image(ImGuiRhi::textureId(viewportTexture_), size);
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    if (!playing_) {
      // Camera: right-drag orbits, middle-drag (or Shift+right-drag) pans, wheel zooms.
      if (hovered && io.MouseWheel != 0.0f) {
        f32 zoom = powf(0.9f, io.MouseWheel);
        camera_.orthoSize = glm::clamp(camera_.orthoSize * zoom, 0.25f, 200.0f);
        camera_.distance = glm::clamp(camera_.distance * zoom, 0.5f, 400.0f);
      }
      bool panning = ImGui::IsMouseDragging(ImGuiMouseButton_Middle) || (ImGui::IsMouseDragging(ImGuiMouseButton_Right) && io.KeyShift);
      if (hovered && panning) {
        mat4 world = camera_.view().world;
        f32 unitsPerPixel = (camera_.orthographic ? 2.0f * camera_.orthoSize : camera_.distance) / size.y;
        camera_.target -= (vec3(world[0]) * io.MouseDelta.x - vec3(world[1]) * io.MouseDelta.y) * unitsPerPixel;
      } else if (hovered && ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
        camera_.yaw -= io.MouseDelta.x * 0.3f;
        camera_.pitch = glm::clamp(camera_.pitch - io.MouseDelta.y * 0.3f, -89.0f, 89.0f);
      }

      vec2 mouseUv((io.MousePos.x - pos.x) / size.x, (io.MousePos.y - pos.y) / size.y);
      auto wireBox = [&](const Aabb& box, ImU32 color) {
        CameraView cv = camera_.view();
        mat4 viewProj = cv.camera.projection(size.x / size.y) * glm::inverse(cv.world);
        ImVec2 corner[8];
        for (int c = 0; c < 8; c++) {
          vec4 clip = viewProj * vec4(c & 1 ? box.max.x : box.min.x, c & 2 ? box.max.y : box.min.y, c & 4 ? box.max.z : box.min.z, 1.0f);
          if (clip.w <= 0.001f) return; // partly behind the camera: skip rather than draw garbage
          corner[c] = ImVec2(pos.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - clip.y / clip.w * 0.5f) * size.y);
        }
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
        for (int a = 0; a < 8; a++)
          for (int bit = 1; bit < 8; bit <<= 1)
            if (!(a & bit)) dl->AddLine(corner[a], corner[a | bit], color, 1.5f);
        dl->PopClipRect();
      };
      if (buildMode_) {
        vec3 center;
        vec3 half = glm::max(brushSize_, vec3(0.0625f)) * 0.5f;
        if (hovered && !io.KeyShift && blockPlacement(mouseUv, center)) wireBox(Aabb{center - half, center + half}, IM_COL32(255, 230, 90, 255));
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) buildModeClick(mouseUv, io.KeyShift, pos, size);
      }
      bool gizmoActive = false;
      if (selected_ != kNoEntity && !buildMode_) {
        CameraView cv = camera_.view();
        mat4 view = glm::inverse(cv.world);
        mat4 proj = cv.camera.projection(size.x / size.y);
        ImGuizmo::SetOrthographic(camera_.orthographic);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
        Transform& t = scene_.reg.get<Transform>(selected_);
        mat4 world = t.world;
        static const ImGuizmo::OPERATION ops[] = {ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE};
        f32 step = gizmoOp_ == 0 ? std::max(snapMove_, 0.0001f) : gizmoOp_ == 1 ? 15.0f : 0.25f;
        f32 snapValues[3] = {step, step, step};
        // Collider outline of the selection, so blocking volumes can be lined up by eye.
        for (Entity e : extraSelection_) { // outline the rest of a multi-selection
          Aabb b = scene_.colliderBounds(e);
          if (b.min.x > b.max.x) {
            const MeshRenderer* m = scene_.reg.try_get<MeshRenderer>(e);
            if (!m) continue;
            const Aabb& local = assets_.mesh(m->mesh)->bounds;
            const mat4& w = scene_.reg.get<Transform>(e).world;
            for (int c = 0; c < 8; c++) b.grow(vec3(w * vec4(c & 1 ? local.max.x : local.min.x, c & 2 ? local.max.y : local.min.y, c & 4 ? local.max.z : local.min.z, 1.0f)));
          }
          wireBox(b, IM_COL32(255, 200, 80, 200));
        }
        Aabb box = scene_.colliderBounds(selected_);
        if (box.min.x <= box.max.x) wireBox(box, IM_COL32(90, 255, 140, 220));
        if (ImGuizmo::Manipulate(&view[0][0], &proj[0][0], ops[gizmoOp_], gizmoOp_ == 2 ? ImGuizmo::LOCAL : ImGuizmo::WORLD, &world[0][0],
                                 nullptr, snap_ ? snapValues : nullptr)) {
          // The same world-space change, applied to every selected root.
          mat4 delta = world * glm::inverse(t.world);
          for (Entity e : selectionRoots()) {
            Transform& et = scene_.reg.get<Transform>(e);
            mat4 parentWorld = scene_.reg.valid(et.parent) ? scene_.reg.get<Transform>(et.parent).world : mat4(1.0f);
            mat4 target = e == selected_ ? world : delta * et.world;
            vec3 skew;
            vec4 persp;
            glm::decompose(glm::inverse(parentWorld) * target, et.scale, et.rotation, et.position, skew, persp);
          }
          scene_.updateTransforms();
          dirty_ = true;
        }
        gizmoActive = ImGuizmo::IsUsing();
        if (gizmoWasUsing_ && !gizmoActive) commit(); // one undo step per drag
        gizmoWasUsing_ = gizmoActive;
      }
      // Icons for lights and cameras, which have nothing to click on otherwise.
      Entity iconHit = kNoEntity;
      {
        CameraView cv = camera_.view();
        mat4 viewProj = cv.camera.projection(size.x / size.y) * glm::inverse(cv.world);
        auto toScreen = [&](vec3 p, ImVec2& out) {
          vec4 clip = viewProj * vec4(p, 1.0f);
          if (clip.w <= 0.001f) return false;
          out = ImVec2(pos.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - clip.y / clip.w * 0.5f) * size.y);
          return out.x >= pos.x && out.y >= pos.y && out.x <= pos.x + size.x && out.y <= pos.y + size.y;
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
        const f32 radius = 9.0f;
        for (auto [e, t] : scene_.reg.view<Transform>().each()) {
          const Light* light = scene_.reg.try_get<Light>(e);
          bool camera = scene_.reg.all_of<Camera>(e);
          if (!light && !camera) continue;
          bool icon = !scene_.reg.any_of<MeshRenderer, SpriteRenderer>(e); // meshes are clickable already
          ImVec2 c;
          bool onScreen = toScreen(vec3(t.world[3]), c);
          ImU32 ink = isSelected(e) ? IM_COL32(90, 255, 140, 255) : IM_COL32(255, 255, 255, 220);
          if (icon && onScreen) dl->AddCircleFilled(c, radius, IM_COL32(20, 22, 30, 200));
          if (!icon || !onScreen) {
          } else if (camera) {
            dl->AddRect(ImVec2(c.x - 5, c.y - 3), ImVec2(c.x + 3, c.y + 4), ink, 0.0f, 0, 1.5f);
            dl->AddTriangleFilled(ImVec2(c.x + 3, c.y), ImVec2(c.x + 7, c.y - 3), ImVec2(c.x + 7, c.y + 4), ink);
          } else if (light->point) {
            dl->AddCircleFilled(c, 4.0f, ink);
          } else { // sun: a disc with rays, plus a line showing where it shines
            dl->AddCircle(c, 4.0f, ink, 0, 1.5f);
            for (int k = 0; k < 8; k++) {
              f32 a = k * glm::quarter_pi<f32>();
              dl->AddLine(ImVec2(c.x + cosf(a) * 6, c.y + sinf(a) * 6), ImVec2(c.x + cosf(a) * 8, c.y + sinf(a) * 8), ink, 1.5f);
            }
            ImVec2 tip;
            if (toScreen(vec3(t.world[3]) - glm::normalize(vec3(t.world[2])) * 2.0f, tip)) dl->AddLine(c, tip, ink, 1.0f);
          }
          if (light && light->point && isSelected(e)) { // show the reach of a selected lamp
            vec3 p = vec3(t.world[3]);
            for (int axis = 0; axis < 3; axis++) {
              ImVec2 prev;
              bool havePrev = false;
              for (int k = 0; k <= 48; k++) {
                f32 a = glm::two_pi<f32>() * k / 48.0f;
                vec3 o(0.0f);
                o[(axis + 1) % 3] = cosf(a) * light->range;
                o[(axis + 2) % 3] = sinf(a) * light->range;
                ImVec2 q;
                vec4 clip = viewProj * vec4(p + o, 1.0f);
                bool ok = clip.w > 0.001f;
                if (ok) q = ImVec2(pos.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - clip.y / clip.w * 0.5f) * size.y);
                if (ok && havePrev) dl->AddLine(prev, q, IM_COL32(255, 210, 120, 160), 1.0f);
                prev = q;
                havePrev = ok;
              }
            }
          }
          ImVec2 m = io.MousePos;
          if (icon && onScreen && (m.x - c.x) * (m.x - c.x) + (m.y - c.y) * (m.y - c.y) <= radius * radius) iconHit = e;
        }
        dl->PopClipRect();
      }
      if (!buildMode_ && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoActive && !(selected_ != kNoEntity && ImGuizmo::IsOver()))
      {
        Entity hit = iconHit != kNoEntity ? iconHit : pick(mouseUv); // icons sit on top of the scene
        if (io.KeyShift && hit != kNoEntity) toggleSelected(hit); // Shift+click adds to / removes from the selection
        else selected_ = hit;
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

// ---------------------------------------------------------------- autotest

// Drives the editor like a user would (--autotest). Each step runs a few frames
// after the previous one so ImGui has processed the input. Run it on a copy of
// the demo project: it saves the scene.
void Editor::testRect(const std::string& id) {
  if (autotest) testRects_[id] = {ImGui::GetItemRectMin(), ImGui::GetItemRectMax()};
}

void Editor::autotestStep() {
  static const int kFramesPerStep = 4;
  static vec3 before;
  static f32 yawBefore, zoomBefore;
  static size_t undoBefore;
  static ImVec2 grab, dragDir, mouse(-1, -1);
  ImGuiIO& io = ImGui::GetIO();
  // Re-assert the scripted pointer every frame: when the window has focus the
  // SDL backend reports the real mouse position each frame.
  if (mouse.x >= 0) io.AddMousePosEvent(mouse.x, mouse.y);
  auto moveTo = [&](f32 x, f32 y) {
    mouse = ImVec2(x, y);
    io.AddMousePosEvent(x, y);
  };
  int frame = autotestFrame_++;
  if (autotestResult_ != 0 || frame % kFramesPerStep != 0) return;
  int step = frame / kFramesPerStep - 3; // let the layout settle first
  if (step < 0) return;

  auto expect = [&](bool ok, const char* what) {
    if (ok || autotestResult_ != 0) return;
    ACE_ERROR("autotest FAILED at step %d: %s", step, what);
    autotestResult_ = -1;
  };
  auto center = [&] { return ImVec2(viewPos_.x + viewSize_.x * 0.5f, viewPos_.y + viewSize_.y * 0.5f); };
  auto toScreen = [&](vec3 world) {
    CameraView cv = camera_.view();
    vec4 clip = cv.camera.projection(viewSize_.x / viewSize_.y) * glm::inverse(cv.world) * vec4(world, 1.0f);
    return ImVec2(viewPos_.x + (clip.x / clip.w * 0.5f + 0.5f) * viewSize_.x, viewPos_.y + (0.5f - clip.y / clip.w * 0.5f) * viewSize_.y);
  };
  auto key = [&](ImGuiKey k, bool ctrl = false, bool shift = false) { // press + release; ImGui spreads them over frames
    // On macOS ImGui maps the physical Cmd key to "Ctrl" as events are added, so
    // send what a real keyboard would: Cmd there, Ctrl elsewhere.
    ImGuiKey mod = io.ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
    if (ctrl) io.AddKeyEvent(mod, true);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, true);
    io.AddKeyEvent(k, true);
    io.AddKeyEvent(k, false);
    if (shift) io.AddKeyEvent(ImGuiMod_Shift, false);
    if (ctrl) io.AddKeyEvent(mod, false);
  };
  auto positionOf = [&](const char* name) {
    Entity e = scene_.find(name);
    return e == kNoEntity ? vec3(1e9f) : scene_.reg.get<Transform>(e).position;
  };
  Entity player = scene_.find("Player");
  auto rectCenter = [&](const char* id) { // a widget recorded by testRect
    auto it = testRects_.find(id);
    expect(it != testRects_.end(), id);
    return it == testRects_.end() ? ImVec2(0, 0) : ImVec2((it->second.min.x + it->second.max.x) * 0.5f, (it->second.min.y + it->second.max.y) * 0.5f);
  };

  switch (step) {
    // --- camera: orbit with a right-drag, zoom with the wheel
    case 0:
      expect(player != kNoEntity && scene_.find("Orb") != kNoEntity && viewSize_.x > 100, "needs the demo scene and a visible viewport");
      yawBefore = camera_.yaw;
      zoomBefore = camera_.orthoSize;
      moveTo(center().x, center().y);
      break;
    case 1: io.AddMouseButtonEvent(ImGuiMouseButton_Right, true); break;
    case 2: moveTo(center().x + 60, center().y); break;
    case 3: moveTo(center().x + 120, center().y); break;
    case 4: io.AddMouseButtonEvent(ImGuiMouseButton_Right, false); break;
    case 5:
      expect(fabsf(camera_.yaw - yawBefore) > 5.0f, "right-drag should orbit the camera");
      camera_.yaw = yawBefore;
      io.AddMouseWheelEvent(0.0f, 2.0f);
      break;
    case 6: expect(camera_.orthoSize < zoomBefore, "mouse wheel should zoom in"); break;
    // --- selection: click an entity in the viewport
    case 7: {
      selected_ = kNoEntity;
      ImVec2 p = toScreen(vec3(scene_.reg.get<Transform>(player).world[3]));
      moveTo(p.x, p.y);
      break;
    }
    case 8:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 9: expect(selected_ == player, "clicking the Player in the viewport should select it"); break;
    // --- gizmo: drag the X axis handle, expect one undo step, then undo/redo
    case 10: {
      before = positionOf("Player");
      undoBefore = undoStack_.size();
      vec3 origin = vec3(scene_.reg.get<Transform>(player).world[3]);
      ImVec2 a = toScreen(origin), b = toScreen(origin + vec3(1, 0, 0));
      f32 len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
      dragDir = ImVec2((b.x - a.x) / len, (b.y - a.y) / len);
      grab = ImVec2(a.x + dragDir.x * 45.0f, a.y + dragDir.y * 45.0f); // on the axis, clear of the centre handle
      moveTo(grab.x, grab.y);
      break;
    }
    case 11: io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); break;
    case 12: moveTo(grab.x + dragDir.x * 30.0f, grab.y + dragDir.y * 30.0f); break;
    case 13: moveTo(grab.x + dragDir.x * 60.0f, grab.y + dragDir.y * 60.0f); break;
    case 14: io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); break;
    case 15: {
      vec3 now = positionOf("Player");
      expect(now.x > before.x + 0.2f, "dragging the gizmo X handle should move the entity along +X");
      expect(fabsf(now.y - before.y) < 1e-3f && fabsf(now.z - before.z) < 1e-3f, "an X-axis drag must not change Y or Z");
      expect(undoStack_.size() == undoBefore + 1, "one gizmo drag should record exactly one undo step");
      expect(dirty_, "an edit should mark the scene as modified");
      moveTo(viewPos_.x + 5, viewPos_.y + 5);
      key(ImGuiKey_Z, true);
      break;
    }
    case 16:
      expect(glm::distance(positionOf("Player"), before) < 1e-4f, "Ctrl+Z should undo the move");
      key(ImGuiKey_Z, true, true);
      break;
    case 17:
      expect(positionOf("Player").x > before.x + 0.2f, "Ctrl+Shift+Z should redo the move");
      expect(selected_ != kNoEntity && selected_ == scene_.find("Player"), "selection should survive undo/redo");
      key(ImGuiKey_D, true);
      break;
    // --- duplicate, delete, undo the delete
    case 18:
      expect(scene_.find("Player copy") != kNoEntity && selected_ == scene_.find("Player copy"), "Ctrl+D should duplicate and select the copy");
      key(ImGuiKey_Delete);
      break;
    case 19:
      expect(scene_.find("Player copy") == kNoEntity, "Delete should remove the selected entity");
      key(ImGuiKey_Z, true);
      break;
    case 20:
      expect(scene_.find("Player copy") != kNoEntity, "Ctrl+Z should bring the deleted entity back");
      key(ImGuiKey_S, true);
      break;
    // --- save, then play/stop must restore the edited scene exactly
    case 21: {
      expect(!dirty_, "Ctrl+S should save and clear the modified flag");
      Scene onDisk;
      expect(onDisk.load(project_.path(scenePath_)) && onDisk.find("Player copy") != kNoEntity, "the saved file should contain the edits");
      before = positionOf("Orb");
      key(ImGuiKey_F5);
      break;
    }
    case 22: break; // let the scripts run a few frames
    case 23:
      expect(playing_, "F5 should start play mode");
      expect(glm::distance(positionOf("Orb"), before) > 1e-4f, "the Orb's script should move it while playing");
      key(ImGuiKey_F5);
      break;
    case 24:
      expect(!playing_, "F5 should stop play mode");
      expect(glm::distance(positionOf("Orb"), before) < 1e-5f, "stopping should restore the scene");
      // --- build mode: click open ground to place a block, shift-click to remove it
      buildMode_ = true;
      brushSize_ = vec3(1.0f);
      before = vec3(-4.0f, 0.0f, 4.0f); // inside the cell whose centre is (-3.5, 0.5, 4.5)
      {
        ImVec2 p = toScreen(before + vec3(0.5f, 0.0f, 0.5f));
        moveTo(p.x, p.y);
      }
      break;
    case 25:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 26: {
      Entity block = scene_.find("Block");
      expect(block != kNoEntity, "clicking the ground in build mode should create a Block");
      if (block != kNoEntity) {
        expect(glm::distance(vec3(scene_.reg.get<Transform>(block).world[3]), vec3(-3.5f, 0.5f, 4.5f)) < 1e-3f,
               "the block should sit on the ground, centred in its grid cell");
        expect(scene_.reg.all_of<Collider>(block) && scene_.reg.get<MeshRenderer>(block).worldTexels, "blocks are solid and world-mapped");
        ImVec2 p = toScreen(vec3(-3.5f, 1.0f, 4.5f)); // top face of the new block
        moveTo(p.x, p.y);
      }
      io.AddKeyEvent(ImGuiMod_Shift, true);
      break;
    }
    case 27:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 28:
      io.AddKeyEvent(ImGuiMod_Shift, false);
      expect(scene_.find("Block") == kNoEntity, "shift-click should remove the block");
      expect(scene_.find("Ground") != kNoEntity, "erasing must never remove non-block entities");
      buildMode_ = false;
      // --- prefabs: save the Tower (with its roof child) and place a copy
      {
        std::error_code ec;
        std::filesystem::create_directories(project_.path("prefabs"), ec);
        expect(scene_.savePrefab(scene_.find("Tower"), project_.path("prefabs/Tower.prefab.json")), "saving a prefab should work");
        size_t before = scene_.reg.view<Name>().size();
        Entity copy = placePrefab("prefabs/Tower.prefab.json");
        expect(copy != kNoEntity && selected_ == copy && scene_.children(copy).size() == 1, "placing a prefab copies the whole subtree and selects it");
        expect(scene_.reg.view<Name>().size() == before + 2, "a two-entity prefab adds two entities");
        undo();
        expect(scene_.reg.view<Name>().size() == before, "placing a prefab is one undo step");
      }
      // --- multi-select: Player is primary, Shift+click the Rock in the viewport
      selected_ = scene_.find("Player");
      lastPrimary_ = selected_;
      extraSelection_.clear();
      {
        ImVec2 p = toScreen(vec3(scene_.reg.get<Transform>(scene_.find("Rock")).world[3]));
        moveTo(p.x, p.y);
      }
      io.AddKeyEvent(ImGuiMod_Shift, true);
      break;
    case 29:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 30: {
      io.AddKeyEvent(ImGuiMod_Shift, false);
      expect(isSelected(scene_.find("Player")) && isSelected(scene_.find("Rock")) && extraSelection_.size() == 1,
             "Shift+click should add the Rock to the selection");
      char* saved = SDL_GetClipboardText(); // the test must not clobber the user's clipboard
      savedClipboard_ = saved ? saved : "";
      SDL_free(saved);
      entityCountBefore_ = scene_.reg.view<Name>().size();
      moveTo(viewPos_.x + 5, viewPos_.y + 5);
      key(ImGuiKey_C, true);
      break;
    }
    case 31: key(ImGuiKey_V, true); break;
    case 32: {
      size_t players = 0;
      for (auto [e, n] : scene_.reg.view<Name>().each()) players += n.value == "Player";
      expect(scene_.reg.view<Name>().size() == entityCountBefore_ + 2 && players == 2, "Ctrl+C, Ctrl+V should paste both entities");
      expect(extraSelection_.size() == 1 && scene_.reg.get<Name>(selected_).value != "", "the pasted entities become the selection");
      SDL_SetClipboardText(savedClipboard_.c_str());
      key(ImGuiKey_Delete);
      break;
    }
    case 33: {
      expect(scene_.reg.view<Name>().size() == entityCountBefore_, "Delete removes every selected entity");
      // Drag the gizmo with Player + Rock selected: both move by the same amount.
      selectAll({scene_.find("Player"), scene_.find("Rock")});
      before = positionOf("Player");
      groupBefore_ = positionOf("Rock");
      vec3 origin = vec3(scene_.reg.get<Transform>(scene_.find("Player")).world[3]);
      ImVec2 a = toScreen(origin), b = toScreen(origin + vec3(1, 0, 0));
      f32 len = sqrtf((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
      dragDir = ImVec2((b.x - a.x) / len, (b.y - a.y) / len);
      grab = ImVec2(a.x + dragDir.x * 45.0f, a.y + dragDir.y * 45.0f);
      moveTo(grab.x, grab.y);
      break;
    }
    case 34: io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); break;
    case 35: moveTo(grab.x + dragDir.x * 30.0f, grab.y + dragDir.y * 30.0f); break;
    case 36: moveTo(grab.x + dragDir.x * 60.0f, grab.y + dragDir.y * 60.0f); break;
    case 37: io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); break;
    case 38: {
      vec3 movedA = positionOf("Player") - before, movedB = positionOf("Rock") - groupBefore_;
      expect(movedA.x > 0.2f && glm::distance(movedA, movedB) < 1e-3f, "a gizmo drag moves every selected entity by the same amount");
      // --- palette: create one, edit a color (GPU texture updates live), save to disk
      {
        expect(scene_.settings.palette.empty(), "the test scene starts without a palette");
        createPalette();
        expect(!scene_.settings.palette.empty() && paletteRows_.size() == 4 && paletteRows_[0].size() == 5, "Create palette makes 4 ramps of 5");
        paletteRows_[2][1] = vec4(1.0f, 0.0f, 0.0f, 1.0f);
        paletteRows_[3].push_back(vec4(0.0f, 0.0f, 1.0f, 1.0f)); // makes the image 6 wide: new GPU texture
        applyPalette();
        const Texture* t = assets_.texture(scene_.settings.palette);
        expect(t->width == 6 && t->pixels[(2 * 6 + 1) * 4] == 255 && t->pixels[(2 * 6 + 1) * 4 + 1] == 0, "edits reach the texture");
        expect(t->pixels[(0 * 6 + 5) * 4 + 3] == 0, "shorter ramps are padded with transparent texels");
        expect(savePalette(), "Save writes the palette");
        std::string path = scene_.settings.palette;
        assets_.reload();
        renderer_.clearTextureCaches();
        loadPalette();
        expect(paletteRows_.size() == 4 && paletteRows_[3].size() == 6 && paletteRows_[2][1].r > 0.99f, "the saved file reads back with the edits");
        std::filesystem::remove(project_.path(path));
      }
      break;
    }
    case 39: { // --- light icons: lights have no mesh, but their icon selects them
      // Click on the Sun's icon (placed at its position on screen).
      Entity sun = scene_.find("Sun");
      selected_ = kNoEntity;
      ImVec2 p = toScreen(vec3(scene_.reg.get<Transform>(sun).world[3]));
      moveTo(p.x, p.y);
      break;
    }
    case 40:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 41:
      expect(selected_ == scene_.find("Sun"), "clicking a light's icon in the viewport selects it");
      // --- New script: creates scripts/<name>.lua from the template (never overwrites)
      {
        std::string path = newScriptPath("Orb");
        expect(path == "scripts/orb.lua", "script names come from the entity name");
        expect(writeScriptTemplate(path, "Orb"), "the template is written");
        std::string text;
        expect(readTextFile(project_.path(path), text) && text.find("function Script:update(dt)") != std::string::npos, "the template has update()");
        expect(writeScriptTemplate(path, "Other") && readTextFile(project_.path(path), text) && text.rfind("-- Orb:", 0) == 0,
               "an existing script is never overwritten");
        expect(newScriptPath("Orb") == "scripts/orb_2.lua", "a taken name gets a number");
        std::filesystem::remove(project_.path(path));
      }
      // --- path tracing: switching it off and on again must restart the image
      // (regression: it kept averaging onto whatever the other mode drew).
      selected_ = kNoEntity;
      scene_.settings.pathTraced = true;
      break;
    case 42: // four path-traced frames have been drawn since the switch
      app_.device->waitIdle();
      expect(app_.device->readTexture(viewportTexture_, pathTraceFirst_), "can read the viewport back");
      scene_.settings.pathTraced = false;
      break;
    case 43: scene_.settings.pathTraced = true; break;
    case 44: {
      std::vector<u8> again;
      app_.device->waitIdle();
      expect(app_.device->readTexture(viewportTexture_, again), "can read the viewport back");
      size_t differing = 0;
      for (size_t k = 0; k < again.size() && k < pathTraceFirst_.size(); k++) differing += again[k] != pathTraceFirst_[k];
      expect(again.size() == pathTraceFirst_.size() && differing == 0,
             "re-enabling path tracing gives the same image as the first time (the accumulation restarted)");
      scene_.settings.pathTraced = false;
      // --- pan: middle-drag moves the orbit target, not the angles
      panTargetBefore_ = camera_.target;
      yawBefore = camera_.yaw;
      moveTo(center().x, center().y);
      break;
    }
    case 45: io.AddMouseButtonEvent(ImGuiMouseButton_Middle, true); break;
    case 46: moveTo(center().x + 80, center().y + 40); break;
    case 47: moveTo(center().x + 160, center().y + 80); break;
    case 48: io.AddMouseButtonEvent(ImGuiMouseButton_Middle, false); break;
    case 49:
      expect(glm::distance(camera_.target, panTargetBefore_) > 0.1f, "middle-drag should pan the camera");
      expect(camera_.yaw == yawBefore, "panning must not orbit");
      camera_.target = panTargetBefore_;
      // --- hierarchy drag and drop: Rock onto Tower makes it a child...
      moveTo(rectCenter("node:Rock").x, rectCenter("node:Rock").y);
      break;
    case 50: io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); break;
    case 51: moveTo(mouse.x, mouse.y + 20); break; // past the drag threshold
    case 52: moveTo(rectCenter("node:Tower").x, rectCenter("node:Tower").y); break;
    case 53: io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); break;
    case 54: {
      std::vector<Entity> kids = scene_.children(scene_.find("Tower"));
      expect(std::find(kids.begin(), kids.end(), scene_.find("Rock")) != kids.end(), "dropping Rock on Tower in the Hierarchy re-parents it");
      // ...and dropping it on the empty area below makes it a root again.
      moveTo(rectCenter("node:Rock").x, rectCenter("node:Rock").y);
      break;
    }
    case 55: io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); break;
    case 56: moveTo(mouse.x, mouse.y + 20); break;
    case 57: moveTo(rectCenter("hierarchy:blank").x, rectCenter("hierarchy:blank").y); break;
    case 58: io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); break;
    case 59: {
      std::vector<Entity> roots = scene_.children(kNoEntity);
      expect(std::find(roots.begin(), roots.end(), scene_.find("Rock")) != roots.end(), "dropping on empty Hierarchy space un-parents");
      // --- drag an asset onto the selected entity's texture field
      selectAll({scene_.find("Rock")});
      moveTo(rectCenter("asset:assets/dirt.png").x, rectCenter("asset:assets/dirt.png").y);
      break;
    }
    case 60: io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); break;
    case 61: moveTo(mouse.x + 20, mouse.y); break;
    case 62: moveTo(rectCenter("field:texture").x, rectCenter("field:texture").y); break;
    case 63: io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); break;
    case 64:
      expect(scene_.reg.get<MeshRenderer>(scene_.find("Rock")).texture == "assets/dirt.png", "dropping an asset on a texture field assigns it");
      // --- type a new name into the Name field
      moveTo(rectCenter("field:Name").x, rectCenter("field:Name").y);
      break;
    case 65:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 66: key(ImGuiKey_A, true); break; // select the old name
    case 67:
      for (char c : std::string("Boulder")) io.AddInputCharacter((unsigned)c);
      break;
    case 68: key(ImGuiKey_Enter); break;
    case 69:
      expect(scene_.find("Boulder") != kNoEntity && scene_.find("Rock") == kNoEntity, "typing in the Name field renames the entity");
      // --- menus: Entity > Cube
      moveTo(rectCenter("menu:Entity").x, rectCenter("menu:Entity").y);
      break;
    case 70:
    case 72:
    case 75:
    case 77:
    case 80:
    case 83:
    case 85:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 71: moveTo(rectCenter("item:Cube").x, rectCenter("item:Cube").y); break;
    case 73:
      expect(scene_.find("Cube") != kNoEntity, "Entity > Cube creates a cube");
      // --- File > Save Scene As..., typing a new path
      moveTo(rectCenter("menu:File").x, rectCenter("menu:File").y);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 74: moveTo(rectCenter("item:Save Scene As...").x, rectCenter("item:Save Scene As...").y); break;
    case 76: moveTo(rectCenter("saveas:path").x, rectCenter("saveas:path").y); break;
    case 78: key(ImGuiKey_A, true); break;
    case 79:
      for (char c : std::string("scenes/autotest_copy")) io.AddInputCharacter((unsigned)c);
      moveTo(rectCenter("saveas:save").x, rectCenter("saveas:save").y);
      break;
    case 81: {
      Scene saved;
      expect(scenePath_ == "scenes/autotest_copy.scene.json" && saved.load(project_.path(scenePath_)) && saved.find("Boulder") != kNoEntity,
             "Save Scene As writes the typed path (with .scene.json added) and switches to it");
      // --- File > New Scene
      moveTo(rectCenter("menu:File").x, rectCenter("menu:File").y);
      break;
    }
    case 82: break;
    case 84: moveTo(rectCenter("item:New Scene").x, rectCenter("item:New Scene").y); break;
    case 86: {
      expect(scenePath_.rfind("scenes/untitled_", 0) == 0 && scene_.reg.view<Name>().size() == 2 && scene_.find("Camera") != kNoEntity &&
                 scene_.find("Sun") != kNoEntity,
             "File > New Scene starts an unsaved scene with a camera and a sun");
      if (autotestResult_ == 0) {
        ACE_INFO("autotest passed");
        autotestResult_ = 1;
      }
      break;
    }
    default: break;
  }
}

} // namespace ace
