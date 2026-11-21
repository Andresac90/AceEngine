// The level editor: scene hierarchy, inspector, asset browser, viewport with
// gizmos, play mode and undo/redo. All panels work from the component
// reflection table (scene/scene.h), so new components appear automatically.
#pragma once
#include "imgui_rhi.h"
#include "platform/app.h"
#include "render/renderer.h"
#include "runtime/project.h"
#include "script/script.h"
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

namespace ace {

struct EditorCamera {
  vec3 target{0.0f, 0.5f, 0.0f};
  f32 yaw = 45.0f, pitch = -35.0f; // degrees
  f32 distance = 20.0f;
  bool orthographic = false; // perspective by default, so world-pixel scenes preview as in the game
  f32 orthoSize = 6.0f;
  CameraView view() const;
};

class Editor {
public:
  // automated: screenshot/test/export runs. They use the default panel layout and
  // never save it, and never ask questions.
  Editor(App& app, Project project, bool automated = false);
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
  // Set by File > Open Project: the main loop then reopens the editor on it.
  std::string requestedProject;
  // Before quitting or switching projects: offers to save unsaved changes.
  // Returns false if the user chose Cancel.
  bool confirmLeave();
  // Opens a file in a code editor (VS Code if installed, else the system default).
  void openInCodeEditor(const std::string& file);
  std::string focusWindow; // panel brought to the front on startup (--focus), e.g. for screenshots
  // Same as the File > Export menu ("macos", "ios-simulator", "ios-device"); for scripted use.
  bool exportTarget(const std::string& target);
  bool exportRunning() const { return exporting_; }
  int exportResult() const { return exportResult_; } // 0 running/none, 1 ok, -1 failed
  rhi::Format uiFormat = rhi::Format::BGRA8_SRGB;

private:
  void drawMenu();
  void drawToolbar();
  void drawHierarchy();
  void drawHierarchyNode(Entity e);
  void drawInspector();
  void drawSettings();
  void drawPalette();
  bool drawScriptButtons(Script& script); // Open / New script under a Script component
  std::string newScriptPath(const std::string& entityName);
  bool writeScriptTemplate(const std::string& relativePath, const std::string& entityName);
  void loadPalette();   // palette image -> paletteRows_
  void applyPalette();  // paletteRows_ -> GPU texture (live preview)
  bool savePalette();   // -> PNG file
  void createPalette(); // new palette file for a scene without one
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
  Entity placePrefab(const std::string& relativePath); // a copy at the camera's focus point, selected
  Entity pick(vec2 uv);
  // Ray through viewport point `uv` (0..1): nearest entity, world hit point and surface normal.
  bool raycast(vec2 uv, Entity& entity, vec3& point, vec3& normal);
  void buildModeClick(vec2 uv, bool erase, ImVec2 viewPos, ImVec2 viewSize);
  bool blockPlacement(vec2 uv, vec3& center); // where a click would put a block
  void refreshAssets();
  void startExport(const char* script, const char* extraArg);
  void flushExportLog(); // moves the export thread's output into the log (main thread only) // runs tools/<script> on a background thread

  App& app_;
  Project project_;
  Renderer renderer_;
  Assets assets_;
  Scene scene_;
  std::unique_ptr<ImGuiRhi> ui_;
  std::unique_ptr<ScriptSystem> scripts_;

  std::string scenePath_; // project-relative
  Entity selected_ = kNoEntity;          // primary selection: the inspector and gizmo follow it
  std::vector<Entity> extraSelection_;   // further entities selected with Shift/Cmd-click
  Entity lastPrimary_ = kNoEntity;       // when selected_ changes any other way, extras are dropped
  bool isSelected(Entity e) const;
  void toggleSelected(Entity e);         // additive click
  std::vector<Entity> selectionRoots();  // selected entities that are not inside another selected one
  void selectAll(const std::vector<Entity>& entities);
  void deleteSelection();
  void duplicateSelection();
  void copySelection();
  void paste();
  EditorCamera camera_;
  int gizmoOp_ = 0; // 0 move, 1 rotate, 2 scale
  bool snap_ = true;      // gizmo snapping: move by snapMove_, rotate by 15 degrees, scale by 0.25
  f32 snapMove_ = 0.25f;
  // Build mode: click a surface to add a textured, solid block on the grid.
  bool buildMode_ = false;
  std::string brushTexture_;
  vec3 brushSize_{1.0f};  // world units; 0.25 = 4 texels at 16 texels per unit
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
  bool automated_ = false;
  std::string iniPath_; // panel layout, in the user's settings folder
  std::string palettePath_;
  std::vector<std::vector<vec4>> paletteRows_;
  bool paletteDirty_ = false;

  std::thread exportThread_;
  std::atomic<bool> exporting_{false};
  std::atomic<int> exportResult_{0};
  std::mutex exportMutex_;
  std::vector<std::string> exportLines_; // produced by the export thread, logged on the main thread

  void autotestStep();
  // Autotest only: screen rectangle of the last ImGui item, under a readable key
  // ("node:Rock", "item:Cube"), so the test can click and drag real widgets.
  void testRect(const std::string& id);
  struct TestRect {
    ImVec2 min, max;
  };
  std::map<std::string, TestRect> testRects_;
  ImVec2 viewPos_{0, 0}, viewSize_{0, 0}; // viewport image rectangle, ImGui coordinates
  int autotestFrame_ = 0, autotestResult_ = 0;
  std::string savedClipboard_;
  size_t entityCountBefore_ = 0;
  vec3 groupBefore_{0.0f}, panTargetBefore_{0.0f};
  std::vector<u8> pathTraceFirst_;
};

} // namespace ace
