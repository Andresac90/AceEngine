#include "editor.h"

#include <ImGuizmo.h>
#include <algorithm>
#include <filesystem>
#include <imgui_impl_sdl3.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

namespace ace {

using json = nlohmann::json;

namespace {

constexpr size_t kMaxUndoSteps = 100;
constexpr size_t kMaxConsoleLines = 500;

// Requests raised while iterating the hierarchy, applied after the loop.
Entity g_deleteRequest = kNoEntity;
Entity g_duplicateRequest = kNoEntity;

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

Editor::Editor(App& app, Project project)
    : app_(app), project_(std::move(project)), renderer_(*app.device), assets_(*app.device, project_.dir) {
  setLogSink([this](LogLevel level, const std::string& line) {
    console_.emplace_back(level, line);
    if (console_.size() > kMaxConsoleLines) console_.erase(console_.begin());
  });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable | ImGuiConfigFlags_NavEnableKeyboard;
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

Editor::~Editor() {
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
      ImGui::DockBuilderDockWindow("Assets", bottom);
      ImGui::DockBuilderDockWindow("Console", bottom);
      ImGui::DockBuilderDockWindow("Viewport", center);
      ImGui::DockBuilderFinish(dock);
    }
  }

  if (!scene_.reg.valid(selected_)) selected_ = kNoEntity;
  handleShortcuts();
  drawMenu();
  drawHierarchy();
  drawInspector();
  drawSettings();
  drawAssets();
  drawConsole();
  drawViewport();

  if (g_deleteRequest != kNoEntity) {
    scene_.destroy(g_deleteRequest);
    if (!scene_.reg.valid(selected_)) selected_ = kNoEntity;
    g_deleteRequest = kNoEntity;
    commit();
  }
  if (g_duplicateRequest != kNoEntity) {
    selected_ = scene_.duplicate(g_duplicateRequest);
    g_duplicateRequest = kNoEntity;
    commit();
  }

  if (playing_) scripts_->update(app_.dt);
  if (!scene_.reg.valid(selected_)) selected_ = kNoEntity; // a script may have destroyed it
  scene_.updateTransforms();

  static std::string lastTitle;
  std::string title = project_.name + " - " + scenePath_ + (dirty_ ? " *" : "") + " - AceEngine";
  if (title != lastTitle) SDL_SetWindowTitle(app_.window, (lastTitle = title).c_str());
}

void Editor::render(rhi::CommandList& cmd, rhi::TextureHandle target) {
  ImGui::Render();
  renderer_.beginFrame();
  if (viewportTexture_) {
    CameraView view = camera_.view();
    if (playing_) Renderer::findCamera(scene_, view); // falls back to the editor camera
    renderer_.render(cmd, scene_, assets_, view, viewportTexture_, rhi::Format::RGBA8_SRGB, viewportW_, viewportH_);
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
  if (selected_ != kNoEntity) {
    if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) g_duplicateRequest = selected_;
    if (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Backspace)) g_deleteRequest = selected_;
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
    if (ImGui::BeginMenu("File")) {
      if (ImGui::MenuItem("New Scene")) newScene();
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
      ImGui::Separator();
      if (ImGui::MenuItem("Use As Start Scene", nullptr, project_.startScene == scenePath_)) {
        project_.startScene = scenePath_;
        project_.save();
      }
      if (ImGui::MenuItem("Reload Assets")) assets_.reload();
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
      if (ImGui::MenuItem("Duplicate", "Ctrl+D", false, selected_ != kNoEntity)) g_duplicateRequest = selected_;
      if (ImGui::MenuItem("Delete", "Del", false, selected_ != kNoEntity)) g_deleteRequest = selected_;
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Entity")) {
      if (ImGui::MenuItem("Empty")) createEntity("Entity", nullptr);
      if (ImGui::MenuItem("Cube")) createEntity("Cube", "builtin:cube");
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
    // Keep saves inside the project folder.
    bool valid = !saveAsPath.empty() && saveAsPath[0] != '/' && saveAsPath.find("..") == std::string::npos;
    ImGui::BeginDisabled(!valid);
    if (ImGui::Button("Save")) {
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
  if (e == selected_) flags |= ImGuiTreeNodeFlags_Selected;
  ImGui::PushID((int)(u32)e);
  bool open = ImGui::TreeNodeEx("##node", flags, "%s", scene_.reg.get<Name>(e).value.c_str());
  if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) selected_ = e;
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
    selected_ = e;
    if (ImGui::MenuItem("Add Child")) {
      Entity child = scene_.create("Entity", e);
      selected_ = child;
      commit();
    }
    if (ImGui::MenuItem("Duplicate")) g_duplicateRequest = e;
    if (ImGui::MenuItem("Delete")) g_deleteRequest = e;
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
        changed = ImGui::InputText(f.name, (std::string*)p);
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

void Editor::drawInspector() {
  ImGui::Begin("Inspector");
  if (selected_ == kNoEntity) {
    ImGui::TextDisabled("Select an entity in the Hierarchy or Viewport.");
    ImGui::End();
    return;
  }
  bool finished = false;
  ImGui::InputText("Name", &scene_.reg.get<Name>(selected_).value);
  finished |= ImGui::IsItemDeactivatedAfterEdit();
  for (const ComponentType& type : componentTypes()) {
    void* component = type.get(scene_.reg, selected_);
    if (!component) continue;
    bool keep = true;
    bool removable = strcmp(type.name, "Transform") != 0;
    if (ImGui::CollapsingHeader(type.name, removable ? &keep : nullptr, ImGuiTreeNodeFlags_DefaultOpen)) {
      ImGui::PushID(type.name);
      finished |= editFields(component, type.fields);
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
  ImGui::TextDisabled("Saved with the scene.");
  if (editFields(&scene_.settings, renderSettingsFields()) && !playing_) commit();
  ImGui::End();
}

void Editor::drawAssets() {
  ImGui::Begin("Assets");
  static std::string filter;
  ImGui::SetNextItemWidth(220);
  ImGui::InputTextWithHint("##filter", "filter", &filter);
  ImGui::SameLine();
  ImGui::TextDisabled("drag onto mesh / texture / script fields; double-click a scene to open it");
  ImGui::BeginChild("files");
  for (const std::string& f : assetFiles_) {
    if (!filter.empty() && f.find(filter) == std::string::npos) continue;
    ImGui::Selectable(f.c_str(), f == scenePath_);
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) && endsWith(f, ".scene.json")) openScene(f);
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

// Nearest entity whose mesh bounds are hit by the ray through viewport point `uv` (0..1).
Entity Editor::pick(vec2 uv) {
  CameraView cv = camera_.view();
  mat4 invViewProj = glm::inverse(cv.camera.projection((f32)viewportW_ / (f32)viewportH_) * glm::inverse(cv.world));
  vec2 ndc{uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f};
  vec4 nearP = invViewProj * vec4(ndc, 0.0f, 1.0f), farP = invViewProj * vec4(ndc, 1.0f, 1.0f);
  vec3 origin = vec3(nearP) / nearP.w, end = vec3(farP) / farP.w;
  Entity best = kNoEntity;
  f32 bestT = 2.0f; // t is in 0..1 along origin->end
  auto test = [&](Entity e, const mat4& world, Aabb box) {
    mat4 inv = glm::inverse(world);
    vec3 o = vec3(inv * vec4(origin, 1.0f)), d = vec3(inv * vec4(end, 1.0f)) - o;
    vec3 pad = glm::max(vec3(0.02f) - (box.max - box.min), vec3(0.0f)) * 0.5f; // give flat meshes some thickness
    f32 t0 = 0.0f, t1 = 1.0f;
    for (int a = 0; a < 3; a++) {
      f32 lo = (box.min[a] - pad[a] - o[a]) / d[a], hi = (box.max[a] + pad[a] - o[a]) / d[a];
      if (lo > hi) std::swap(lo, hi);
      t0 = std::max(t0, lo);
      t1 = std::min(t1, hi);
    }
    if (t0 <= t1 && t0 < bestT) {
      bestT = t0;
      best = e;
    }
  };
  for (auto [e, t, m] : scene_.reg.view<Transform, MeshRenderer>().each()) test(e, t.world, assets_.mesh(m.mesh)->bounds);
  for (auto [e, t, s] : scene_.reg.view<Transform, SpriteRenderer>().each())
    test(e, t.world, Aabb{vec3(-s.size * 0.5f, 0.0f), vec3(s.size * 0.5f, 0.0f)});
  return best;
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
  ImGui::Checkbox("Orthographic", &camera_.orthographic);
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

      bool gizmoActive = false;
      if (selected_ != kNoEntity) {
        CameraView cv = camera_.view();
        mat4 view = glm::inverse(cv.world);
        mat4 proj = cv.camera.projection(size.x / size.y);
        ImGuizmo::SetOrthographic(camera_.orthographic);
        ImGuizmo::SetDrawlist();
        ImGuizmo::SetRect(pos.x, pos.y, size.x, size.y);
        Transform& t = scene_.reg.get<Transform>(selected_);
        mat4 world = t.world;
        static const ImGuizmo::OPERATION ops[] = {ImGuizmo::TRANSLATE, ImGuizmo::ROTATE, ImGuizmo::SCALE};
        if (ImGuizmo::Manipulate(&view[0][0], &proj[0][0], ops[gizmoOp_], gizmoOp_ == 2 ? ImGuizmo::LOCAL : ImGuizmo::WORLD, &world[0][0])) {
          mat4 parentWorld = scene_.reg.valid(t.parent) ? scene_.reg.get<Transform>(t.parent).world : mat4(1.0f);
          vec3 skew;
          vec4 persp;
          glm::decompose(glm::inverse(parentWorld) * world, t.scale, t.rotation, t.position, skew, persp);
          dirty_ = true;
        }
        gizmoActive = ImGuizmo::IsUsing();
        if (gizmoWasUsing_ && !gizmoActive) commit(); // one undo step per drag
        gizmoWasUsing_ = gizmoActive;
      }
      if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !gizmoActive && !(selected_ != kNoEntity && ImGuizmo::IsOver()))
        selected_ = pick({(io.MousePos.x - pos.x) / size.x, (io.MousePos.y - pos.y) / size.y});
    }
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

// ---------------------------------------------------------------- autotest

// Drives the editor like a user would (--autotest). Each step runs a few frames
// after the previous one so ImGui has processed the input. Run it on a copy of
// the demo project: it saves the scene.
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
      if (autotestResult_ == 0) {
        ACE_INFO("autotest passed");
        autotestResult_ = 1;
      }
      break;
    default: break;
  }
}

} // namespace ace
