// Lua gameplay scripting. An entity with a Script component runs a Lua file that
// returns a table; `start(self)` runs once and `update(self, dt)` every frame.
// `self.entity` is the entity id to pass to the `ace.*` functions (docs/SCRIPTING.md).
#pragma once
#include "assets/assets.h"
#include "audio/audio.h"
#include "platform/app.h"
#include "scene/scene.h"
#include <unordered_map>

struct lua_State;

namespace ace {

class ScriptSystem {
public:
  ScriptSystem(Scene& scene, Assets& assets, const Input& input);
  ~ScriptSystem();
  void update(f32 dt); // instantiates scripts on new entities, then updates all

  Scene& scene;
  Assets& assets;
  const Input& input;
  f64 time = 0.0;
  Audio audio; // sounds started by scripts; they stop when this system is destroyed

private:
  bool instantiate(Entity e, const std::string& path);
  lua_State* L_ = nullptr;
  struct Instance {
    int ref = -1;      // Lua registry reference, or -1 if the script failed to load or errored
    std::string path;
    i64 modified = 0;  // file time when loaded, to detect edits
  };
  std::unordered_map<u32, Instance> instances_;
  f64 lastReloadCheck_ = 0.0;
};

} // namespace ace
