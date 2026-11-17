// Scene = an EnTT registry plus the per-scene render settings.
//
// Components are plain structs. Each one is described once in a ComponentType
// table (name + fields); serialization, the editor inspector and Lua all work
// from that table, so adding a component is: write the struct, register it.
#pragma once
#include "core/base.h"
#include "core/math.h"
#include <entt/entt.hpp>
#include <nlohmann/json.hpp>

namespace ace {

using Entity = entt::entity;
constexpr Entity kNoEntity = entt::null;

struct Name {
  std::string value;
};

struct Transform {
  vec3 position{0.0f};
  quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
  vec3 scale{1.0f};
  Entity parent = kNoEntity;
  mat4 world{1.0f}; // derived by Scene::updateTransforms
  mat4 local() const;
};

struct MeshRenderer {
  std::string mesh = "builtin:cube"; // builtin:cube|sphere|plane|quad or a .gltf/.glb path
  std::string texture;               // optional albedo, project-relative
  vec4 color{1.0f};
  vec3 emissive{0.0f}; // light emitted (path tracer) / unlit add (raster)
  bool worldTexels = false; // map the texture in world space at the scene's texel density
};

struct SpriteRenderer {
  std::string texture;
  vec4 color{1.0f};
  vec2 size{1.0f}; // world units
  bool billboard = false; // always face the camera, staying upright
  // Sprite sheet: a grid of equally sized frames, read left to right, top to bottom.
  int columns = 1, rows = 1;
  int frame = 0;          // first (or only) frame shown
  f32 fps = 0.0f;         // > 0 plays all frames in a loop starting at `frame`
  bool flipX = false;
};

struct Camera {
  bool orthographic = true;
  f32 orthoSize = 5.0f; // half of the visible height, world units
  f32 fov = 50.0f;      // vertical, degrees
  f32 nearPlane = 0.1f;
  f32 farPlane = 200.0f;
  mat4 projection(f32 aspect) const;
};

// Directional light; shines along the entity's -Z axis.
struct Light {
  vec3 color{1.0f, 0.96f, 0.88f};
  f32 intensity = 3.0f;
};

// Axis-aligned box used by Scene::moveAndSlide. Follows the entity's position
// and scale; rotation is ignored.
// shortcut: boxes only, no rotation, no dynamics. Replace with a physics
// library (Jolt was the candidate) when the game needs slopes, pushing or stacking.
struct Collider {
  vec3 size{1.0f};   // full extents before the entity's scale
  vec3 offset{0.0f}; // center, relative to the entity
  bool solid = true; // false: ignored by moveAndSlide (reserved for triggers)
};

struct Script {
  std::string path; // Lua file, project-relative
};

// How a scene is drawn. Lives in the scene file so each level owns its look.
struct RenderSettings {
  bool pathTraced = false;  // false: rasterized toon shading
  bool pixelArt = true;     // render at low resolution and upscale with hard pixels
  int pixelHeight = 180;    // low-resolution image height when pixelArt is on
  bool pixelSnap = true;    // snap orthographic cameras to the texel grid (no shimmer)
  int toonBands = 3;        // 0 = smooth lighting
  f32 dither = 0.6f;        // ordered dithering between light bands and palette colors (0 = none)
  bool texelLighting = true; // evaluate light/shadow per texture texel, not per screen pixel
  int texelsPerUnit = 16;   // texture pixels per world unit for worldTexels meshes
  std::string palette;      // optional image; every on-screen color snaps to one of its (max 64) colors
  bool shadows = true;      // sun shadows in raster mode (the path tracer always has them)
  bool outlines = true;
  f32 outlineStrength = 0.6f;
  f32 highlightStrength = 0.6f;
  vec3 ambient{0.25f, 0.28f, 0.38f};
  vec3 skyTop{0.30f, 0.52f, 0.86f};
  vec3 skyBottom{0.80f, 0.86f, 0.92f};
  int ptSamples = 4; // path tracer: samples per pixel per frame
  int ptBounces = 3;
  f32 exposure = 1.0f;
};

enum class FieldType { Bool, Int, Float, Vec2, Vec3, Color3, Color4, Quat, String, Asset };
struct Field {
  const char* name;
  FieldType type;
  size_t offset;
};
#define ACE_FIELD(Struct, member, type) ::ace::Field{#member, ::ace::FieldType::type, offsetof(Struct, member)}

struct ComponentType {
  const char* name;
  std::vector<Field> fields;
  void* (*get)(entt::registry&, Entity);   // null if the entity lacks it
  void* (*add)(entt::registry&, Entity);   // get-or-add
  void (*remove)(entt::registry&, Entity);
};
const std::vector<ComponentType>& componentTypes();
const std::vector<Field>& renderSettingsFields();

nlohmann::json fieldsToJson(const void* object, const std::vector<Field>& fields);
void fieldsFromJson(void* object, const std::vector<Field>& fields, const nlohmann::json& j);

class Scene {
public:
  entt::registry reg;
  RenderSettings settings;

  Entity create(const std::string& name, Entity parent = kNoEntity);
  void destroy(Entity e); // also destroys children
  Entity find(const std::string& name);
  Entity duplicate(Entity e);
  void setParent(Entity child, Entity parent); // refuses cycles
  std::vector<Entity> children(Entity parent); // kNoEntity = roots, in creation order
  void updateTransforms();

  // Moves an entity that has a Collider by `delta` (world units), stopping at
  // solid colliders one axis at a time so it slides along walls. Returns the
  // movement actually applied. Uses world transforms from the last updateTransforms().
  vec3 moveAndSlide(Entity e, vec3 delta);
  Aabb colliderBounds(Entity e); // world space; empty (min > max) if it has no Collider

  nlohmann::json toJson();
  void fromJson(const nlohmann::json& j); // replaces the whole scene
  bool save(const std::string& path);
  bool load(const std::string& path);
  void clear();

  // Bumped whenever geometry-affecting state may have changed; the path tracer
  // compares it to decide when to rebuild its BVH and restart accumulation.
  u64 version = 1;
};

} // namespace ace
