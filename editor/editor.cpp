#include "editor.h"
#include "projects.h"
#include "ui_text.h"

#include <ImGuizmo.h>
#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <set>
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
    statusLine_ = line;
    statusLevel_ = level;
    statusTime_ = app_.time;
    if (level == LogLevel::Error) unseenErrors_++;
  });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  // Panel sizes live in the user's settings folder: an app opened from Finder
  // starts in "/", which is not writable. Automated runs use the default layout.
  automated_ = automated;
  iniPath_ = editorSettingsDir() + "imgui.ini";
  io.IniFilename = automated ? nullptr : iniPath_.c_str();
  io.ConfigWindowsMoveFromTitleBarOnly = true;
  ui::setup(SDL_GetWindowPixelDensity(app_.window));
  ImGuizmo::Style& gizmo = ImGuizmo::GetStyle(); // axis colors match the inspector's X/Y/Z strips
  for (int i = 0; i < 3; i++) gizmo.Colors[ImGuizmo::DIRECTION_X + i] = ImGui::ColorConvertU32ToFloat4(ui::color::kAxis[i]);
  gizmo.Colors[ImGuizmo::SELECTION] = ImGui::ColorConvertU32ToFloat4(ui::color::kAccent);
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
  locked_.clear();
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
  locked_.clear();
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
  paused_ = false;
  if (playing) {
    playSnapshot_ = scene_.toJson().dump();
    scripts_ = std::make_unique<ScriptSystem>(scene_, assets_, app_.input);
  } else {
    scripts_.reset();
    restore(playSnapshot_);
  }
}

Entity Editor::createEntity(const char* name, const char* mesh, const vec3* at) {
  Entity e = scene_.create(name);
  Transform& t = scene_.reg.get<Transform>(e);
  if (at) {
    t.position = *at;
  } else { // the point the camera orbits, on the ground, on the move grid
    t.position = vec3(camera_.target.x, 0.0f, camera_.target.z);
    if (snap_ && snapMove_ > 0.0f) t.position = glm::round(t.position / snapMove_) * snapMove_;
  }
  if (mesh) {
    scene_.reg.emplace<MeshRenderer>(e).mesh = mesh;
    t.position.y -= assets_.mesh(mesh)->bounds.min.y; // rest on the surface, not in it
  }
  selected_ = e;
  return e;
}

Entity Editor::createItem(int kind, const vec3* at) {
  switch (kind) {
    case 0: return createEntity("Entity", nullptr, at);
    case 1: { // level-building brick: world-mapped texture + collider
      Entity e = createEntity("Block", "builtin:cube", at);
      scene_.reg.get<MeshRenderer>(e).worldTexels = true;
      scene_.reg.emplace<Collider>(e);
      return e;
    }
    case 2: return createEntity("Cube", "builtin:cube", at);
    case 3: return createEntity("Sphere", "builtin:sphere", at);
    case 4: return createEntity("Plane", "builtin:plane", at);
    case 5: {
      Entity e = createEntity("Sprite", nullptr, at);
      scene_.reg.get<Transform>(e).position.y += 0.5f; // sprites are centred on their position
      scene_.reg.emplace<SpriteRenderer>(e);
      return e;
    }
    case 6: {
      Entity e = createEntity("Camera", nullptr, at);
      scene_.reg.emplace<Camera>(e);
      return e;
    }
    case 7: {
      Entity e = createEntity("Light", nullptr, at);
      scene_.reg.emplace<Light>(e);
      return e;
    }
    default: {
      Entity e = createEntity("Lamp", nullptr, at);
      scene_.reg.emplace<Light>(e).point = true;
      scene_.reg.get<Transform>(e).position.y += 2.0f;
      return e;
    }
  }
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

Entity Editor::placePrefab(const std::string& relativePath, const vec3* point) {
  Entity root = scene_.instantiatePrefab(project_.path(relativePath));
  if (root == kNoEntity) return kNoEntity;
  // At `point` (its height added to the prefab's own), else at the point the
  // editor camera orbits around, snapped to the move grid.
  vec3 at = point ? *point : camera_.target;
  if (!point && snap_ && snapMove_ > 0.0f) at = glm::round(at / snapMove_) * snapMove_;
  vec3& p = scene_.reg.get<Transform>(root).position;
  p = vec3(at.x, p.y + (point ? at.y : 0.0f), at.z);
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

namespace {

namespace col = ui::color;
using ui::Icon;

constexpr f32 kGap = 10.0f;     // between the floating cards and the window edges
constexpr f32 kTitleH = 24.0f;  // a card's title row
constexpr size_t kGroupMin = 3; // identical leaf siblings folded into one Scene row from this many
const ImGuiWindowFlags kCardFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove |
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse | ImGuiWindowFlags_NoBackground;
const ImGuiWindowFlags kFloatFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBackground |
                                     ImGuiWindowFlags_NoNav;
const char* const kCreateLabels[] = {"Empty", "Block", "Cube", "Sphere", "Plane", "Sprite", "Camera", "Light", "Point light"};

f32 fontSize(ImFont* f) { return f->FontSize * ImGui::GetIO().FontGlobalScale; }

void cardBackground() {
  ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
  ui::card(ImGui::GetWindowDrawList(), p, ImVec2(p.x + s.x, p.y + s.y));
}

// A pixel-font title (and an optional dim note) filling a title row.
void cardTitle(const char* title, const char* note) {
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  f32 ps = fontSize(ui::fonts.pixel);
  dl->AddText(ui::fonts.pixel, ps, ImVec2(p.x + 2, p.y + (kTitleH - ps) * 0.5f + 1), col::kText, title);
  f32 w = ui::fonts.pixel->CalcTextSizeA(ps, FLT_MAX, 0, title).x + 2;
  if (note) dl->AddText(ImVec2(p.x + w + 10, p.y + (kTitleH - ImGui::GetFontSize()) * 0.5f), col::kFaint, note);
  ImGui::Dummy(ImVec2(w, kTitleH));
}

// A tab drawn as pixel-font text, underlined in the accent color when active.
bool tabButton(const char* label, bool on, ImU32 ink = 0) {
  ImFont* font = ui::fonts.pixel;
  f32 fs = fontSize(font);
  ImVec2 ts = font->CalcTextSizeA(fs, FLT_MAX, 0, label);
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool pressed = ImGui::InvisibleButton(label, ImVec2(ts.x + 16, kTitleH));
  bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddText(font, fs, ImVec2(p.x + 8, p.y + (kTitleH - fs) * 0.5f + 1), ink ? ink : on ? col::kAccent : hovered ? col::kText : col::kDim, label);
  if (on) dl->AddRectFilled(ImVec2(p.x + 8, p.y + kTitleH - 2), ImVec2(p.x + 8 + ts.x, p.y + kTitleH), col::kAccent);
  return pressed;
}

// A text toggle: accent text on a soft accent background when on.
bool chip(const char* label, bool on, const char* tip = nullptr, ImU32 ink = 0) {
  ImVec2 ts = ImGui::CalcTextSize(label, nullptr, true);
  f32 h = ImGui::GetFrameHeight();
  ImVec2 p = ImGui::GetCursorScreenPos();
  bool pressed = ImGui::InvisibleButton(label, ImVec2(ts.x + 16, h));
  bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  if (on || hovered) dl->AddRectFilled(p, ImVec2(p.x + ts.x + 16, p.y + h), on ? col::kAccentSoft : col::kHover);
  ImU32 text = ink ? ink : on ? col::kAccent : hovered ? col::kText : col::kDim;
  dl->AddText(ImVec2(p.x + 8, p.y + (h - ImGui::GetFontSize()) * 0.5f), text, label, ImGui::FindRenderedTextEnd(label));
  if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
  return pressed;
}
f32 chipWidth(const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x + 16; }

std::string fitText(const std::string& s, f32 width) {
  if (ImGui::CalcTextSize(s.c_str()).x <= width) return s;
  std::string t = s;
  while (!t.empty() && ImGui::CalcTextSize((t + "...").c_str()).x > width) t.pop_back();
  return t + "...";
}

void toggleDrawer(int& tab, int& last, int which) {
  tab = tab == which ? -1 : which;
  if (tab >= 0) last = tab;
}

enum class AssetKind { Image, Mesh, Scene, Prefab, Script, Audio, Other };
AssetKind assetKind(const std::string& f) {
  if (endsWith(f, ".png") || endsWith(f, ".jpg") || endsWith(f, ".jpeg")) return AssetKind::Image;
  if (endsWith(f, ".gltf") || endsWith(f, ".glb")) return AssetKind::Mesh;
  if (endsWith(f, ".scene.json")) return AssetKind::Scene;
  if (endsWith(f, ".prefab.json")) return AssetKind::Prefab;
  if (endsWith(f, ".lua")) return AssetKind::Script;
  if (endsWith(f, ".wav") || endsWith(f, ".ogg")) return AssetKind::Audio;
  return AssetKind::Other;
}
Icon assetIcon(AssetKind k) {
  static const Icon icons[] = {Icon::Image, Icon::Mesh, Icon::Scene, Icon::Prefab, Icon::Script, Icon::File, Icon::File};
  return icons[(int)k];
}
ImU32 assetInk(AssetKind k) {
  static const ImU32 inks[] = {col::kAxis[2], col::kAxis[0], col::kPlay, col::kAccent, col::kWarn, col::kDim, col::kFaint};
  return inks[(int)k];
}
std::string fileName(const std::string& path) { return path.substr(path.find_last_of('/') + 1); }
std::string stem(const std::string& path) {
  std::string name = fileName(path);
  return name.substr(0, name.find('.'));
}
std::string assetTag(const std::string& f) { // "PNG", "SCENE", ...
  AssetKind k = assetKind(f);
  if (k == AssetKind::Scene) return "SCENE";
  if (k == AssetKind::Prefab) return "PREFAB";
  std::string name = fileName(f), ext = name.find('.') == std::string::npos ? "" : name.substr(name.find_last_of('.') + 1);
  for (char& c : ext) c = (char)toupper((unsigned char)c);
  return ext;
}

size_t fieldBytes(FieldType t) {
  switch (t) {
    case FieldType::Bool: return sizeof(bool);
    case FieldType::Int: return sizeof(int);
    case FieldType::Float: return sizeof(f32);
    case FieldType::Vec2: return sizeof(vec2);
    case FieldType::Vec3:
    case FieldType::Color3: return sizeof(vec3);
    case FieldType::Color4: return sizeof(vec4);
    case FieldType::Quat: return sizeof(quat);
    default: return 0;
  }
}
bool fieldEquals(const Field& f, const u8* a, const u8* b) {
  if (f.type == FieldType::String || f.type == FieldType::Asset) return *(const std::string*)a == *(const std::string*)b;
  return memcmp(a, b, fieldBytes(f.type)) == 0;
}
void fieldCopy(const Field& f, u8* dst, const u8* src) {
  if (f.type == FieldType::String || f.type == FieldType::Asset) *(std::string*)dst = *(const std::string*)src;
  else memcpy(dst, src, fieldBytes(f.type));
}

// A default-constructed component of `type`, for the reset buttons.
const void* componentDefaults(const ComponentType& type) {
  static entt::registry registry;
  static std::map<std::string, Entity> made;
  auto [it, fresh] = made.try_emplace(type.name, kNoEntity);
  if (fresh) {
    it->second = registry.create();
    type.add(registry, it->second);
  }
  return type.get(registry, it->second);
}

// N drag fields side by side, each with an X/Y/Z color strip.
bool dragAxes(f32* v, int n, f32 speed, f32 width, bool& done) {
  bool changed = false;
  const f32 gap = 3.0f, w = (width - gap * (n - 1)) / n;
  for (int i = 0; i < n; i++) {
    ImGui::PushID(i);
    if (i) ImGui::SameLine(0, gap);
    ImVec2 a = ImGui::GetCursorScreenPos();
    ImGui::SetNextItemWidth(w);
    changed |= ImGui::DragFloat("##axis", &v[i], speed, 0.0f, 0.0f, "%.4g");
    done |= ImGui::IsItemDeactivatedAfterEdit();
    ImGui::GetWindowDrawList()->AddRectFilled(a, ImVec2(a.x + 2, a.y + ImGui::GetFrameHeight()), col::kAxis[i]);
    ImGui::PopID();
  }
  return changed;
}

struct Command {
  std::string label, note; // note: shortcut or kind, shown on the right
  Icon icon;
  std::function<void()> run;
  int score = 0;
};

} // namespace

std::string Editor::modKey(const char* key) const {
  return std::string(ImGui::GetIO().ConfigMacOSXBehaviors ? "Cmd+" : "Ctrl+") + key;
}

ui::Icon Editor::entityIcon(Entity e) {
  if (scene_.reg.all_of<Camera>(e)) return Icon::Camera;
  if (const Light* l = scene_.reg.try_get<Light>(e)) return l->point ? Icon::Bulb : Icon::Sun;
  if (scene_.reg.all_of<SpriteRenderer>(e)) return Icon::Sprite;
  if (scene_.reg.all_of<MeshRenderer>(e)) return Icon::Mesh;
  if (scene_.reg.all_of<Script>(e)) return Icon::Script;
  if (scene_.reg.all_of<Collider>(e)) return Icon::Collider;
  return Icon::Empty;
}

Aabb Editor::worldBounds(Entity e) {
  Aabb box = scene_.colliderBounds(e);
  if (box.min.x <= box.max.x) return box;
  Aabb local;
  if (const MeshRenderer* m = scene_.reg.try_get<MeshRenderer>(e)) local = assets_.mesh(m->mesh)->bounds;
  else if (const SpriteRenderer* s = scene_.reg.try_get<SpriteRenderer>(e)) local = Aabb{vec3(-s->size * 0.5f, 0.0f), vec3(s->size * 0.5f, 0.0f)};
  else return box;
  const mat4& w = scene_.reg.get<Transform>(e).world;
  for (int c = 0; c < 8; c++)
    box.grow(vec3(w * vec4(c & 1 ? local.max.x : local.min.x, c & 2 ? local.max.y : local.min.y, c & 4 ? local.max.z : local.min.z, 1.0f)));
  return box;
}

bool Editor::isLocked(Entity e) const {
  for (Entity p = e; scene_.reg.valid(p); p = scene_.reg.get<Transform>(p).parent)
    if (std::find(locked_.begin(), locked_.end(), p) != locked_.end()) return true;
  return false;
}

void Editor::frameSelection() {
  if (selected_ != kNoEntity) camera_.target = vec3(scene_.reg.get<Transform>(selected_).world[3]);
}

void Editor::reloadAssets() {
  app_.device->waitIdle();
  assets_.reload();
  renderer_.clearTextureCaches();
  ui_->forgetAllTextures(); // thumbnails: reloaded textures can reuse old ids
  paletteRows_.clear();     // re-read from the file
  palettePath_.clear();
}

void Editor::openCommands() { commandsOpen_ = true; }

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

  if (!scene_.reg.valid(selected_)) selected_ = kNoEntity;
  if (selected_ != lastPrimary_) { // a plain click, pick, undo...: the selection is just the new primary
    extraSelection_.clear();
    lastPrimary_ = selected_;
  }
  std::erase_if(extraSelection_, [&](Entity e) { return !scene_.reg.valid(e) || e == selected_; });
  std::erase_if(locked_, [&](Entity e) { return !scene_.reg.valid(e); });
  if (!focusWindow.empty()) { // --focus: show that panel
    if (focusWindow == "Render Settings" || focusWindow == "Render") rightTab_ = 1;
    else if (focusWindow == "Palette") rightTab_ = 2;
    else if (focusWindow == "Assets") drawerTab_ = 0;
    else if (focusWindow == "Console") drawerTab_ = 1;
    else if (focusWindow == "Commands") openCommands();
    focusWindow.clear();
  }
  handleShortcuts();
  drawTopBar();
  drawStatusBar();
  drawViewport(); // fills the work area, behind everything else
  ImGuiViewport* vp = ImGui::GetMainViewport();
  leftEdge_ = vp->WorkPos.x;
  rightEdge_ = vp->WorkPos.x + vp->WorkSize.x;
  if (!panelsHidden_) {
    drawSceneCard();
    drawRightCard();
    drawToolRail();
    drawViewOverlay();
    drawDrawer();
  }
  drawCommandPalette();
  drawSaveAs();

  if (g_deleteRequest) {
    g_deleteRequest = false;
    deleteSelection();
  }
  if (g_duplicateRequest) {
    g_duplicateRequest = false;
    duplicateSelection();
  }

  if (playing_ && !paused_) {
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
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_K) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_P)) openCommands();
  if (io.WantTextInput) return;
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) saveScene();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z)) redo();
  else if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_J)) toggleDrawer(drawerTab_, lastDrawerTab_, drawerTab_ >= 0 ? drawerTab_ : lastDrawerTab_);
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Backslash)) panelsHidden_ = !panelsHidden_;
  if (ImGui::IsKeyPressed(ImGuiKey_F5)) setPlaying(!playing_);
  if (ImGui::IsKeyPressed(ImGuiKey_F6) && playing_) paused_ = !paused_;
  if (playing_) return; // the keys below belong to the game while it runs
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_V)) paste();
  if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_A)) selectAll(orderedEntities(scene_));
  if (selected_ != kNoEntity) {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) g_duplicateRequest = true;
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_C)) copySelection();
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Backspace)) g_deleteRequest = true;
    if (ImGui::IsKeyPressed(ImGuiKey_F)) frameSelection();
    if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
      renaming_ = selected_;
      renameText_ = scene_.reg.get<Name>(selected_).value;
    }
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    if (buildMode_) buildMode_ = false;
    else selectAll({});
  }
  if (!ImGui::IsMouseDown(ImGuiMouseButton_Right) && !io.KeyCtrl) {
    int op = ImGui::IsKeyPressed(ImGuiKey_Q) ? -1 : ImGui::IsKeyPressed(ImGuiKey_W) ? 0 : ImGui::IsKeyPressed(ImGuiKey_E) ? 1 : ImGui::IsKeyPressed(ImGuiKey_R) ? 2 : -2;
    if (op != -2) {
      gizmoOp_ = op;
      buildMode_ = false;
    }
    if (ImGui::IsKeyPressed(ImGuiKey_B)) buildMode_ = !buildMode_;
  }
}

// ---------------------------------------------------------------- bars

void Editor::drawCreateItems() {
  for (int i = 0; i < (int)std::size(kCreateLabels); i++) {
    if (i == 6) ImGui::Separator();
    if (ImGui::MenuItem(kCreateLabels[i])) {
      createItem(i);
      commit();
    }
    if (i == 1 && ImGui::IsItemHovered()) ImGui::SetTooltip("A textured, solid level-building brick (world-mapped texture + collider)");
    testRect(std::string("item:") + kCreateLabels[i]);
  }
}

void Editor::drawTopBar() {
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 10));
  ImGui::PushStyleColor(ImGuiCol_MenuBarBg, col::kBar);
  if (ImGui::BeginMainMenuBar()) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    f32 barH = ImGui::GetWindowHeight();
    // Logo, in the engine's own pixel font.
    ImVec2 p = ImGui::GetCursorScreenPos();
    f32 ls = fontSize(ui::fonts.pixelLarge);
    dl->AddText(ui::fonts.pixelLarge, ls, ImVec2(p.x + 4, p.y + (barH - ls) * 0.5f + 2), col::kAccent, "ACE");
    ImGui::Dummy(ImVec2(ui::fonts.pixelLarge->CalcTextSizeA(ls, FLT_MAX, 0, "ACE").x + 12, 1));

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6)); // menu contents
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
      if (ImGui::MenuItem("Save Scene", modKey("S").c_str())) saveScene();
      if (ImGui::MenuItem("Save Scene As...")) openSaveAs_ = true;
      testRect("item:Save Scene As...");
      ImGui::Separator();
      if (ImGui::MenuItem("Use As Start Scene", nullptr, project_.startScene == scenePath_)) {
        project_.startScene = scenePath_;
        project_.save();
      }
      if (ImGui::MenuItem("Reload Assets")) reloadAssets();
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
      if (ImGui::MenuItem("Undo", modKey("Z").c_str(), false, !playing_ && undoIndex_ > 0)) undo();
      if (ImGui::MenuItem("Redo", modKey("Shift+Z").c_str(), false, !playing_ && undoIndex_ + 1 < undoStack_.size())) redo();
      ImGui::Separator();
      if (ImGui::MenuItem("Copy", modKey("C").c_str(), false, selected_ != kNoEntity)) copySelection();
      if (ImGui::MenuItem("Paste", modKey("V").c_str())) paste();
      if (ImGui::MenuItem("Duplicate", modKey("D").c_str(), false, selected_ != kNoEntity)) g_duplicateRequest = true;
      if (ImGui::MenuItem("Delete", "Del", false, selected_ != kNoEntity)) g_deleteRequest = true;
      ImGui::Separator();
      if (ImGui::MenuItem("Select All", modKey("A").c_str())) selectAll(orderedEntities(scene_));
      if (ImGui::MenuItem("Frame Selection", "F", false, selected_ != kNoEntity)) frameSelection();
      ImGui::EndMenu();
    }
    bool createMenu = ImGui::BeginMenu("Create");
    testRect("menu:Create");
    if (createMenu) {
      ImGui::BeginDisabled(playing_);
      drawCreateItems();
      ImGui::EndDisabled();
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
      if (ImGui::MenuItem("Command Palette", modKey("K").c_str())) openCommands();
      ImGui::Separator();
      if (ImGui::MenuItem("Scene List", nullptr, sceneOpen_)) sceneOpen_ = !sceneOpen_;
      if (ImGui::MenuItem("Inspector", nullptr, inspectorOpen_ && rightTab_ == 0)) inspectorOpen_ = true, rightTab_ = 0;
      if (ImGui::MenuItem("Render Settings", nullptr, inspectorOpen_ && rightTab_ == 1)) inspectorOpen_ = true, rightTab_ = 1;
      if (ImGui::MenuItem("Palette", nullptr, inspectorOpen_ && rightTab_ == 2)) inspectorOpen_ = true, rightTab_ = 2;
      if (ImGui::MenuItem("Assets", modKey("J").c_str(), drawerTab_ == 0)) toggleDrawer(drawerTab_, lastDrawerTab_, 0);
      if (ImGui::MenuItem("Console", nullptr, drawerTab_ == 1)) toggleDrawer(drawerTab_, lastDrawerTab_, 1);
      ImGui::Separator();
      if (ImGui::MenuItem("Hide Panels", modKey("\\").c_str(), panelsHidden_)) panelsHidden_ = !panelsHidden_;
      if (ImGui::MenuItem("Orthographic", nullptr, camera_.orthographic)) camera_.orthographic = !camera_.orthographic;
      ImGui::EndMenu();
    }
    ImGui::PopStyleVar();

    // Where we are: project / scene, with a dot while there are unsaved changes.
    ImGui::Dummy(ImVec2(10, 1));
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kFaint), "%s  /", project_.name.c_str());
    ImGui::TextUnformatted(stem(scenePath_).c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", scenePath_.c_str());
    if (dirty_) {
      ImVec2 r = ImGui::GetItemRectMax(), l = ImGui::GetItemRectMin();
      f32 cy = (l.y + r.y) * 0.5f;
      dl->AddRectFilled(ImVec2(r.x + 6, cy - 3), ImVec2(r.x + 12, cy + 3), col::kAccent);
      ImGui::Dummy(ImVec2(10, 1));
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Unsaved changes (%s)", modKey("S").c_str());
    }

    // Transport, centered in the window. Each button is placed explicitly:
    // SameLine in a menu bar would put the second one at the top of the bar.
    const f32 btn = 26.0f, btnY = floorf((barH - btn) * 0.5f), midX = floorf(ImGui::GetWindowWidth() * 0.5f);
    ImGui::SetCursorPos(ImVec2(midX - btn - 1, btnY));
    if (ui::iconButton("##play", playing_ ? Icon::Stop : Icon::Play, playing_, playing_ ? "Stop (F5)" : "Play (F5)", btn)) setPlaying(!playing_);
    ImGui::SetCursorPos(ImVec2(midX + 1, btnY));
    ImGui::BeginDisabled(!playing_);
    if (ui::iconButton("##pause", Icon::Pause, paused_, "Pause (F6)", btn)) paused_ = !paused_;
    ImGui::EndDisabled();
    if (playing_) { // the mode, beside the transport (never over the game's own HUD)
      const char* label = paused_ ? "PAUSED" : "PLAYING";
      f32 ps = fontSize(ui::fonts.pixel);
      ImVec2 ts = ui::fonts.pixel->CalcTextSizeA(ps, FLT_MAX, 0, label);
      ImVec2 a(ImGui::GetWindowPos().x + midX + btn + 10, ImGui::GetWindowPos().y + floorf((barH - ps) * 0.5f) - 3);
      dl->AddRectFilled(a, ImVec2(a.x + ts.x + 12, a.y + ps + 5), paused_ ? col::kWarn : col::kPlay);
      dl->AddText(ui::fonts.pixel, ps, ImVec2(a.x + 6, a.y + 3), col::kBackdrop, label);
    }

    // The command palette's entry point, right-aligned like a search field.
    const f32 w = 300.0f, h = 26.0f;
    ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - w - 12, (barH - h) * 0.5f));
    ImVec2 q = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##commands", ImVec2(w, h))) openCommands();
    dl->AddRectFilled(q, ImVec2(q.x + w, q.y + h), ImGui::IsItemHovered() ? col::kHover : col::kRaised);
    ui::icon(dl, ImVec2(q.x + 14, q.y + h * 0.5f), Icon::Search, col::kDim);
    f32 ty = q.y + (h - ImGui::GetFontSize()) * 0.5f;
    dl->AddText(ImVec2(q.x + 28, ty), col::kFaint, "Search or run a command");
    std::string keys = modKey("K");
    dl->AddText(ImVec2(q.x + w - 10 - ImGui::CalcTextSize(keys.c_str()).x, ty), col::kFaint, keys.c_str());
    ImGui::EndMainMenuBar();
  }
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
}

void Editor::drawSaveAs() {
  if (std::exchange(openSaveAs_, false)) {
    saveAsPath_ = scenePath_;
    ImGui::OpenPopup("Save Scene As");
  }
  if (!ImGui::BeginPopupModal("Save Scene As", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextUnformatted("Path inside the project:");
  ImGui::SetNextItemWidth(360);
  ImGui::InputText("##path", &saveAsPath_);
  testRect("saveas:path");
  // Keep saves inside the project folder.
  bool valid = !saveAsPath_.empty() && saveAsPath_[0] != '/' && saveAsPath_.find("..") == std::string::npos;
  ImGui::BeginDisabled(!valid);
  bool save = ImGui::Button("Save");
  testRect("saveas:save");
  if (save) {
    if (!endsWith(saveAsPath_, ".scene.json")) saveAsPath_ += ".scene.json";
    scenePath_ = saveAsPath_;
    saveScene();
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndDisabled();
  ImGui::SameLine();
  if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

void Editor::drawStatusBar() {
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col::kBar);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 2));
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6, 3));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(6, 0));
  f32 h = ImGui::GetFrameHeight() + 4;
  if (ImGui::BeginViewportSideBar("##status", ImGui::GetMainViewport(), ImGuiDir_Down, h,
                                  ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav)) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    // What is selected.
    ImGui::AlignTextToFramePadding();
    if (selected_ != kNoEntity) {
      ImVec2 p = ImGui::GetCursorScreenPos();
      ImGui::Dummy(ImVec2(12, ImGui::GetFrameHeight()));
      ui::icon(dl, ImVec2(p.x + 5, p.y + ImGui::GetFrameHeight() * 0.5f), entityIcon(selected_), col::kAccent);
      ImGui::SameLine();
      ImGui::TextUnformatted(scene_.reg.get<Name>(selected_).value.c_str());
      if (!extraSelection_.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kFaint), "+%zu", extraSelection_.size());
      }
    } else {
      ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kFaint), "No selection");
    }

    // Right side: drawer toggles and stats.
    std::string stats = std::string(rhi::backendName(app_.device->backend())) + "   " + std::to_string((int)(ImGui::GetIO().Framerate + 0.5f)) + " fps";
    std::string console = unseenErrors_ ? "Console  " + std::to_string(unseenErrors_) + (unseenErrors_ == 1 ? " error" : " errors") : "Console";
    f32 rightW = chipWidth("Assets") + chipWidth(console.c_str()) + ImGui::CalcTextSize(stats.c_str()).x + 18;
    f32 rightX = ImGui::GetWindowWidth() - rightW - 10;

    // Middle: the latest log line for a few seconds, otherwise what you can do now.
    std::string hint;
    ImU32 ink = col::kDim;
    if (app_.time - statusTime_ < 4.0) {
      static const ImU32 inks[] = {col::kText, col::kWarn, col::kError};
      hint = statusLine_;
      ink = inks[(int)statusLevel_];
    } else if (playing_) {
      hint = paused_ ? "Paused     F6 resume     F5 stop" : "Playing     F5 stop     F6 pause     changes made now are undone when you stop";
    } else if (buildMode_) {
      hint = "Click a surface: add a block     Shift+click: remove one     B or Esc: leave Build";
    } else if (selected_ != kNoEntity) {
      hint = "W move     E rotate     R scale     F frame     F2 rename     " + modKey("D") + " duplicate     Del delete";
    } else {
      hint = "Click: select     Right-drag: orbit     Middle-drag: pan     Wheel: zoom     " + modKey("K") + ": find anything";
    }
    ImGui::SameLine(0, 20);
    f32 room = rightX - ImGui::GetCursorPosX() - 16;
    if (room > 40) ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(ink), "%s", fitText(hint, room).c_str());

    ImGui::SameLine(rightX);
    if (chip("Assets", drawerTab_ == 0, ("Project files (" + modKey("J") + ")").c_str())) toggleDrawer(drawerTab_, lastDrawerTab_, 0);
    ImGui::SameLine();
    if (chip(console.c_str(), drawerTab_ == 1, "Log messages", unseenErrors_ ? col::kError : 0)) toggleDrawer(drawerTab_, lastDrawerTab_, 1);
    ImGui::SameLine(0, 12);
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kFaint), "%s", stats.c_str());
  }
  ImGui::End();
  ImGui::PopStyleVar(3);
  ImGui::PopStyleColor();
}

// ---------------------------------------------------------------- scene list

void Editor::drawSceneCard() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  f32 h = sceneOpen_ ? vp->WorkSize.y - 2 * kGap : kTitleH + 16;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + kGap, vp->WorkPos.y + kGap));
  ImGui::SetNextWindowSize(ImVec2(270, h), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(200, h), ImVec2(560, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
  ImGui::Begin("Scene##card", nullptr, kCardFlags);
  ImGui::PopStyleVar();
  cardBackground();
  leftEdge_ = ImGui::GetWindowPos().x + ImGui::GetWindowSize().x;

  std::string count = std::to_string(scene_.reg.view<Name>().size());
  cardTitle("SCENE", count.c_str());
  ImGui::SameLine(ImGui::GetWindowWidth() - 10 - 2 * kTitleH);
  ImGui::BeginDisabled(playing_);
  if (ui::iconButton("##create", Icon::Plus, false, "Create an entity", kTitleH, 1.0f)) ImGui::OpenPopup("create");
  ImGui::EndDisabled();
  if (ImGui::BeginPopup("create")) {
    drawCreateItems();
    ImGui::EndPopup();
  }
  ImGui::SameLine(0, 0);
  if (ui::iconButton("##fold", sceneOpen_ ? Icon::ChevronDown : Icon::ChevronRight, false, sceneOpen_ ? "Collapse" : "Expand", kTitleH, 1.0f))
    sceneOpen_ = !sceneOpen_;
  if (!sceneOpen_) {
    ImGui::End();
    return;
  }

  ImGui::SetNextItemWidth(-1);
  ImGui::InputTextWithHint("##find", "Find in scene", &hierarchyFilter_);
  testRect("hierarchy:search");
  ImGui::BeginChild("rows");
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 3));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 1));
  if (hierarchyFilter_.empty()) {
    drawHierarchyChildren(scene_.children(kNoEntity));
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
  } else {
    drawHierarchySearch();
  }
  ImGui::PopStyleVar(2);
  ImGui::EndChild();
  ImGui::End();
}

void Editor::drawRowLabel(ui::Icon which, const std::string& name, const char* badge, bool faint) {
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  f32 x = a.x + ImGui::GetTreeNodeToLabelSpacing(), cy = (a.y + b.y) * 0.5f, fs = ImGui::GetFontSize();
  ui::icon(dl, ImVec2(x + 3, cy), which, faint ? col::kFaint : col::kDim);
  f32 badgeW = badge ? ImGui::CalcTextSize(badge).x + 6 : 0.0f;
  f32 right = b.x - (b.y - a.y) - 2 - badgeW; // room for the lock button
  dl->PushClipRect(a, ImVec2(right, b.y), true);
  dl->AddText(ImVec2(x + 13, cy - fs * 0.5f), faint ? col::kFaint : col::kText, name.c_str());
  dl->PopClipRect();
  if (badge) dl->AddText(ImVec2(right + 2, cy - fs * 0.5f), col::kFaint, badge);
}

void Editor::drawHierarchyChildren(const std::vector<Entity>& kids) {
  // Leaves sharing a name (a forest of "Tree"s) fold into one row.
  std::map<std::string, std::vector<Entity>> leaves;
  std::vector<bool> leaf(kids.size());
  for (size_t i = 0; i < kids.size(); i++)
    if ((leaf[i] = scene_.children(kids[i]).empty())) leaves[scene_.reg.get<Name>(kids[i]).value].push_back(kids[i]);
  std::set<std::string> grouped;
  for (size_t i = 0; i < kids.size(); i++) {
    const std::string& name = scene_.reg.get<Name>(kids[i]).value;
    if (leaf[i] && leaves[name].size() >= kGroupMin) {
      if (grouped.insert(name).second) drawHierarchyGroup(name, leaves[name]);
      continue;
    }
    drawHierarchyNode(kids[i]);
  }
}

void Editor::drawHierarchyGroup(const std::string& name, const std::vector<Entity>& members) {
  bool all = std::all_of(members.begin(), members.end(), [&](Entity e) { return isSelected(e); });
  ImGui::PushID(("group:" + name).c_str());
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_FramePadding;
  bool open = ImGui::TreeNodeEx("##group", flags | (all ? ImGuiTreeNodeFlags_Selected : 0));
  testRect("group:" + name);
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeyShift) {
      for (Entity e : members)
        if (!isSelected(e)) toggleSelected(e);
    } else {
      selectAll(members);
    }
  }
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("%zu entities named %s: click selects all of them, the arrow lists them", members.size(), name.c_str());
  std::string badge = "x" + std::to_string(members.size());
  drawRowLabel(entityIcon(members[0]), name, badge.c_str(), false);
  if (open) {
    for (Entity e : members) drawHierarchyNode(e);
    ImGui::TreePop();
  }
  ImGui::PopID();
}

void Editor::drawHierarchyNode(Entity e) {
  std::vector<Entity> kids = scene_.children(e);
  std::string name = scene_.reg.get<Name>(e).value; // a copy: renaming changes it
  ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_DefaultOpen |
                             ImGuiTreeNodeFlags_FramePadding | ImGuiTreeNodeFlags_AllowOverlap;
  if (kids.empty()) flags |= ImGuiTreeNodeFlags_Leaf;
  if (isSelected(e)) flags |= ImGuiTreeNodeFlags_Selected;
  ImGui::PushID((int)(u32)e);
  bool open = ImGui::TreeNodeEx("##node", flags);
  testRect("node:" + name);
  ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
  bool toggled = ImGui::IsItemToggledOpen();
  if (ImGui::IsItemClicked() && !toggled) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeyShift) toggleSelected(e); // Cmd-click on macOS arrives as Ctrl
    else selected_ = e;
  }
  if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && !toggled && !playing_) {
    renaming_ = e;
    renameText_ = name;
  }
  if (ImGui::BeginDragDropSource()) {
    ImGui::SetDragDropPayload("ACE_ENTITY", &e, sizeof(e));
    ImGui::TextUnformatted(name.c_str());
    ImGui::EndDragDropSource();
  }
  if (ImGui::BeginDragDropTarget()) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("ACE_ENTITY")) {
      scene_.setParent(*(const Entity*)p->Data, e);
      commit();
    }
    ImGui::EndDragDropTarget();
  }
  bool locked = std::find(locked_.begin(), locked_.end(), e) != locked_.end();
  auto toggleLock = [&] {
    if (locked) std::erase(locked_, e);
    else locked_.push_back(e);
  };
  if (ImGui::BeginPopupContextItem()) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 6));
    if (!isSelected(e)) selected_ = e; // right-click inside a multi-selection keeps it
    if (ImGui::MenuItem("Rename", "F2")) {
      renaming_ = e;
      renameText_ = name;
    }
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
      for (char c : name) file += isalnum((unsigned char)c) || c == '-' || c == '_' ? c : '_';
      if (file.empty()) file = "prefab";
      std::error_code ec;
      std::filesystem::create_directories(project_.path("prefabs"), ec);
      std::string relative = "prefabs/" + file + ".prefab.json";
      if (scene_.savePrefab(e, project_.path(relative))) ACE_INFO("saved %s (double-click it in Assets to place a copy)", relative.c_str());
      else ACE_ERROR("could not write %s", relative.c_str());
      assetsRefreshed_ = -10.0;
    }
    if (ImGui::MenuItem(locked ? "Unlock" : "Lock")) toggleLock();
    ImGui::Separator();
    if (ImGui::MenuItem("Delete")) g_deleteRequest = true;
    ImGui::PopStyleVar();
    ImGui::EndPopup();
  }

  f32 labelX = a.x + ImGui::GetTreeNodeToLabelSpacing() + 10;
  if (renaming_ == e) {
    static Entity focused = kNoEntity; // focus the field once per rename
    ImGui::SameLine();
    ImGui::SetCursorScreenPos(ImVec2(labelX, a.y));
    ImGui::SetNextItemWidth(b.x - labelX - 2);
    if (focused != e) {
      ImGui::SetKeyboardFocusHere();
      focused = e;
    }
    bool enter = ImGui::InputText("##rename", &renameText_, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (enter || (ImGui::IsItemDeactivated() && !ImGui::IsKeyPressed(ImGuiKey_Escape))) {
      if (!renameText_.empty() && renameText_ != name) {
        scene_.reg.get<Name>(e).value = renameText_;
        commit();
      }
      renaming_ = kNoEntity;
      focused = kNoEntity;
    } else if (ImGui::IsItemDeactivated()) {
      renaming_ = kNoEntity;
      focused = kNoEntity;
    }
  } else {
    std::string count = std::to_string(kids.size());
    drawRowLabel(entityIcon(e), name, kids.empty() ? nullptr : count.c_str(), isLocked(e));
    // The lock shows on hover, and stays while locked.
    f32 h = b.y - a.y;
    if (locked || (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(a, b))) {
      ImGui::SameLine();
      ImGui::SetCursorScreenPos(ImVec2(b.x - h, a.y));
      if (ui::iconButton("##lock", Icon::Lock, locked, locked ? "Unlock" : "Lock: viewport clicks pass through it", h, 1.0f)) toggleLock();
    }
  }
  if (open) {
    drawHierarchyChildren(kids);
    ImGui::TreePop();
  }
  ImGui::PopID();
}

void Editor::drawHierarchySearch() {
  std::vector<std::pair<int, Entity>> hits;
  for (auto [e, n] : scene_.reg.view<Name>().each())
    if (int score = ui::fuzzyScore(hierarchyFilter_, n.value); score >= 0) hits.push_back({score, e});
  std::stable_sort(hits.begin(), hits.end(), [](auto& x, auto& y) { return x.first > y.first; });
  if (hits.empty()) ImGui::TextDisabled("Nothing is named like that.");
  for (size_t i = 0; i < hits.size() && i < 300; i++) {
    Entity e = hits[i].second;
    const std::string& name = scene_.reg.get<Name>(e).value;
    ImGui::PushID((int)(u32)e);
    if (ImGui::Selectable("##hit", isSelected(e), ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, ImGui::GetFrameHeight()))) {
      ImGuiIO& io = ImGui::GetIO();
      if (io.KeyCtrl || io.KeyShift) toggleSelected(e);
      else selected_ = e;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) frameSelection();
    }
    testRect("found:" + name);
    Entity parent = scene_.reg.get<Transform>(e).parent;
    drawRowLabel(entityIcon(e), name, scene_.reg.valid(parent) ? scene_.reg.get<Name>(parent).value.c_str() : nullptr, isLocked(e));
    ImGui::PopID();
  }
}

// ---------------------------------------------------------------- inspector

bool Editor::sectionHeader(const char* title, ui::Icon which, bool* remove) {
  ImGuiStorage* storage = ImGui::GetStateStorage();
  ImGuiID id = ImGui::GetID(title);
  bool open = storage->GetBool(id, true);
  ImVec2 a = ImGui::GetCursorScreenPos();
  f32 w = ImGui::GetContentRegionAvail().x, h = 26.0f;
  if (ImGui::InvisibleButton(title, ImVec2(remove ? w - h : w, h))) storage->SetBool(id, open = !open);
  bool hovered = ImGui::IsItemHovered();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(a, ImVec2(a.x + w, a.y + h), hovered ? col::kHover : col::kRaised);
  ui::icon(dl, ImVec2(a.x + 10, a.y + h * 0.5f), open ? Icon::ChevronDown : Icon::ChevronRight, col::kFaint);
  ui::icon(dl, ImVec2(a.x + 26, a.y + h * 0.5f), which, col::kAccent);
  std::string upper = ui::humanize(title);
  for (char& c : upper) c = (char)toupper((unsigned char)c);
  f32 ps = fontSize(ui::fonts.pixel);
  dl->AddText(ui::fonts.pixel, ps, ImVec2(a.x + 40, a.y + (h - ps) * 0.5f + 1), col::kText, upper.c_str());
  if (remove) {
    ImGui::SameLine(0, 0);
    if (ui::iconButton("##remove", Icon::Close, false, "Remove this component", h, 1.0f)) *remove = true;
  }
  if (open) ImGui::Dummy(ImVec2(0, 2));
  return open;
}

bool Editor::editFields(void* object, const std::vector<Field>& fields, const void* defaults, const char* tip) {
  bool finished = false;
  const f32 labelW = floorf(std::max(92.0f, ImGui::GetContentRegionAvail().x * 0.36f));
  const f32 resetW = ImGui::GetFrameHeight();
  for (const Field& f : fields) {
    u8* p = (u8*)object + f.offset;
    const u8* d = defaults ? (const u8*)defaults + f.offset : nullptr;
    ImGui::PushID(f.name);
    f32 x0 = ImGui::GetCursorPosX();
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, col::kDim);
    ImGui::TextUnformatted(ui::humanize(f.name).c_str());
    ImGui::PopStyleColor();
    if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
    ImGui::SameLine(x0 + labelW);
    f32 width = ImGui::GetContentRegionAvail().x - resetW - 4;
    bool changed = false, instant = false, done = false;
    auto finishedEditing = [&] { done |= ImGui::IsItemDeactivatedAfterEdit(); };
    switch (f.type) {
      case FieldType::Bool: changed = instant = ImGui::Checkbox("##v", (bool*)p); break;
      case FieldType::Int:
        ImGui::SetNextItemWidth(width);
        changed = ImGui::DragInt("##v", (int*)p, 1.0f, 0, 100000);
        finishedEditing();
        break;
      case FieldType::Float:
        ImGui::SetNextItemWidth(width);
        changed = ImGui::DragFloat("##v", (f32*)p, 0.02f, 0.0f, 0.0f, "%.4g");
        finishedEditing();
        break;
      case FieldType::Vec2: changed = dragAxes((f32*)p, 2, 0.05f, width, done); break;
      case FieldType::Vec3: changed = dragAxes((f32*)p, 3, 0.05f, width, done); break;
      case FieldType::Color3:
        ImGui::SetNextItemWidth(width);
        changed = ImGui::ColorEdit3("##v", (f32*)p, ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR);
        finishedEditing();
        break;
      case FieldType::Color4:
        ImGui::SetNextItemWidth(width);
        changed = ImGui::ColorEdit4("##v", (f32*)p);
        finishedEditing();
        break;
      case FieldType::Quat: {
        // shortcut: Euler angles are re-derived from the quaternion every frame,
        // so values can jump near +-90 degrees pitch; store Euler alongside if it annoys.
        quat& q = *(quat*)p;
        vec3 euler = glm::degrees(glm::eulerAngles(q));
        if ((changed = dragAxes(&euler.x, 3, 0.5f, width, done))) q = glm::normalize(quat(glm::radians(euler)));
        break;
      }
      case FieldType::String:
      case FieldType::Asset: {
        if (f.type == FieldType::Asset) {
          if (assetPicker(f, *(std::string*)p)) changed = instant = true;
          ImGui::SameLine(0.0f, 2.0f);
          width -= ImGui::GetFrameHeight() + 2;
        }
        ImGui::SetNextItemWidth(width);
        changed |= ImGui::InputText("##v", (std::string*)p);
        testRect(std::string("field:") + f.name);
        finishedEditing();
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
    // Reset: only shown when the value differs from the default (like Godot's revert arrow).
    ImGui::SameLine(0, 4);
    if (d && !fieldEquals(f, p, d)) {
      if (ui::iconButton("##reset", Icon::Reset, false, "Reset to the default", resetW, 1.0f)) {
        fieldCopy(f, p, d);
        changed = instant = true;
      }
    } else {
      ImGui::Dummy(ImVec2(resetW, resetW));
    }
    if (changed) dirty_ = true;
    if (instant || done) finished = true;
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

void Editor::drawRightCard() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  f32 h = inspectorOpen_ ? vp->WorkSize.y - 2 * kGap : kTitleH + 16;
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - kGap, vp->WorkPos.y + kGap), ImGuiCond_Always, ImVec2(1, 0));
  ImGui::SetNextWindowSize(ImVec2(360, h), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(280, h), ImVec2(680, h));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
  ImGui::Begin("Inspector##card", nullptr, kCardFlags);
  ImGui::PopStyleVar();
  cardBackground();
  rightEdge_ = ImGui::GetWindowPos().x;

  static const char* const tabs[] = {"INSPECTOR", "RENDER", "PALETTE"};
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 6));
  for (int i = 0; i < 3; i++) {
    if (i) ImGui::SameLine();
    if (tabButton(tabs[i], inspectorOpen_ && rightTab_ == i)) {
      rightTab_ = i;
      inspectorOpen_ = true;
    }
  }
  ImGui::PopStyleVar();
  ImGui::SameLine(ImGui::GetWindowWidth() - 10 - kTitleH);
  if (ui::iconButton("##fold", inspectorOpen_ ? Icon::ChevronDown : Icon::ChevronRight, false, inspectorOpen_ ? "Collapse" : "Expand", kTitleH, 1.0f))
    inspectorOpen_ = !inspectorOpen_;
  if (inspectorOpen_) {
    ImGui::BeginChild("content");
    if (rightTab_ == 0) drawInspector();
    else if (rightTab_ == 1) drawSettings();
    else drawPalette();
    ImGui::EndChild();
  }
  ImGui::End();
}

void Editor::drawInspector() {
  if (selected_ == kNoEntity) {
    ImGui::Dummy(ImVec2(0, 30));
    ImGui::PushStyleColor(ImGuiCol_Text, col::kDim);
    ImGui::TextWrapped("Nothing selected.");
    ImGui::PopStyleColor();
    ImGui::PushStyleColor(ImGuiCol_Text, col::kFaint);
    ImGui::TextWrapped("Click something in the viewport or the Scene list, or press %s to find anything by name.", modKey("K").c_str());
    ImGui::PopStyleColor();
    return;
  }
  bool finished = false;
  // Title: type icon and the name, edited in place.
  ImVec2 p = ImGui::GetCursorScreenPos();
  ui::icon(ImGui::GetWindowDrawList(), ImVec2(p.x + 10, p.y + ImGui::GetFrameHeight() * 0.5f), entityIcon(selected_), col::kAccent, 2.0f);
  ImGui::SetCursorScreenPos(ImVec2(p.x + 26, p.y));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0));
  ImGui::SetNextItemWidth(-1);
  ImGui::InputText("##name", &scene_.reg.get<Name>(selected_).value);
  testRect("field:Name");
  finished |= ImGui::IsItemDeactivatedAfterEdit();
  ImGui::PopStyleColor();
  // Where it sits, and how many more are selected.
  std::string where;
  Entity parent = scene_.reg.get<Transform>(selected_).parent;
  if (scene_.reg.valid(parent)) where = "Child of " + scene_.reg.get<Name>(parent).value;
  if (!extraSelection_.empty())
    where += (where.empty() ? "" : ". ") + std::to_string(extraSelection_.size()) + " more selected: the gizmo, delete, duplicate and copy apply to all";
  if (!where.empty()) {
    ImGui::PushStyleColor(ImGuiCol_Text, col::kFaint);
    ImGui::TextWrapped("%s", where.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::Dummy(ImVec2(0, 4));

  static const std::map<std::string, Icon> icons = {{"Transform", Icon::Move},      {"MeshRenderer", Icon::Mesh}, {"SpriteRenderer", Icon::Sprite},
                                                    {"Camera", Icon::Camera},       {"Light", Icon::Sun},         {"Collider", Icon::Collider},
                                                    {"Script", Icon::Script}};
  auto iconOf = [&](const char* name) {
    auto it = icons.find(name);
    return it == icons.end() ? Icon::Empty : it->second;
  };
  for (const ComponentType& type : componentTypes()) {
    void* component = type.get(scene_.reg, selected_);
    if (!component) continue;
    ImGui::PushID(type.name);
    bool remove = false;
    bool removable = strcmp(type.name, "Transform") != 0;
    bool open = sectionHeader(type.name, iconOf(type.name), removable ? &remove : nullptr);
    if (ImGui::BeginPopupContextItem("component")) {
      if (ImGui::MenuItem("Reset to defaults")) {
        const void* d = componentDefaults(type);
        for (const Field& f : type.fields) fieldCopy(f, (u8*)component + f.offset, (const u8*)d + f.offset);
        finished = true;
      }
      if (removable && ImGui::MenuItem("Remove")) remove = true;
      ImGui::EndPopup();
    }
    if (open && !remove) {
      ImGui::Indent(4);
      finished |= editFields(component, type.fields, componentDefaults(type));
      if (strcmp(type.name, "Script") == 0) finished |= drawScriptButtons(*(Script*)component);
      ImGui::Unindent(4);
    }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PopID();
    if (remove) {
      type.remove(scene_.reg, selected_);
      finished = true;
    }
  }

  // Add component: a searchable list (type, then Enter).
  if (ImGui::Button("+  Add component", ImVec2(-1, 28))) {
    componentQuery_.clear();
    ImGui::OpenPopup("add_component");
  }
  ImGui::SetNextWindowSize(ImVec2(ImGui::GetItemRectSize().x, 0));
  if (ImGui::BeginPopup("add_component")) {
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    ImGui::SetNextItemWidth(-1);
    bool enter = ImGui::InputTextWithHint("##q", "Search components", &componentQuery_, ImGuiInputTextFlags_EnterReturnsTrue);
    for (const ComponentType& type : componentTypes()) {
      if (type.get(scene_.reg, selected_) || ui::fuzzyScore(componentQuery_, type.name) < 0) continue;
      std::string label = "      " + ui::humanize(type.name);
      ImVec2 p2 = ImGui::GetCursorScreenPos();
      bool pick = ImGui::Selectable(label.c_str()) || std::exchange(enter, false);
      ui::icon(ImGui::GetWindowDrawList(), ImVec2(p2.x + 8, p2.y + ImGui::GetTextLineHeight() * 0.5f), iconOf(type.name), col::kDim);
      if (pick) {
        type.add(scene_.reg, selected_);
        finished = true;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  }
  if (finished && !playing_) commit();
}

void Editor::drawSettings() {
  ImGui::PushStyleColor(ImGuiCol_Text, col::kFaint);
  ImGui::TextWrapped("Saved with the scene. Hover a setting's name for what it does.");
  ImGui::PopStyleColor();
  RenderSettings& rs = scene_.settings;
  static const RenderSettings defaults{};
  struct Item {
    const char* name;
    const char* tip;
  };
  struct Group {
    const char* title;
    Icon icon;
    std::vector<Item> items;
  };
  // Every RenderSettings field, grouped and explained. Fields missing here still
  // show up under "Other", so adding a setting never hides it.
  static const std::vector<Group> groups = {
      {"Pixels",
       Icon::Grid,
       {{"worldPixels", "Pixels belong to the 3D world (texel splatting): they stay put when a perspective camera turns or moves. "
                        "Orthographic cameras use screen pixels instead."},
        {"probeResolution", "World pixels: pixels per cube-map face (90 degrees of view). Lower = bigger, chunkier pixels."},
        {"probeCell", "World pixels: how far (world units) the camera moves before the pixel grid re-centres, with a short dissolve."},
        {"pixelArt", "Screen pixels: render a low-resolution image and scale it up with hard edges."},
        {"pixelHeight", "Screen pixels: height of that image in pixels. Also sets the size of on-screen text in every mode."},
        {"integerScale", "Screen pixels: make every game pixel the same whole number of screen pixels (adjusts the height slightly)."},
        {"pixelSnap", "Screen pixels with an orthographic camera: snap the camera to the pixel grid so nothing shimmers."}}},
      {"Shading",
       Icon::Sun,
       {{"toonBands", "Light comes in this many hard steps (0 = smooth)."},
        {"dither", "Ordered dithering between light steps and palette colors (0 = none)."},
        {"texelLighting", "Light and shadow follow the texture's pixel grid instead of the screen's."},
        {"texelsPerUnit", "Texture pixels per world unit, for meshes with worldTexels ticked."},
        {"shadows", "Sun shadows (raster). Path tracing always has shadows."},
        {"outlines", "Dark outlines on silhouettes."},
        {"outlineStrength", "How dark outlines are (0..1)."},
        {"highlightStrength", "Light edge on convex creases (0 = off)."}}},
      {"Palette",
       Icon::Image,
       {{"palette", "Image whose colors are the only ones allowed on screen (up to 64). Edit it in the Palette tab."},
        {"rampShading", "Shade by stepping along the palette's rows (color ramps) instead of darkening colors."},
        {"rampShadowSteps", "How many ramp colors darker a surface gets in full shadow."}}},
      {"Sky and light",
       Icon::Bulb,
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
       Icon::Frame,
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
    finished |= editFields(&rs, {fields[i]}, &defaults, tip);
    ImGui::EndDisabled();
  };
  for (const Group& g : groups) {
    ImGui::Dummy(ImVec2(0, 2));
    bool open = sectionHeader(g.title, g.icon, nullptr);
    for (const Item& it : g.items)
      for (size_t i = 0; i < fields.size(); i++)
        if (it.name == std::string(fields[i].name)) {
          if (open) drawField(i, it.tip);
          else shown[i] = true;
        }
  }
  bool other = false;
  for (size_t i = 0; i < fields.size(); i++) {
    if (shown[i]) continue;
    if (!other) other = sectionHeader("Other", Icon::Empty, nullptr);
    if (other) drawField(i, nullptr);
  }
  if (finished && !playing_) commit();
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
  if (assets_.setTexturePixels(palettePath_, width, height, pixels.data())) {
    renderer_.clearTextureCaches();
    app_.device->waitIdle();
    ui_->forgetAllTextures(); // the old texture's id can come back for another one
  }
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
  if (scene_.settings.palette != palettePath_) loadPalette();
  if (palettePath_.empty()) {
    ImGui::Dummy(ImVec2(0, 10));
    ImGui::TextWrapped("This scene has no palette. With one, every color on screen snaps to it, and "
                       "ramp shading steps along its rows.");
    if (ImGui::Button("Create palette")) createPalette();
    return;
  }
  ImGui::TextUnformatted(palettePath_.c_str());
  if (paletteDirty_) {
    ImGui::SameLine();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kAccent), "unsaved");
  }
  ImGui::PushStyleColor(ImGuiCol_Text, col::kFaint);
  ImGui::TextWrapped("Each row is a ramp, darkest on the left. Click a swatch to edit it.");
  ImGui::PopStyleColor();
  bool changed = false;
  int removeRow = -1;
  const f32 swatch = 26.0f;
  for (int r = 0; r < (int)paletteRows_.size(); r++) {
    ImGui::PushID(r);
    auto& row = paletteRows_[r];
    for (int c = 0; c < (int)row.size(); c++) {
      ImGui::PushID(c);
      if (c > 0) ImGui::SameLine(0.0f, 2.0f);
      changed |= ImGui::ColorEdit3("##c", &row[c].x, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel | ImGuiColorEditFlags_NoBorder);
      ImGui::PopID();
    }
    ImGui::SameLine(0, 8);
    if (ui::iconButton("##add", Icon::Plus, false, "Add a color to this ramp", swatch * 0.8f, 1.0f)) {
      row.push_back(row.empty() ? vec4(0.5f, 0.5f, 0.5f, 1.0f) : row.back());
      changed = true;
    }
    ImGui::SameLine(0, 0);
    if (ui::iconButton("##remove", Icon::Close, false, "Remove the last color", swatch * 0.8f, 1.0f) && !row.empty()) {
      row.pop_back();
      changed = true;
      if (row.empty()) removeRow = r;
    }
    ImGui::PopID();
  }
  if (removeRow >= 0) paletteRows_.erase(paletteRows_.begin() + removeRow);
  ImGui::Dummy(ImVec2(0, 4));
  if (ImGui::Button("Add ramp")) {
    paletteRows_.push_back({vec4(0.2f, 0.2f, 0.25f, 1), vec4(0.45f, 0.45f, 0.5f, 1), vec4(0.75f, 0.75f, 0.78f, 1)});
    changed = true;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(!paletteDirty_ && !changed);
  if (ImGui::Button("Save")) savePalette();
  ImGui::SameLine();
  if (ImGui::Button("Revert")) {
    reloadAssets(); // re-reads every file, including the palette
    loadPalette();
  }
  ImGui::EndDisabled();
  if (changed) applyPalette();
}

// ---------------------------------------------------------------- drawer

void Editor::drawDrawer() {
  if (drawerTab_ < 0) return;
  ImGuiViewport* vp = ImGui::GetMainViewport();
  f32 x0 = leftEdge_ + kGap, w = std::max(240.0f, rightEdge_ - kGap - x0);
  ImGui::SetNextWindowPos(ImVec2(x0, vp->WorkPos.y + vp->WorkSize.y - kGap), ImGuiCond_Always, ImVec2(0, 1));
  ImGui::SetNextWindowSize(ImVec2(w, 300), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(w, 150), ImVec2(w, std::max(150.0f, vp->WorkSize.y * 0.75f)));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
  ImGui::Begin("Drawer##card", nullptr, kCardFlags);
  ImGui::PopStyleVar();
  cardBackground();
  if (drawerTab_ == 1) unseenErrors_ = 0;
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 6));
  if (tabButton("ASSETS", drawerTab_ == 0)) drawerTab_ = lastDrawerTab_ = 0;
  ImGui::SameLine();
  if (tabButton("CONSOLE", drawerTab_ == 1)) drawerTab_ = lastDrawerTab_ = 1;
  ImGui::PopStyleVar();
  ImGui::SameLine(ImGui::GetWindowWidth() - 10 - kTitleH);
  if (ui::iconButton("##close", Icon::Close, false, ("Close (" + modKey("J") + ")").c_str(), kTitleH, 1.0f)) drawerTab_ = -1;
  if (drawerTab_ == 0) drawAssets();
  else if (drawerTab_ == 1) drawConsole();
  ImGui::End();
}

void Editor::openAsset(const std::string& f) {
  AssetKind kind = assetKind(f);
  if (kind == AssetKind::Scene) openScene(f);
  else if (kind == AssetKind::Prefab && !playing_) {
    placePrefab(f);
  } else if (kind == AssetKind::Script) openInCodeEditor(project_.path(f));
}

void Editor::drawAssets() {
  // Folders on the left.
  std::set<std::string> folders;
  for (const std::string& f : assetFiles_)
    for (size_t slash = f.find('/'); slash != std::string::npos; slash = f.find('/', slash + 1)) folders.insert(f.substr(0, slash));
  ImGui::BeginChild("folders", ImVec2(170, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8, 1));
  auto folderRow = [&](const std::string& path, const char* label, int depth) {
    ImGui::PushID(path.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##folder", assetFolder_ == path && assetFilter_.empty(), 0, ImVec2(0, ImGui::GetFrameHeight()))) {
      assetFolder_ = path;
      assetFilter_.clear();
    }
    f32 x = p.x + 6 + depth * 12.0f, cy = p.y + ImGui::GetFrameHeight() * 0.5f;
    ui::icon(ImGui::GetWindowDrawList(), ImVec2(x + 4, cy), Icon::Folder, col::kAccent);
    ImGui::GetWindowDrawList()->AddText(ImVec2(x + 14, cy - ImGui::GetFontSize() * 0.5f), col::kText, label);
    ImGui::PopID();
  };
  folderRow("", project_.name.c_str(), 0);
  for (const std::string& dir : folders) {
    int depth = 1 + (int)std::count(dir.begin(), dir.end(), '/');
    folderRow(dir, fileName(dir).c_str(), depth);
  }
  ImGui::PopStyleVar();
  ImGui::EndChild();
  ImGui::SameLine();

  ImGui::BeginChild("files");
  // Toolbar: breadcrumb, search, view mode.
  {
    std::string crumb = project_.name;
    for (size_t start = 0; start < assetFolder_.size();) {
      size_t slash = std::min(assetFolder_.find('/', start), assetFolder_.size());
      crumb += "  /  " + assetFolder_.substr(start, slash - start);
      start = slash + 1;
    }
    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kDim), "%s", assetFilter_.empty() ? crumb.c_str() : "Search results");
    f32 searchW = 200.0f, toolsW = searchW + 2 * ImGui::GetFrameHeight() + (assetGrid_ ? 110.0f : 0.0f) + 12;
    ImGui::SameLine(std::max(ImGui::GetCursorPosX() + 10, ImGui::GetWindowWidth() - toolsW));
    if (assetGrid_) {
      ImGui::SetNextItemWidth(100);
      ImGui::SliderFloat("##size", &assetTile_, 56.0f, 160.0f, "");
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("Thumbnail size");
      ImGui::SameLine(0, 8);
    }
    ImGui::SetNextItemWidth(searchW);
    ImGui::InputTextWithHint("##filter", "Search all files", &assetFilter_);
    ImGui::SameLine(0, 4);
    f32 fh = ImGui::GetFrameHeight();
    if (ui::iconButton("##grid", Icon::Grid, assetGrid_, "Thumbnails", fh, 1.0f)) assetGrid_ = true;
    ImGui::SameLine(0, 0);
    if (ui::iconButton("##list", Icon::List, !assetGrid_, "List", fh, 1.0f)) assetGrid_ = false;
  }

  // What to show: a search over every file, or this folder's subfolders and files.
  struct Item {
    std::string path;
    bool folder;
  };
  std::vector<Item> items;
  if (!assetFilter_.empty()) {
    std::vector<std::pair<int, const std::string*>> hits;
    for (const std::string& f : assetFiles_)
      if (int s = ui::fuzzyScore(assetFilter_, f); s >= 0) hits.push_back({s, &f});
    std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (auto& [s, f] : hits) items.push_back({*f, false});
  } else {
    std::string prefix = assetFolder_.empty() ? "" : assetFolder_ + "/";
    for (const std::string& dir : folders)
      if (dir.rfind(prefix, 0) == 0 && dir.find('/', prefix.size()) == std::string::npos && dir.size() > prefix.size()) items.push_back({dir, true});
    for (const std::string& f : assetFiles_)
      if (f.rfind(prefix, 0) == 0 && f.find('/', prefix.size()) == std::string::npos) items.push_back({f, false});
  }

  ImGui::BeginChild("items");
  ImGui::PushStyleColor(ImGuiCol_Text, col::kFaint);
  ImGui::TextUnformatted("Drag onto the viewport to place, onto an entity to apply, or onto a field. Double-click opens scenes, scripts and prefabs.");
  ImGui::PopStyleColor();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const f32 spacing = 8.0f, labelH = ImGui::GetTextLineHeight() + 6;
  int columns = std::max(1, (int)((ImGui::GetContentRegionAvail().x + spacing) / (assetTile_ + spacing)));
  for (size_t i = 0; i < items.size(); i++) {
    const std::string& path = items[i].path;
    bool folder = items[i].folder;
    AssetKind kind = folder ? AssetKind::Other : assetKind(path);
    ImGui::PushID(path.c_str());
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImVec2 size = assetGrid_ ? ImVec2(assetTile_, assetTile_ + labelH) : ImVec2(ImGui::GetContentRegionAvail().x, ImGui::GetFrameHeight());
    if (assetGrid_ && i % columns) {
      ImGui::SameLine(0, spacing);
      p = ImGui::GetCursorScreenPos();
    }
    ImGui::InvisibleButton("##item", size);
    testRect((folder ? "folder:" : "asset:") + path);
    bool hovered = ImGui::IsItemHovered(), selected = assetSelected_ == path;
    if (ImGui::IsItemClicked()) assetSelected_ = path;
    if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
      if (folder) {
        assetFolder_ = path;
        assetFilter_.clear();
      } else {
        openAsset(path);
      }
    }
    if (!folder && ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("ACE_ASSET", path.c_str(), path.size() + 1);
      ImGui::TextUnformatted(path.c_str());
      ImGui::EndDragDropSource();
    }
    if (hovered && !folder) ImGui::SetItemTooltip("%s", path.c_str());
    ImVec2 q(p.x + size.x, p.y + size.y);
    if (selected || hovered) dl->AddRectFilled(p, q, selected ? col::kAccentSoft : col::kHover);
    std::string name = folder ? fileName(path) : (kind == AssetKind::Scene || kind == AssetKind::Prefab ? stem(path) : fileName(path));
    if (assetGrid_) {
      // Thumbnail: the image itself (nearest-filtered), otherwise a big type icon.
      ImVec2 a(p.x + 6, p.y + 6), b(q.x - 6, p.y + assetTile_ - 6);
      dl->AddRectFilled(a, b, col::kRaised);
      if (kind == AssetKind::Image) {
        const Texture* t = assets_.texture(path);
        f32 bw = b.x - a.x - 8, bh = b.y - a.y - 8, s = std::min(bw / std::max(1u, t->width), bh / std::max(1u, t->height));
        if (s >= 1.0f) s = floorf(s); // whole-pixel scaling keeps pixel art even
        ImVec2 c((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), half(t->width * s * 0.5f, t->height * s * 0.5f);
        dl->AddImage(ImGuiRhi::textureId(t->handle), ImVec2(floorf(c.x - half.x), floorf(c.y - half.y)), ImVec2(floorf(c.x - half.x) + half.x * 2, floorf(c.y - half.y) + half.y * 2));
      } else {
        ui::icon(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), folder ? Icon::Folder : assetIcon(kind), folder ? col::kAccent : assetInk(kind),
                 std::max(2.0f, floorf(assetTile_ / 26.0f)));
      }
      if (!folder) { // type tag, top-left
        std::string tag = assetTag(path);
        f32 ps = fontSize(ui::fonts.pixel);
        ImVec2 ts = ui::fonts.pixel->CalcTextSizeA(ps, FLT_MAX, 0, tag.c_str());
        dl->AddRectFilled(a, ImVec2(a.x + ts.x + 6, a.y + ps + 2), col::kBackdrop);
        dl->AddText(ui::fonts.pixel, ps, ImVec2(a.x + 3, a.y + 2), assetInk(kind), tag.c_str());
      }
      std::string label = fitText(name, size.x - 6);
      f32 lw = ImGui::CalcTextSize(label.c_str()).x;
      dl->AddText(ImVec2(p.x + (size.x - lw) * 0.5f, p.y + assetTile_ - 2), selected ? col::kAccent : col::kText, label.c_str());
    } else {
      f32 cy = p.y + size.y * 0.5f;
      ui::icon(dl, ImVec2(p.x + 10, cy), folder ? Icon::Folder : assetIcon(kind), folder ? col::kAccent : assetInk(kind));
      dl->AddText(ImVec2(p.x + 22, cy - ImGui::GetFontSize() * 0.5f), selected ? col::kAccent : col::kText, (assetFilter_.empty() ? name : path).c_str());
    }
    ImGui::PopID();
  }
  if (items.empty()) ImGui::TextDisabled(assetFilter_.empty() ? "This folder is empty." : "No file matches.");
  ImGui::EndChild();
  ImGui::EndChild();
}

void Editor::drawConsole() {
  size_t counts[3] = {0, 0, 0};
  for (auto& [level, line] : console_) counts[(int)level]++;
  if (ImGui::Button("Clear")) console_.clear();
  static const char* const names[] = {"Info", "Warnings", "Errors"};
  static const ImU32 inks[] = {col::kText, col::kWarn, col::kError};
  for (int i = 0; i < 3; i++) {
    ImGui::SameLine(0, 4);
    std::string label = std::string(names[i]) + "  " + std::to_string(counts[i]);
    if (chip(label.c_str(), consoleShow_[i], "Show or hide these lines", consoleShow_[i] && i && counts[i] ? inks[i] : 0)) consoleShow_[i] = !consoleShow_[i];
  }
  ImGui::SameLine(0, 12);
  ImGui::SetNextItemWidth(std::min(260.0f, ImGui::GetContentRegionAvail().x));
  ImGui::InputTextWithHint("##filter", "Filter lines", &consoleFilter_);
  ImGui::BeginChild("lines", ImVec2(0, 0), ImGuiChildFlags_None, ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PushFont(ui::fonts.mono);
  for (auto& [level, line] : console_) {
    if (!consoleShow_[(int)level]) continue;
    if (!consoleFilter_.empty() && ui::fuzzyScore(consoleFilter_, line) < 0) continue;
    ImGui::PushStyleColor(ImGuiCol_Text, inks[(int)level]);
    ImGui::TextUnformatted(line.c_str());
    ImGui::PopStyleColor();
  }
  ImGui::PopFont();
  if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
  ImGui::EndChild();
}

// ---------------------------------------------------------------- command palette

void Editor::drawCommandPalette() {
  if (std::exchange(commandsOpen_, false)) {
    commandQuery_.clear();
    commandIndex_ = 0;
    ImGui::OpenPopup("##commands");
  }
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + 48), ImGuiCond_Always, ImVec2(0.5f, 0));
  ImGui::SetNextWindowSize(ImVec2(std::min(600.0f, vp->WorkSize.x - 40), 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 10));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, col::kCard);
  bool open = ImGui::BeginPopup("##commands");
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
  if (!open) return;

  // Everything the palette can do. Entities and files join once something is typed.
  std::vector<Command> all;
  auto add = [&](std::string label, std::string note, Icon which, std::function<void()> run) { all.push_back({std::move(label), std::move(note), which, std::move(run)}); };
  add(playing_ ? "Stop" : "Play", "F5", playing_ ? Icon::Stop : Icon::Play, [this] { setPlaying(!playing_); });
  add("Save scene", modKey("S"), Icon::Scene, [this] { saveScene(); });
  add("Save scene as...", "", Icon::Scene, [this] { openSaveAs_ = true; });
  add("New scene", "", Icon::Scene, [this] { newScene(); });
  add("Undo", modKey("Z"), Icon::Reset, [this] { undo(); });
  add("Redo", modKey("Shift+Z"), Icon::Reset, [this] { redo(); });
  if (!playing_) {
    for (int i = 0; i < (int)std::size(kCreateLabels); i++)
      add(std::string("Create ") + kCreateLabels[i], "Create", Icon::Plus, [this, i] {
        createItem(i);
        commit();
      });
    if (selected_ != kNoEntity) {
      add("Duplicate selection", modKey("D"), Icon::Prefab, [] { g_duplicateRequest = true; });
      add("Delete selection", "Del", Icon::Close, [] { g_deleteRequest = true; });
      add("Frame selection", "F", Icon::Frame, [this] { frameSelection(); });
      add("Rename selection", "F2", Icon::File, [this] {
        renaming_ = selected_;
        renameText_ = scene_.reg.get<Name>(selected_).value;
      });
    }
    add("Select all", modKey("A"), Icon::Select, [this] { selectAll(orderedEntities(scene_)); });
  }
  add("Tool: select", "Q", Icon::Select, [this] { gizmoOp_ = -1, buildMode_ = false; });
  add("Tool: move", "W", Icon::Move, [this] { gizmoOp_ = 0, buildMode_ = false; });
  add("Tool: rotate", "E", Icon::Rotate, [this] { gizmoOp_ = 1, buildMode_ = false; });
  add("Tool: scale", "R", Icon::Scale, [this] { gizmoOp_ = 2, buildMode_ = false; });
  add("Toggle build mode", "B", Icon::Build, [this] { buildMode_ = !buildMode_; });
  add("Toggle snapping", "", Icon::Snap, [this] { snap_ = !snap_; });
  add("Toggle orthographic view", "", Icon::Camera, [this] { camera_.orthographic = !camera_.orthographic; });
  auto setting = [&](const char* label, bool RenderSettings::*field) {
    add(label, "Render", Icon::Sun, [this, field] {
      scene_.settings.*field = !(scene_.settings.*field);
      if (!playing_) commit();
    });
  };
  setting("Toggle path tracing", &RenderSettings::pathTraced);
  setting("Toggle world pixels", &RenderSettings::worldPixels);
  setting("Toggle screen pixels (pixel art)", &RenderSettings::pixelArt);
  setting("Toggle shadows", &RenderSettings::shadows);
  setting("Toggle outlines", &RenderSettings::outlines);
  add("Show inspector", "", Icon::Select, [this] { inspectorOpen_ = true, rightTab_ = 0; });
  add("Show render settings", "", Icon::Sun, [this] { inspectorOpen_ = true, rightTab_ = 1; });
  add("Show palette", "", Icon::Image, [this] { inspectorOpen_ = true, rightTab_ = 2; });
  add("Show assets", modKey("J"), Icon::Folder, [this] { drawerTab_ = lastDrawerTab_ = 0; });
  add("Show console", "", Icon::Console, [this] { drawerTab_ = lastDrawerTab_ = 1; });
  add("Hide or show panels", modKey("\\"), Icon::Grid, [this] { panelsHidden_ = !panelsHidden_; });
  add("Reload assets", "", Icon::File, [this] { reloadAssets(); });
  if (!commandQuery_.empty()) {
    for (auto [e, n] : scene_.reg.view<Name>().each())
      add(n.value, "Entity", entityIcon(e), [this, e = e] {
        selectAll({e});
        frameSelection();
      });
    for (const std::string& f : assetFiles_) {
      AssetKind kind = assetKind(f);
      const char* verb = kind == AssetKind::Scene ? "Open scene" : kind == AssetKind::Prefab ? "Place prefab" : kind == AssetKind::Script ? "Edit script" : "Show file";
      add(f, verb, assetIcon(kind), [this, f, kind] {
        if (kind == AssetKind::Scene || kind == AssetKind::Prefab || kind == AssetKind::Script) {
          openAsset(f);
        } else {
          drawerTab_ = lastDrawerTab_ = 0;
          assetFilter_.clear();
          assetFolder_ = f.find('/') == std::string::npos ? "" : f.substr(0, f.find_last_of('/'));
          assetSelected_ = f;
        }
      });
    }
  }

  if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
  ImGui::SetNextItemWidth(-1);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10, 8));
  bool enter = ImGui::InputTextWithHint("##query", "Type an action, an entity or a file name", &commandQuery_, ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::PopStyleVar();
  testRect("commands:query");
  if (ImGui::IsItemEdited()) commandIndex_ = 0;

  std::vector<Command*> matches;
  for (Command& c : all)
    if ((c.score = ui::fuzzyScore(commandQuery_, c.label)) >= 0) matches.push_back(&c);
  std::stable_sort(matches.begin(), matches.end(), [](Command* a, Command* b) { return a->score > b->score; });
  int count = (int)matches.size();
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) commandIndex_ = std::min(commandIndex_ + 1, count - 1);
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) commandIndex_ = std::max(commandIndex_ - 1, 0);
  commandIndex_ = std::clamp(commandIndex_, 0, std::max(0, count - 1));

  Command* run = nullptr;
  if (enter && count > 0) run = matches[commandIndex_];
  const f32 rowH = ImGui::GetFrameHeight() + 2;
  ImGui::BeginChild("##list", ImVec2(0, std::min(count, 10) * (rowH + ImGui::GetStyle().ItemSpacing.y) + 2));
  for (int i = 0; i < count; i++) {
    ImGui::PushID(i);
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::Selectable("##row", i == commandIndex_, 0, ImVec2(0, rowH))) run = matches[i];
    if (i == commandIndex_ && (ImGui::IsKeyPressed(ImGuiKey_DownArrow) || ImGui::IsKeyPressed(ImGuiKey_UpArrow))) ImGui::SetScrollHereY();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    f32 cy = p.y + rowH * 0.5f, fs = ImGui::GetFontSize();
    ui::icon(dl, ImVec2(p.x + 10, cy), matches[i]->icon, i == commandIndex_ ? col::kAccent : col::kDim);
    dl->AddText(ImVec2(p.x + 24, cy - fs * 0.5f), col::kText, matches[i]->label.c_str());
    f32 nw = ImGui::CalcTextSize(matches[i]->note.c_str()).x;
    dl->AddText(ImVec2(p.x + ImGui::GetContentRegionAvail().x - nw - 6, cy - fs * 0.5f), col::kFaint, matches[i]->note.c_str());
    ImGui::PopID();
  }
  ImGui::EndChild();
  if (count == 0) ImGui::TextDisabled("Nothing matches. Try fewer letters.");
  if (run) {
    ImGui::CloseCurrentPopup();
    run->run();
  } else if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
  }
  ImGui::EndPopup();
}

// ---------------------------------------------------------------- viewport

void Editor::viewRay(vec2 uv, vec3& origin, vec3& end) {
  CameraView cv = camera_.view();
  mat4 invViewProj = glm::inverse(cv.camera.projection((f32)viewportW_ / (f32)viewportH_) * glm::inverse(cv.world));
  vec2 ndc{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f};
  vec4 nearP = invViewProj * vec4(ndc, 0.0f, 1.0f), farP = invViewProj * vec4(ndc, 1.0f, 1.0f);
  origin = vec3(nearP) / nearP.w;
  end = vec3(farP) / farP.w;
}

bool Editor::raycast(vec2 uv, Entity& entity, vec3& point, vec3& normal, bool forPicking) {
  vec3 origin, end;
  viewRay(uv, origin, end);
  entity = kNoEntity;
  f32 bestT = 2.0f; // t is in 0..1 along origin->end
  // Tests against mesh bounds, not triangles.
  // shortcut: exact for boxes and planes (what levels are built from); loose for
  // spheres and imported models. Trace triangles if picking them feels sloppy.
  auto test = [&](Entity e, const mat4& world, Aabb box) {
    if (forPicking && isLocked(e)) return;
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
  raycast(uv, e, point, normal, true);
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

void Editor::dropAsset(const std::string& f, Entity hit, vec3 point) {
  AssetKind kind = assetKind(f);
  if (kind == AssetKind::Scene) {
    openScene(f);
    return;
  }
  if (playing_) return;
  vec3 at = point;
  if (snap_ && snapMove_ > 0.0f) {
    at.x = roundf(at.x / snapMove_) * snapMove_;
    at.z = roundf(at.z / snapMove_) * snapMove_;
  }
  if (kind == AssetKind::Prefab) {
    placePrefab(f, &at);
  } else if (kind == AssetKind::Mesh) {
    createEntity(stem(f).c_str(), f.c_str(), &at);
    commit();
  } else if (kind == AssetKind::Image) { // a texture for what is under the cursor, or a new sprite
    if (MeshRenderer* m = hit == kNoEntity ? nullptr : scene_.reg.try_get<MeshRenderer>(hit)) m->texture = f;
    else if (SpriteRenderer* s = hit == kNoEntity ? nullptr : scene_.reg.try_get<SpriteRenderer>(hit)) s->texture = f;
    else {
      at.y += 0.5f; // sprites are centred on their position
      hit = createEntity(stem(f).c_str(), nullptr, &at);
      scene_.reg.emplace<SpriteRenderer>(hit).texture = f;
    }
    selected_ = hit;
    commit();
  } else if (kind == AssetKind::Script && hit != kNoEntity) {
    scene_.reg.emplace_or_replace<Script>(hit).path = f;
    selected_ = hit;
    commit();
  }
}

void Editor::drawToolRail() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(leftEdge_ + kGap, vp->WorkPos.y + kGap));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
  ImGui::Begin("##tools", nullptr, kFloatFlags);
  ImGui::PopStyleVar(2);
  cardBackground();
  ImGui::BeginDisabled(playing_);
  struct Tool {
    Icon icon;
    int op;
    const char* tip;
  };
  static const Tool tools[] = {{Icon::Select, -1, "Select (Q)"}, {Icon::Move, 0, "Move (W)"}, {Icon::Rotate, 1, "Rotate (E)"}, {Icon::Scale, 2, "Scale (R)"}};
  for (const Tool& t : tools)
    if (ui::iconButton(t.tip, t.icon, !buildMode_ && gizmoOp_ == t.op, t.tip, 32)) {
      gizmoOp_ = t.op;
      buildMode_ = false;
    }
  ImGui::Dummy(ImVec2(32, 4));
  if (ui::iconButton("##build", Icon::Build, buildMode_, "Build (B): click a surface to add a block, Shift+click to remove one", 32)) buildMode_ = !buildMode_;
  std::string snapTip = "Snap moves to " + std::to_string(snapMove_).substr(0, 6) + " units, rotations to 15 degrees, scale to 0.25. Right-click to change the step.";
  if (ui::iconButton("##snap", Icon::Snap, snap_, snapTip.c_str(), 32)) snap_ = !snap_;
  if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) ImGui::OpenPopup("snap");
  if (ImGui::BeginPopup("snap")) {
    ImGui::TextUnformatted("Move step (world units)");
    ImGui::SetNextItemWidth(140);
    ImGui::DragFloat("##snapstep", &snapMove_, 0.0125f, 0.0125f, 10.0f, "%.4g");
    ImGui::EndPopup();
  }
  ImGui::Dummy(ImVec2(32, 4));
  ImGui::BeginDisabled(selected_ == kNoEntity);
  if (ui::iconButton("##frame", Icon::Frame, false, "Frame the selection (F)", 32)) frameSelection();
  ImGui::EndDisabled();
  ImGui::EndDisabled();
  ImVec2 railPos = ImGui::GetWindowPos(), railSize = ImGui::GetWindowSize();
  ImGui::End();

  if (!buildMode_ || playing_) return;
  // Build options float beside the rail while Build is on.
  ImGui::SetNextWindowPos(ImVec2(railPos.x + railSize.x + 6, railPos.y));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 8));
  ImGui::Begin("##build", nullptr, kFloatFlags);
  ImGui::PopStyleVar();
  cardBackground();
  cardTitle("BUILD", nullptr);
  ImGui::SetNextItemWidth(190);
  if (ImGui::BeginCombo("Texture", brushTexture_.empty() ? "(none)" : fileName(brushTexture_).c_str())) {
    if (ImGui::Selectable("(none)", brushTexture_.empty())) brushTexture_.clear();
    for (const std::string& f : assetFiles_)
      if (assetKind(f) == AssetKind::Image && ImGui::Selectable(f.c_str(), f == brushTexture_)) brushTexture_ = f;
    ImGui::EndCombo();
  }
  ImGui::SetNextItemWidth(190);
  ImGui::DragFloat3("Size", &brushSize_.x, 0.0625f, 0.0625f, 64.0f, "%.4g");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Block size, also the placement grid (0.25 = 4 texels at 16 texels per unit)");
  ImGui::End();
}

void Editor::drawViewOverlay() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(rightEdge_ - kGap, vp->WorkPos.y + kGap), ImGuiCond_Always, ImVec2(1, 0));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(4, 4));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(2, 0));
  ImGui::Begin("##view", nullptr, kFloatFlags);
  cardBackground();
  if (chip("Persp", !camera_.orthographic, "Perspective view, as the game camera usually sees it")) camera_.orthographic = false;
  ImGui::SameLine();
  if (chip("Ortho", camera_.orthographic, "Orthographic view: no perspective, good for lining things up")) camera_.orthographic = true;
  ImGui::SameLine(0, 10);
  if (chip("Path traced", scene_.settings.pathTraced, "Path-trace the scene (saved with it): soft shadows and bounce light")) {
    scene_.settings.pathTraced = !scene_.settings.pathTraced;
    if (!playing_) commit();
  }
  ImGui::SameLine(0, 10);
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(col::kFaint), "%.0f fps  ", ImGui::GetIO().Framerate);
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void Editor::drawViewport() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::PushStyleColor(ImGuiCol_WindowBg, col::kBackdrop);
  ImGui::Begin("##viewport", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus |
                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoScrollWithMouse);
  ImGui::PopStyleColor();
  ImGui::PopStyleVar();
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
    vec2 mouseUv((io.MousePos.x - pos.x) / size.x, (io.MousePos.y - pos.y) / size.y);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    CameraView cv = camera_.view();
    mat4 viewProj = cv.camera.projection(size.x / size.y) * glm::inverse(cv.world);
    auto toScreen = [&](vec3 p, ImVec2& out) {
      vec4 clip = viewProj * vec4(p, 1.0f);
      if (clip.w <= 0.001f) return false;
      out = ImVec2(pos.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - clip.y / clip.w * 0.5f) * size.y);
      return out.x >= pos.x && out.y >= pos.y && out.x <= pos.x + size.x && out.y <= pos.y + size.y;
    };
    auto wireBox = [&](const Aabb& box, ImU32 color) {
      ImVec2 corner[8];
      for (int c = 0; c < 8; c++) {
        vec4 clip = viewProj * vec4(c & 1 ? box.max.x : box.min.x, c & 2 ? box.max.y : box.min.y, c & 4 ? box.max.z : box.min.z, 1.0f);
        if (clip.w <= 0.001f) return; // partly behind the camera: skip rather than draw garbage
        corner[c] = ImVec2(pos.x + (clip.x / clip.w * 0.5f + 0.5f) * size.x, pos.y + (0.5f - clip.y / clip.w * 0.5f) * size.y);
      }
      dl->PushClipRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), true);
      for (int a = 0; a < 8; a++)
        for (int bit = 1; bit < 8; bit <<= 1)
          if (!(a & bit)) dl->AddLine(corner[a], corner[a | bit], color, 1.5f);
      dl->PopClipRect();
    };

    // Assets dropped from the drawer: place meshes and prefabs where they land,
    // put textures and scripts on the entity under the cursor.
    if (ImGui::BeginDragDropTarget()) {
      const ImGuiPayload* payload =
          ImGui::AcceptDragDropPayload("ACE_ASSET", ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect);
      if (payload) {
        std::string file = (const char*)payload->Data;
        Entity hit;
        vec3 point, normal;
        if (!raycast(mouseUv, hit, point, normal)) { // nothing under the cursor: the ground plane
          vec3 o, e;
          viewRay(mouseUv, o, e);
          f32 t = fabsf(e.y - o.y) > 1e-6f ? o.y / (o.y - e.y) : -1.0f;
          point = t > 0.0f && t < 1.0f ? o + (e - o) * t : camera_.target;
        }
        AssetKind kind = assetKind(file);
        if ((kind == AssetKind::Image || kind == AssetKind::Script) && hit != kNoEntity) wireBox(worldBounds(hit), col::kAccent);
        else if (kind == AssetKind::Mesh || kind == AssetKind::Prefab || kind == AssetKind::Image) {
          ImVec2 c;
          if (toScreen(point, c)) {
            dl->AddCircle(c, 7.0f, col::kAccent, 0, 2.0f);
            dl->AddLine(ImVec2(c.x - 11, c.y), ImVec2(c.x + 11, c.y), col::kAccent);
            dl->AddLine(ImVec2(c.x, c.y - 11), ImVec2(c.x, c.y + 11), col::kAccent);
          }
        }
        if (payload->IsDelivery()) dropAsset(file, hit, point);
      }
      ImGui::EndDragDropTarget();
    }

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

      if (buildMode_) {
        vec3 center;
        vec3 half = glm::max(brushSize_, vec3(0.0625f)) * 0.5f;
        if (hovered && !io.KeyShift && blockPlacement(mouseUv, center)) wireBox(Aabb{center - half, center + half}, IM_COL32(255, 230, 90, 255));
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) buildModeClick(mouseUv, io.KeyShift, pos, size);
      }
      bool gizmoActive = false;
      if (selected_ != kNoEntity && !buildMode_) {
        for (Entity e : extraSelection_) { // outline the rest of a multi-selection
          Aabb b = worldBounds(e);
          if (b.min.x <= b.max.x) wireBox(b, IM_COL32(255, 200, 80, 200));
        }
        // Collider outline of the selection, so blocking volumes can be lined up by eye.
        Aabb box = scene_.colliderBounds(selected_);
        if (box.min.x <= box.max.x) wireBox(box, IM_COL32(90, 255, 140, 220));
        else if (gizmoOp_ < 0 && (box = worldBounds(selected_)).min.x <= box.max.x) wireBox(box, col::kAccent); // select tool: no gizmo
        if (gizmoOp_ >= 0) {
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
        }
        if (gizmoWasUsing_ && !gizmoActive) commit(); // one undo step per drag
        gizmoWasUsing_ = gizmoActive;
      }
      // Icons for lights and cameras, which have nothing to click on otherwise.
      Entity iconHit = kNoEntity;
      {
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
          if (icon && onScreen && !isLocked(e) && (m.x - c.x) * (m.x - c.x) + (m.y - c.y) * (m.y - c.y) <= radius * radius) iconHit = e;
        }
        dl->PopClipRect();
      }
      if (!buildMode_ && hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoActive &&
          !(selected_ != kNoEntity && gizmoOp_ >= 0 && ImGuizmo::IsOver())) {
        Entity hit = iconHit != kNoEntity ? iconHit : pick(mouseUv); // icons sit on top of the scene
        if (io.KeyShift && hit != kNoEntity) toggleSelected(hit);    // Shift+click adds to / removes from the selection
        else selected_ = hit;
      }
    }
    // Play mode: a colored frame (and a label in the top bar), so it is never mistaken for editing.
    if (playing_) dl->AddRect(pos, ImVec2(pos.x + size.x, pos.y + size.y), paused_ ? col::kWarn : col::kPlay, 0.0f, 0, 3.0f);
  }
  ImGui::End();
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
      drawerTab_ = 0; // the asset drawer, showing assets/, for the drag at step 59
      assetFolder_ = "assets";
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
      moveTo(rectCenter("menu:Create").x, rectCenter("menu:Create").y);
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
      // --- command palette: Cmd/Ctrl+K, type, Enter runs the best match
      key(ImGuiKey_K, true);
      break;
    }
    case 87:
      for (char c : std::string("create cube")) io.AddInputCharacter((unsigned)c);
      break;
    case 88: key(ImGuiKey_Enter); break;
    case 89: {
      Entity cube = scene_.find("Cube");
      expect(cube != kNoEntity && selected_ == cube, "the command palette runs 'Create Cube'");
      expect(cube != kNoEntity && fabsf(scene_.reg.get<Transform>(cube).position.y - 0.5f) < 1e-4f, "new meshes rest on the ground");
      // --- drag a texture from the asset drawer onto the cube in the viewport
      moveTo(rectCenter("asset:assets/dirt.png").x, rectCenter("asset:assets/dirt.png").y);
      break;
    }
    case 90: io.AddMouseButtonEvent(ImGuiMouseButton_Left, true); break;
    case 91: moveTo(mouse.x, mouse.y - 20); break;
    case 92: {
      ImVec2 p = toScreen(vec3(scene_.reg.get<Transform>(scene_.find("Cube")).world[3]));
      moveTo(p.x, p.y);
      break;
    }
    case 93: io.AddMouseButtonEvent(ImGuiMouseButton_Left, false); break;
    case 94: {
      Entity cube = scene_.find("Cube");
      expect(cube != kNoEntity && scene_.reg.get<MeshRenderer>(cube).texture == "assets/dirt.png", "dropping a texture on an entity in the viewport applies it");
      // --- a locked entity is not picked by viewport clicks
      locked_ = {cube};
      selected_ = kNoEntity;
      break; // the mouse is still over the cube
    }
    case 95:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 96:
      expect(selected_ == kNoEntity, "clicking a locked entity in the viewport selects nothing");
      locked_.clear();
      // --- identical siblings fold into one Scene row; clicking it selects them all
      for (int i = 0; i < 3; i++) createEntity("Tree", "builtin:cube");
      commit();
      selectAll({});
      break;
    case 97: moveTo(rectCenter("group:Tree").x, rectCenter("group:Tree").y); break;
    case 98:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 99: {
      size_t trees = 0;
      for (auto [e, n] : scene_.reg.view<Name>().each()) trees += n.value == "Tree" && isSelected(e);
      expect(trees == 3, "clicking a folded group selects every entity in it");
      // --- Scene search: type, then click the match
      moveTo(rectCenter("hierarchy:search").x, rectCenter("hierarchy:search").y);
      break;
    }
    case 100:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 101:
      for (char c : std::string("sun")) io.AddInputCharacter((unsigned)c);
      break;
    case 102: moveTo(rectCenter("found:Sun").x, rectCenter("found:Sun").y); break;
    case 103:
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
      io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
      break;
    case 104:
      expect(selected_ == scene_.find("Sun"), "clicking a search result selects it");
      hierarchyFilter_.clear();
      moveTo(center().x, center().y);
      key(ImGuiKey_Backslash, true);
      break;
    case 105:
      expect(panelsHidden_, "Cmd/Ctrl+\\ hides the panels");
      key(ImGuiKey_Backslash, true);
      break;
    case 106:
      expect(!panelsHidden_, "and shows them again");
      if (autotestResult_ == 0) {
        ACE_INFO("autotest passed");
        autotestResult_ = 1;
      }
      break;
    default: break;
  }
}

} // namespace ace
