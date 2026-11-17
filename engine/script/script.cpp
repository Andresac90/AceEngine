#include "script/script.h"

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

ScriptSystem::ScriptSystem(Scene& s, Assets& a, const Input& i) : scene(s), assets(a), input(i) {
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
      {"set_scale", l_set_scale}, {"set_mesh", l_set_mesh}, {"set_color", l_set_color},     {"set_sprite", l_set_sprite},
      {nullptr, nullptr}};
  lua_newtable(L_);
  lua_pushlightuserdata(L_, this);
  luaL_setfuncs(L_, api, 1);
  lua_setglobal(L_, "ace");
}

ScriptSystem::~ScriptSystem() { lua_close(L_); }

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
  lua_newtable(L_);
  lua_pushvalue(L_, -3);
  lua_setfield(L_, -2, "__index");
  lua_setmetatable(L_, -2);
  int ref = luaL_ref(L_, LUA_REGISTRYINDEX);
  lua_pop(L_, 2); // class, traceback
  instances_[(u32)e] = ref;
  return callMethod(L_, ref, "start", 0, 0.0f);
}

void ScriptSystem::update(f32 dt) {
  time += dt;
  // Copy first: scripts may spawn or destroy entities while we iterate.
  std::vector<std::pair<Entity, std::string>> scripts;
  for (auto [e, s] : scene.reg.view<Script>().each())
    if (!s.path.empty()) scripts.emplace_back(e, s.path);
  for (auto& [e, path] : scripts) {
    if (!scene.reg.valid(e)) continue;
    auto it = instances_.find((u32)e);
    if (it == instances_.end()) {
      if (!instantiate(e, path)) instances_[(u32)e] = -1;
      it = instances_.find((u32)e);
    }
    if (it->second < 0) continue; // failed earlier; stay quiet instead of spamming every frame
    if (!callMethod(L_, it->second, "update", 1, dt)) it->second = -1;
  }
}

} // namespace ace
