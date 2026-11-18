#include "script/script.h"
#include <filesystem>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace ace {

namespace {

ScriptSystem& self(lua_State* L) { return *(ScriptSystem*)lua_touserdata(L, lua_upvalueindex(1)); }

// Script-facing entity ids are integers; a stale or bogus id raises a Lua error
// instead of touching freed memory.
Entity checkEntity(lua_State* L, int arg) {
  Entity e = (Entity)(u32)luaL_checkinteger(L, arg);
  if (!self(L).scene.reg.valid(e)) luaL_error(L, "invalid entity");
  return e;
}
vec3 checkVec3(lua_State* L, int first) {
  return {(f32)luaL_checknumber(L, first), (f32)luaL_checknumber(L, first + 1), (f32)luaL_checknumber(L, first + 2)};
}
int pushVec3(lua_State* L, vec3 v) {
  lua_pushnumber(L, v.x);
  lua_pushnumber(L, v.y);
  lua_pushnumber(L, v.z);
  return 3;
}
Transform& transformOf(lua_State* L) { return self(L).scene.reg.get<Transform>(checkEntity(L, 1)); }

int l_log(lua_State* L) {
  ACE_INFO("[lua] %s", luaL_tolstring(L, 1, nullptr));
  return 0;
}
int l_key(lua_State* L) {
  lua_pushboolean(L, self(L).input.keyDown(luaL_checkstring(L, 1)));
  return 1;
}
int l_key_pressed(lua_State* L) {
  lua_pushboolean(L, self(L).input.keyPressed(luaL_checkstring(L, 1)));
  return 1;
}
int l_time(lua_State* L) {
  lua_pushnumber(L, self(L).time);
  return 1;
}
int l_find(lua_State* L) {
  Entity e = self(L).scene.find(luaL_checkstring(L, 1));
  if (e == kNoEntity) lua_pushnil(L);
  else lua_pushinteger(L, (lua_Integer)(u32)e);
  return 1;
}
int l_spawn(lua_State* L) {
  lua_pushinteger(L, (lua_Integer)(u32)self(L).scene.create(luaL_checkstring(L, 1)));
  return 1;
}
int l_destroy(lua_State* L) {
  self(L).scene.destroy(checkEntity(L, 1));
  return 0;
}
int l_position(lua_State* L) { return pushVec3(L, transformOf(L).position); }
int l_set_position(lua_State* L) {
  transformOf(L).position = checkVec3(L, 2);
  return 0;
}
int l_rotation(lua_State* L) { return pushVec3(L, glm::degrees(glm::eulerAngles(transformOf(L).rotation))); }
int l_set_rotation(lua_State* L) {
  transformOf(L).rotation = quat(glm::radians(checkVec3(L, 2)));
  return 0;
}
int l_scale(lua_State* L) { return pushVec3(L, transformOf(L).scale); }
int l_set_scale(lua_State* L) {
  transformOf(L).scale = checkVec3(L, 2);
  return 0;
}
// ace.set_mesh(e, mesh [, r, g, b]) adds or updates a MeshRenderer.
int l_set_mesh(lua_State* L) {
  MeshRenderer& m = self(L).scene.reg.get_or_emplace<MeshRenderer>(checkEntity(L, 1));
  m.mesh = luaL_checkstring(L, 2);
  if (lua_gettop(L) >= 5) m.color = vec4(checkVec3(L, 3), 1.0f);
  return 0;
}
int l_set_color(lua_State* L) {
  if (auto* m = self(L).scene.reg.try_get<MeshRenderer>(checkEntity(L, 1))) m->color = vec4(checkVec3(L, 2), 1.0f);
  return 0;
}

// ace.pointer() -> down, x, y. Mouse button or finger; x and y are 0..1 across the window.
int l_pointer(lua_State* L) {
  const Input& in = self(L).input;
  lua_pushboolean(L, in.mouseDown[SDL_BUTTON_LEFT]);
  lua_pushnumber(L, in.mouse.x / in.windowSize.x);
  lua_pushnumber(L, in.mouse.y / in.windowSize.y);
  return 3;
}

// ace.play_sound(path [, volume [, loop]]) -> id (0 if it could not play)
int l_play_sound(lua_State* L) {
  f32 volume = (f32)luaL_optnumber(L, 2, 1.0);
  bool loop = lua_toboolean(L, 3);
  lua_pushinteger(L, self(L).audio.play(luaL_checkstring(L, 1), volume, loop));
  return 1;
}
int l_stop_sound(lua_State* L) {
  self(L).audio.stop((u32)luaL_checkinteger(L, 1));
  return 0;
}

// ace.text(text, x, y [, r, g, b [, scale]]): draw text this frame, in game pixels from the top-left.
int l_text(lua_State* L) {
  TextDraw t;
  t.text = luaL_checkstring(L, 1);
  t.position = vec2((f32)luaL_checknumber(L, 2), (f32)luaL_checknumber(L, 3));
  if (lua_gettop(L) >= 6) t.color = vec4((f32)luaL_checknumber(L, 4), (f32)luaL_checknumber(L, 5), (f32)luaL_checknumber(L, 6), 1.0f);
  t.scale = (int)luaL_optinteger(L, 7, 1);
  auto& texts = self(L).texts;
  if (texts.size() < 1024) texts.push_back(std::move(t)); // bound a runaway loop
  return 0;
}
// ace.screen() -> width, height in game pixels
int l_screen(lua_State* L) {
  lua_pushnumber(L, self(L).gameSize.x);
  lua_pushnumber(L, self(L).gameSize.y);
  return 2;
}
// ace.text_width(text) -> width in game pixels at scale 1
int l_text_width(lua_State* L) {
  lua_pushinteger(L, Renderer::textWidth(luaL_checkstring(L, 1)));
  return 1;
}

// --- save data: Lua tables <-> JSON in the per-user preferences folder ---

constexpr int kMaxSaveDepth = 16;

nlohmann::json luaToJson(lua_State* L, int index, int depth) {
  index = lua_absindex(L, index);
  switch (lua_type(L, index)) {
    case LUA_TBOOLEAN: return (bool)lua_toboolean(L, index);
    case LUA_TNUMBER: return lua_isinteger(L, index) ? nlohmann::json(lua_tointeger(L, index)) : nlohmann::json(lua_tonumber(L, index));
    case LUA_TSTRING: return std::string(lua_tostring(L, index));
    case LUA_TTABLE: {
      if (depth >= kMaxSaveDepth) luaL_error(L, "save data nested too deeply (or contains a cycle)");
      // A table with keys 1..n is an array; anything else becomes an object with string keys.
      lua_Integer n = (lua_Integer)lua_rawlen(L, index);
      nlohmann::json out = n > 0 ? nlohmann::json::array() : nlohmann::json::object();
      if (n > 0) {
        for (lua_Integer i = 1; i <= n; i++) {
          lua_rawgeti(L, index, i);
          out.push_back(luaToJson(L, -1, depth + 1));
          lua_pop(L, 1);
        }
        return out;
      }
      lua_pushnil(L);
      while (lua_next(L, index)) {
        if (lua_type(L, -2) == LUA_TSTRING) out[lua_tostring(L, -2)] = luaToJson(L, -1, depth + 1);
        else if (lua_type(L, -2) == LUA_TNUMBER) out[std::to_string((long long)lua_tointeger(L, -2))] = luaToJson(L, -1, depth + 1);
        lua_pop(L, 1);
      }
      return out;
    }
    default: return nullptr; // functions, userdata: not saved
  }
}

void pushJson(lua_State* L, const nlohmann::json& j, int depth) {
  luaL_checkstack(L, 4, "save data");
  if (depth > kMaxSaveDepth) {
    lua_pushnil(L);
  } else if (j.is_boolean()) {
    lua_pushboolean(L, j.get<bool>());
  } else if (j.is_number_integer()) {
    lua_pushinteger(L, j.get<lua_Integer>());
  } else if (j.is_number()) {
    lua_pushnumber(L, j.get<double>());
  } else if (j.is_string()) {
    lua_pushstring(L, j.get<std::string>().c_str());
  } else if (j.is_array()) {
    lua_createtable(L, (int)j.size(), 0);
    for (size_t i = 0; i < j.size(); i++) {
      pushJson(L, j[i], depth + 1);
      lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
  } else if (j.is_object()) {
    lua_createtable(L, 0, (int)j.size());
    for (auto it = j.begin(); it != j.end(); ++it) {
      pushJson(L, it.value(), depth + 1);
      lua_setfield(L, -2, it.key().c_str());
    }
  } else {
    lua_pushnil(L);
  }
}

std::string saveFilePath(ScriptSystem& s) {
  std::string project = std::filesystem::path(s.assets.projectDir()).parent_path().filename().string();
  if (project.empty()) project = "game";
  char* dir = SDL_GetPrefPath("AceEngine", project.c_str()); // per user, per game; created if missing
  if (!dir) return "";
  std::string path = std::string(dir) + "save.json";
  SDL_free(dir);
  return path;
}

// ace.save_data(table) -> true on success. Numbers, strings, booleans and nested tables.
int l_save_data(lua_State* L) {
  luaL_checktype(L, 1, LUA_TTABLE);
  std::string path = saveFilePath(self(L));
  nlohmann::json j = luaToJson(L, 1, 0);
  // Write to a temporary file first so a crash mid-write cannot destroy the old save.
  bool ok = !path.empty() && writeTextFile(path + ".tmp", j.dump(2));
  std::error_code ec;
  if (ok) std::filesystem::rename(path + ".tmp", path, ec);
  ok = ok && !ec;
  if (!ok) ACE_ERROR("could not write save data to %s", path.c_str());
  lua_pushboolean(L, ok);
  return 1;
}

// ace.load_data() -> table, or nil when there is no save yet (or it is unreadable)
int l_load_data(lua_State* L) {
  std::string text;
  if (!readTextFile(saveFilePath(self(L)), text)) {
    lua_pushnil(L);
    return 1;
  }
  nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
  if (j.is_discarded()) {
    ACE_WARN("save data is corrupt; ignoring it");
    lua_pushnil(L);
    return 1;
  }
  pushJson(L, j, 0);
  return 1;
}

// ace.spawn_prefab(path [, x, y, z]) -> root entity id (nil if the prefab could not be loaded)
int l_spawn_prefab(lua_State* L) {
  ScriptSystem& s = self(L);
  Entity root = s.scene.instantiatePrefab(s.assets.resolve(luaL_checkstring(L, 1)));
  if (root == kNoEntity) {
    lua_pushnil(L);
    return 1;
  }
  if (lua_gettop(L) >= 4) s.scene.reg.get<Transform>(root).position = checkVec3(L, 2);
  lua_pushinteger(L, (lua_Integer)(u32)root);
  return 1;
}

// ace.load_scene(path): switch level after this frame's scripts have run.
int l_load_scene(lua_State* L) {
  self(L).pendingScene = luaL_checkstring(L, 1);
  return 0;
}
// ace.overlaps(e) -> array of entity ids whose colliders overlap e's
int l_overlaps(lua_State* L) {
  std::vector<Entity> hits = self(L).scene.overlapping(checkEntity(L, 1));
  lua_createtable(L, (int)hits.size(), 0);
  for (size_t i = 0; i < hits.size(); i++) {
    lua_pushinteger(L, (lua_Integer)(u32)hits[i]);
    lua_rawseti(L, -2, (lua_Integer)i + 1);
  }
  return 1;
}
// ace.raycast(ox, oy, oz, dx, dy, dz [, max_distance [, ignore_entity]]) -> entity, distance, x, y, z  (nil if nothing)
int l_raycast(lua_State* L) {
  Entity ignore = lua_isnoneornil(L, 8) ? kNoEntity : checkEntity(L, 8);
  Scene::RayHit hit = self(L).scene.raycast(checkVec3(L, 1), checkVec3(L, 4), (f32)luaL_optnumber(L, 7, 1000.0), ignore);
  if (hit.entity == kNoEntity) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, (lua_Integer)(u32)hit.entity);
  lua_pushnumber(L, hit.distance);
  pushVec3(L, hit.point);
  return 5;
}
// ace.gamepad_axis(name) -> -1..1 ("leftx", "lefty", "rightx", "righty", "left_trigger", "right_trigger")
int l_gamepad_axis(lua_State* L) {
  SDL_GamepadAxis a = SDL_GetGamepadAxisFromString(luaL_checkstring(L, 1));
  lua_pushnumber(L, a == SDL_GAMEPAD_AXIS_INVALID ? 0.0 : self(L).input.gamepadAxis[a]);
  return 1;
}
// ace.gamepad_button(name) / ace.gamepad_pressed(name) ("south" = A on Xbox, "east", "start", "dpup", ...)
int gamepadButton(lua_State* L, bool pressed) {
  SDL_GamepadButton b = SDL_GetGamepadButtonFromString(luaL_checkstring(L, 1));
  const Input& in = self(L).input;
  lua_pushboolean(L, b != SDL_GAMEPAD_BUTTON_INVALID && (pressed ? in.gamepadPressed[b] : in.gamepadDown[b]));
  return 1;
}
int l_gamepad_button(lua_State* L) { return gamepadButton(L, false); }
int l_gamepad_pressed(lua_State* L) { return gamepadButton(L, true); }

// ace.set_look(e, yaw, pitch): first-person orientation in degrees: turn
// around the vertical axis, then tilt up/down (never rolls the view).
int l_set_look(lua_State* L) {
  f32 yaw = glm::radians((f32)luaL_checknumber(L, 2)), pitch = glm::radians((f32)luaL_checknumber(L, 3));
  transformOf(L).rotation = glm::angleAxis(yaw, vec3(0, 1, 0)) * glm::angleAxis(pitch, vec3(1, 0, 0));
  return 0;
}
// ace.lock_mouse(on): hide the cursor and capture the mouse for looking around
// (games only: ignored inside the editor). ace.mouse_locked() -> on
int l_lock_mouse(lua_State* L) {
  self(L).input.wantMouseLock = lua_toboolean(L, 1);
  return 0;
}
int l_mouse_locked(lua_State* L) {
  lua_pushboolean(L, self(L).input.mouseLocked);
  return 1;
}

// ace.ground(x, z [, from_y [, ignore]]) -> height of the highest mesh surface
// below (x, from_y, z), or nil. For walking on terrain.
int l_ground(lua_State* L) {
  ScriptSystem& s = self(L);
  f32 x = (f32)luaL_checknumber(L, 1), z = (f32)luaL_checknumber(L, 2), from = (f32)luaL_optnumber(L, 3, 10000.0);
  Entity ignore = lua_isnoneornil(L, 4) ? kNoEntity : checkEntity(L, 4);
  vec3 origin(x, from, z);
  f32 best = -1.0f;
  for (auto [e, t, m] : s.scene.reg.view<Transform, MeshRenderer>().each()) {
    if (e == ignore) continue;
    f32 dist;
    if (raycastMesh(*s.assets.mesh(m.mesh), t.world, origin, vec3(0, -1, 0), best < 0.0f ? 1e9f : best, dist)) best = dist;
  }
  if (best < 0.0f) lua_pushnil(L);
  else lua_pushnumber(L, from - best);
  return 1;
}
// ace.mouse_delta() -> dx, dy: mouse movement this frame, in window points
int l_mouse_delta(lua_State* L) {
  lua_pushnumber(L, self(L).input.mouseDelta.x);
  lua_pushnumber(L, self(L).input.mouseDelta.y);
  return 2;
}
// ace.mouse_button(n) -> held: 1 left, 2 middle, 3 right
int l_mouse_button(lua_State* L) {
  int b = (int)luaL_checkinteger(L, 1);
  lua_pushboolean(L, b >= 1 && b <= 3 && self(L).input.mouseDown[b]);
  return 1;
}

// ace.move(e, dx, dy, dz) -> moved x, y, z. Slides along solid colliders.
int l_move(lua_State* L) { return pushVec3(L, self(L).scene.moveAndSlide(checkEntity(L, 1), checkVec3(L, 2))); }

// ace.set_sprite(e, frame [, fps [, flip_x]]): fps > 0 loops the whole sheet from `frame`.
int l_set_sprite(lua_State* L) {
  if (auto* s = self(L).scene.reg.try_get<SpriteRenderer>(checkEntity(L, 1))) {
    s->frame = (int)luaL_checkinteger(L, 2);
    if (lua_gettop(L) >= 3) s->fps = (f32)luaL_checknumber(L, 3);
    if (lua_gettop(L) >= 4) s->flipX = lua_toboolean(L, 4);
  }
  return 0;
}

int traceback(lua_State* L) {
  luaL_traceback(L, L, lua_tostring(L, 1), 1);
  return 1;
}

// Calls instance:method(args...). Returns false (and logs) on a Lua error.
bool callMethod(lua_State* L, int ref, const char* method, int nargs, f32 arg) {
  lua_pushcfunction(L, traceback);
  lua_rawgeti(L, LUA_REGISTRYINDEX, ref);
  lua_getfield(L, -1, method);
  if (!lua_isfunction(L, -1)) {
    lua_pop(L, 3);
    return true; // optional callback
  }
  lua_insert(L, -2); // function, instance
  if (nargs) lua_pushnumber(L, arg);
  bool ok = lua_pcall(L, 1 + nargs, 0, -3 - nargs) == LUA_OK;
  if (!ok) {
    ACE_ERROR("script error: %s", lua_tostring(L, -1));
    lua_pop(L, 1);
  }
  lua_pop(L, 1); // traceback
  return ok;
}

} // namespace

ScriptSystem::ScriptSystem(Scene& s, Assets& a, Input& i) : scene(s), assets(a), input(i), audio(a.projectDir()) {
  L_ = luaL_newstate();
  // Scripts get the safe standard libraries only: no io/os/package, so a game
  // script cannot read or write arbitrary files on the player's machine.
  static const luaL_Reg libs[] = {{LUA_GNAME, luaopen_base}, {LUA_TABLIBNAME, luaopen_table}, {LUA_STRLIBNAME, luaopen_string},
                                  {LUA_MATHLIBNAME, luaopen_math}, {LUA_UTF8LIBNAME, luaopen_utf8}};
  for (const luaL_Reg& lib : libs) {
    luaL_requiref(L_, lib.name, lib.func, 1);
    lua_pop(L_, 1);
  }
  for (const char* unsafe : {"dofile", "loadfile", "load"}) {
    lua_pushnil(L_);
    lua_setglobal(L_, unsafe);
  }
  static const luaL_Reg api[] = {
      {"log", l_log},           {"key", l_key},           {"key_pressed", l_key_pressed}, {"time", l_time},
      {"find", l_find},         {"spawn", l_spawn},       {"destroy", l_destroy},         {"position", l_position},
      {"set_position", l_set_position}, {"rotation", l_rotation}, {"set_rotation", l_set_rotation}, {"scale", l_scale},
      {"set_scale", l_set_scale}, {"set_mesh", l_set_mesh}, {"set_color", l_set_color},     {"set_sprite", l_set_sprite},   {"move", l_move},               {"pointer", l_pointer},         {"ground", l_ground},
      {"set_look", l_set_look},       {"lock_mouse", l_lock_mouse}, {"mouse_locked", l_mouse_locked},
      {"mouse_delta", l_mouse_delta}, {"mouse_button", l_mouse_button},
      {"play_sound", l_play_sound}, {"stop_sound", l_stop_sound},
      {"save_data", l_save_data},  {"load_data", l_load_data},
      {"load_scene", l_load_scene},  {"spawn_prefab", l_spawn_prefab},  {"overlaps", l_overlaps},   {"raycast", l_raycast},
      {"gamepad_axis", l_gamepad_axis}, {"gamepad_button", l_gamepad_button}, {"gamepad_pressed", l_gamepad_pressed},
      {"text", l_text},             {"screen", l_screen},       {"text_width", l_text_width},
      {nullptr, nullptr}};
  lua_newtable(L_);
  lua_pushlightuserdata(L_, this);
  luaL_setfuncs(L_, api, 1);
  lua_setglobal(L_, "ace");
}

ScriptSystem::~ScriptSystem() {
  input.wantMouseLock = false; // stopping the game gives the cursor back
  lua_close(L_);
}

void ScriptSystem::restart() {
  for (auto& [e, inst] : instances_)
    if (inst.ref >= 0) luaL_unref(L_, LUA_REGISTRYINDEX, inst.ref);
  instances_.clear();
  texts.clear();
}

std::vector<std::pair<std::string, std::string>> parseScriptParams(std::string_view text) {
  std::vector<std::pair<std::string, std::string>> out;
  auto trim = [](std::string_view s) {
    while (!s.empty() && isspace((unsigned char)s.front())) s.remove_prefix(1);
    while (!s.empty() && isspace((unsigned char)s.back())) s.remove_suffix(1);
    return std::string(s);
  };
  while (!text.empty()) {
    size_t end = text.find_first_of(";\n");
    std::string_view item = text.substr(0, end);
    text = end == std::string_view::npos ? std::string_view() : text.substr(end + 1);
    size_t eq = item.find('=');
    if (eq == std::string_view::npos) continue;
    std::string key = trim(item.substr(0, eq));
    bool identifier = !key.empty() && !isdigit((unsigned char)key[0]) && key != "entity";
    for (char c : key) identifier &= isalnum((unsigned char)c) || c == '_';
    if (identifier) out.emplace_back(key, trim(item.substr(eq + 1)));
    else ACE_WARN("script params: ignoring '%.*s' (need name=value; 'entity' is reserved)", (int)item.size(), item.data());
  }
  return out;
}

bool ScriptSystem::instantiate(Entity e, const std::string& path) {
  std::string source;
  if (!readTextFile(assets.resolve(path), source)) {
    ACE_ERROR("script '%s' not found", path.c_str());
    return false;
  }
  lua_pushcfunction(L_, traceback);
  if (luaL_loadbufferx(L_, source.data(), source.size(), ("@" + path).c_str(), "t") != LUA_OK || lua_pcall(L_, 0, 1, -2) != LUA_OK) {
    ACE_ERROR("script error: %s", lua_tostring(L_, -1));
    lua_pop(L_, 2);
    return false;
  }
  if (!lua_istable(L_, -1)) {
    ACE_ERROR("script '%s' must return a table", path.c_str());
    lua_pop(L_, 2);
    return false;
  }
  // instance = setmetatable({entity = id}, {__index = class})
  lua_newtable(L_);
  lua_pushinteger(L_, (lua_Integer)(u32)e);
  lua_setfield(L_, -2, "entity");
  // Per-entity parameters from the Script component override the class defaults.
  for (auto& [key, value] : parseScriptParams(scene.reg.get<Script>(e).params)) {
    char* end = nullptr;
    double number = strtod(value.c_str(), &end);
    if (value == "true" || value == "false") lua_pushboolean(L_, value == "true");
    else if (!value.empty() && end && *end == '\0') lua_pushnumber(L_, number);
    else lua_pushlstring(L_, value.data(), value.size());
    lua_setfield(L_, -2, key.c_str());
  }
  lua_newtable(L_);
  lua_pushvalue(L_, -3);
  lua_setfield(L_, -2, "__index");
  lua_setmetatable(L_, -2);
  int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
  lua_pop(L_, 2); // class, traceback
  instances_[(u32)e].ref = ref;
  return callMethod(L_, ref, "start", 0, 0.0f);
}

// Modification time of a script file, or 0 if it cannot be read as a plain file
// (for example inside an Android package, where hot reload does not apply).
static i64 fileTime(const std::string& path) {
  std::error_code ec;
  auto t = std::filesystem::last_write_time(path, ec);
  return ec ? 0 : (i64)t.time_since_epoch().count();
}

void ScriptSystem::update(f32 dt) {
  time += dt;
  texts.clear();
  audio.update();
  // Hot reload: twice a second, restart any script whose file changed on disk.
  // The instance starts fresh (its `start` runs again); entity state is kept.
  if (time - lastReloadCheck_ > 0.5) {
    lastReloadCheck_ = time;
    for (auto it = instances_.begin(); it != instances_.end();) {
      i64 now = fileTime(assets.resolve(it->second.path));
      if (now != 0 && now != it->second.modified) {
        ACE_INFO("reloading %s", it->second.path.c_str());
        if (it->second.ref >= 0) luaL_unref(L_, LUA_REGISTRYINDEX, it->second.ref);
        it = instances_.erase(it);
      } else {
        ++it;
      }
    }
  }
  // Copy first: scripts may spawn or destroy entities while we iterate.
  std::vector<std::pair<Entity, std::string>> scripts;
  for (auto [e, s] : scene.reg.view<Script>().each())
    if (!s.path.empty()) scripts.emplace_back(e, s.path);
  for (auto& [e, path] : scripts) {
    if (!scene.reg.valid(e)) continue;
    auto it = instances_.find((u32)e);
    if (it == instances_.end() || it->second.path != path) {
      Instance& inst = instances_[(u32)e];
      inst = Instance{-1, path, fileTime(assets.resolve(path))};
      if (!instantiate(e, path)) instances_[(u32)e].ref = -1;
      it = instances_.find((u32)e);
    }
    if (it->second.ref < 0) continue; // failed earlier; stay quiet instead of spamming every frame
    if (!callMethod(L_, it->second.ref, "update", 1, dt)) it->second.ref = -1;
  }
}

} // namespace ace
