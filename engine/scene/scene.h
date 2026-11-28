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
  // Path tracer only: mirror-like reflection on top of the diffuse color.
  f32 reflectivity = 0.0f; // Fresnel reflectance facing the surface (Schlick F0); 0 = matte. Ice ~0.1
  f32 roughness = 0.0f;    // blur of the reflection (0 = mirror)
};

struct SpriteRenderer {
  std::string texture;
  vec4 color{1.0f};
  vec2 size{1.0f}; // world units
  bool billboard = false; // always face the camera, staying upright
  // Sprite sheet: a grid of equally sized frames, read left to right, top to bottom.
  int columns = 1, rows = 1;
  // Directional sheet: 4 or 8 rows, one per viewing angle (see renderer.h); columns are the animation frames.
  int directions = 1;
  int frame = 0;          // first (or only) frame shown
  f32 fps = 0.0f;         // > 0 plays all frames in a loop starting at `frame`
  bool flipX = false;
};

// A large animated crowd drawn and path traced without one entity per person.
// `pack` (*.crowd.json) holds the character models, split into rigid parts (one
// per bone), and their animation clips baked as one rigid transform per bone per
// frame. `members` (*.members.json) places every person: position, facing,
// model, team, idle clip, speed, phase, stadium-wave delay and three colors.
// See render/crowd.h. The runtime fields below are set by scripts
// (ace.crowd_event, ace.crowd_wave) and are not saved.
struct Crowd {
  std::string pack;
  std::string members;
  f32 speed = 1.0f; // animation playback rate for everyone
  // Times are on the renderer's clock. Scripts only request (`pending`); the
  // renderer stamps the start, because script and render clocks may differ.
  struct Event {
    std::string clip; // empty = none
    f64 start = 0.0, end = 0.0, seconds = 0.0;
    bool pending = false;
  };
  Event teams[3];         // per team (0 home, 1 away, 2 neutral): play `clip` between start and end
  f64 waveStart = -1.0;   // < 0: no stadium wave
  int waveLaps = 0;
  bool wavePending = false;
};

// One extra crowd person driven by its entity's Transform (skaters, mascots):
// drawn with the scene's first Crowd pack and animated like the members.
struct CrowdActor {
  int model = 0;
  int clip = 0;
  int team = 0;
  f32 speed = 1.0f;
  vec3 primary{1.0f}, secondary{1.0f}, accent{1.0f}; // sRGB
};

struct Camera {
  bool orthographic = true;
  f32 orthoSize = 5.0f; // half of the visible height, world units
  f32 fov = 50.0f;      // vertical, degrees
  f32 nearPlane = 0.1f;
  f32 farPlane = 200.0f;
  mat4 projection(f32 aspect) const;
};

// A sun (directional: shines along the entity's -Z axis) or, with `point` set,
// a lamp/torch at the entity's position that fades out at `range`.
// The first non-point light is the sun. Up to 8 point lights are used per frame
// (the 8 nearest the camera); they cast no shadows.
// Point lights cast shadows in path-traced mode only.
// shortcut: no raster shadows for point lights; add cube shadow maps if
// torch-lit dungeons need them without path tracing.
struct Light {
  vec3 color{1.0f, 0.96f, 0.88f};
  f32 intensity = 3.0f;
  bool point = false;
  f32 range = 6.0f; // world units (point lights)
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
  std::string path;   // Lua file, project-relative
  std::string params; // "key=value; key=value": set as fields on the script instance (numbers, true/false, or text)
};

// How a scene is drawn. Lives in the scene file so each level owns its look.
struct RenderSettings {
  bool pathTraced = false;  // false: rasterized toon shading
  // World pixels (texel splatting): the scene is captured into low-resolution
  // cube maps around a probe snapped to a world grid, and every captured texel
  // is drawn as a small square in the 3D world. Pixels then stay put when the
  // camera turns or moves, even with a perspective camera. Orthographic cameras
  // use the screen-pixel path instead (grid snapping is already stable for them).
  bool worldPixels = false;
  int probeResolution = 384; // texels per cube face: lower = bigger pixels
  f32 probeCell = 4.0f;      // grid spacing (world units) the stable probe snaps to; larger = fewer re-samplings
  bool pixelArt = true;     // render at low resolution and upscale with hard pixels
  int pixelHeight = 180;    // low-resolution image height when pixelArt is on
  bool integerScale = true; // adjust that height slightly so every game pixel is the same whole number of screen pixels
  bool pixelSnap = true;    // snap orthographic cameras to the texel grid (no shimmer)
  int toonBands = 3;        // 0 = smooth lighting
  f32 dither = 0.6f;        // ordered dithering between light bands and palette colors (0 = none)
  bool texelLighting = true; // evaluate light/shadow per texture texel, not per screen pixel
  int texelsPerUnit = 16;   // texture pixels per world unit for worldTexels meshes
  std::string palette;      // optional image; every on-screen color snaps to one of its (max 64) colors
  bool rampShading = false; // shade by stepping along the palette's rows (dark -> light ramps) instead of darkening
  int rampShadowSteps = 2;  // how many ramp colors darker a fully shadowed surface gets
  bool shadows = true;      // sun shadows in raster mode (the path tracer always has them)
  bool outlines = true;
  f32 outlineStrength = 0.6f;
  f32 highlightStrength = 0.6f;
  vec3 ambient{0.25f, 0.28f, 0.38f};
  vec3 skyTop{0.30f, 0.52f, 0.86f};
  vec3 skyBottom{0.80f, 0.86f, 0.92f};
  f32 sunSize = 2.0f;        // sun disc radius in degrees (0 = no disc)
  f32 sunGlow = 0.6f;        // glow around the sun, in the sky and in the haze
  vec3 fogColor{0.75f, 0.78f, 0.85f};
  f32 fogDensity = 0.0f;     // haze per world unit (0 = none; 0.02 = far things fade over ~50 units)
  f32 cloudCoverage = 0.0f;  // 0 = clear sky, 1 = overcast
  f32 cloudScale = 1.2f;     // larger = smaller clouds
  f32 cloudSpeed = 0.01f;    // drift
  f32 stars = 0.0f;          // stars where the sky is dark (0..1)
  int ptSamples = 4; // path tracer: samples per pixel per frame
  int ptBounces = 3;
  bool denoise = true; // path tracing: smooth the light (not the textures) while samples are few
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
  // Entities whose Collider overlaps e's (solid or not): triggers, pickups, doors.
  std::vector<Entity> overlapping(Entity e);
  struct RayHit {
    Entity entity = kNoEntity;
    f32 distance = 0.0f;
    vec3 point{0.0f}, normal{0.0f};
  };
  // Nearest Collider hit by the ray within maxDistance; `ignore` is skipped.
  // Non-solid colliders are hit only if includeTriggers is set.
  RayHit raycast(vec3 origin, vec3 direction, f32 maxDistance, Entity ignore = kNoEntity, bool includeTriggers = false);

  // Prefabs: a saved entity subtree (*.prefab.json). Instances are independent
  // copies: editing the prefab file later does not change them.
  // shortcut: no live link back to the prefab; add one (prefab path + overrides) if levels need it.
  bool savePrefab(Entity root, const std::string& path);
  // Copy/paste: the subtrees under `roots` as JSON, with each root's transform
  // stored in world space so it lands in the same place when pasted at the top level.
  nlohmann::json copyEntities(const std::vector<Entity>& roots);
  std::vector<Entity> pasteEntities(const nlohmann::json& copied); // returns the new roots
  Entity instantiatePrefab(const std::string& path, Entity parent = kNoEntity); // returns the new root

  nlohmann::json toJson();
  void fromJson(const nlohmann::json& j); // replaces the whole scene
  bool save(const std::string& path);
  bool load(const std::string& path);
  void clear();

  // Bumped whenever geometry-affecting state may have changed; the path tracer
  // compares it to decide when to rebuild its BVH and restart accumulation.
  u64 version = 1;

private:
  // Entities in `order` as a JSON array; parents are indices into the array.
  nlohmann::json entitiesToJson(const std::vector<Entity>& order);
  // Creates the entities of such an array; entries without a parent attach to `parent`.
  std::vector<Entity> entitiesFromJson(const nlohmann::json& entities, Entity parent = kNoEntity);
};

} // namespace ace
