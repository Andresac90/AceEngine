// The level editor: scene hierarchy, inspector, asset browser, viewport with
// gizmos, play mode and undo/redo. All panels work from the component
// reflection table (scene/scene.h), so new components appear automatically.
#pragma once
#include "imgui_rhi.h"
#include "platform/app.h"
#include "render/renderer.h"
#include "runtime/project.h"
#include "script/script.h"

namespace ace {

struct EditorCamera {
  vec3 target{0.0f, 0.5f, 0.0f};
  f32 yaw = 45.0f, pitch = -35.0f; // degrees
  f32 distance = 20.0f;
  bool orthographic = true;
  f32 orthoSize = 6.0f;
  CameraView view() const;
};

class Editor {
public:
  Editor(App& app, Project project);
  ~Editor();
  // Builds the UI and renders one frame into `target` (the backbuffer, or an
  // offscreen texture when taking automated screenshots).
  void buildUi();
  void render(rhi::CommandList& cmd, rhi::TextureHandle target);
  void onEvent(const SDL_Event& e);
  // Automation hooks (used by --select / --play for scripted screenshots).
  void select(const std::string& name) { selected_ = scene_.find(name); }
  void play() { setPlaying(true); }
  // Scripted interaction test (--autotest): injects mouse/keyboard input and
  // checks the editor reacted. Returns 0 while running, 1 on pass, -1 on failure.
  int autotestResult() const { return autotestResult_; }
  bool autotest = false;
  rhi::Format uiFormat = rhi::Format::BGRA8_SRGB;

private:
  void drawMenu();
  void drawToolbar();
  void drawHierarchy();
  void drawHierarchyNode(Entity e);
  void drawInspector();
  void drawSettings();
  void drawAssets();
  void drawConsole();
  void drawViewport();
  bool editFields(void* object, const std::vector<Field>& fields); // true when an edit finished
  bool assetPicker(const Field& field, std::string& value);         // dropdown of matching project files
  void handleShortcuts();

  void commit(); // record an undo step after a finished edit
  void undo();
  void redo();
  void restore(const std::string& snapshot);
  void newScene();
  void openScene(const std::string& relativePath);
  void saveScene();
  void setPlaying(bool playing);
  Entity createEntity(const char* name, const char* mesh);
  Entity pick(vec2 uv);
  void refreshAssets();

  App& app_;
  Project project_;
  Renderer renderer_;
  Assets assets_;
  Scene scene_;
  std::unique_ptr<ImGuiRhi> ui_;
  std::unique_ptr<ScriptSystem> scripts_;

  std::string scenePath_; // project-relative
  Entity selected_ = kNoEntity;
  EditorCamera camera_;
  int gizmoOp_ = 0; // 0 move, 1 rotate, 2 scale
  bool snap_ = true;      // gizmo snapping: move by snapMove_, rotate by 15 degrees, scale by 0.25
  f32 snapMove_ = 0.25f;  // world units; 0.25 = 4 texels at 16 texels per unit
  bool gizmoWasUsing_ = false;
  bool playing_ = false;
  bool dirty_ = false;
  std::string playSnapshot_;
  std::vector<std::string> undoStack_;
  size_t undoIndex_ = 0;

  rhi::TextureHandle viewportTexture_;
  u32 viewportW_ = 0, viewportH_ = 0;     // texture size in pixels
  u32 wantedW_ = 0, wantedH_ = 0;         // size the panel asked for this frame
  std::vector<std::string> assetFiles_;   // project-relative
  f64 assetsRefreshed_ = -10.0;
  std::vector<std::pair<LogLevel, std::string>> console_;
  bool firstFrame_ = true;

  void autotestStep();
  ImVec2 viewPos_{0, 0}, viewSize_{0, 0}; // viewport image rectangle, ImGui coordinates
  int autotestFrame_ = 0, autotestResult_ = 0;
};

} // namespace ace
