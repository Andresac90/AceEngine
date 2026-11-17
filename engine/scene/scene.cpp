#include "scene/scene.h"
#include <algorithm>

namespace ace {

using json = nlohmann::json;

mat4 Transform::local() const {
  return glm::translate(mat4(1.0f), position) * glm::mat4_cast(rotation) * glm::scale(mat4(1.0f), scale);
}

mat4 Camera::projection(f32 aspect) const {
  if (orthographic) return glm::orthoRH_ZO(-orthoSize * aspect, orthoSize * aspect, -orthoSize, orthoSize, nearPlane, farPlane);
  return glm::perspectiveRH_ZO(glm::radians(fov), aspect, nearPlane, farPlane);
}

template <typename T> static ComponentType makeType(const char* name, std::vector<Field> fields) {
  return ComponentType{
      name, std::move(fields),
      [](entt::registry& r, Entity e) -> void* { return r.try_get<T>(e); },
      [](entt::registry& r, Entity e) -> void* { return &r.get_or_emplace<T>(e); },
      [](entt::registry& r, Entity e) { r.remove<T>(e); },
  };
}

const std::vector<ComponentType>& componentTypes() {
  static const std::vector<ComponentType> types = {
      makeType<Transform>("Transform", {ACE_FIELD(Transform, position, Vec3), ACE_FIELD(Transform, rotation, Quat),
                                        ACE_FIELD(Transform, scale, Vec3)}),
      makeType<MeshRenderer>("MeshRenderer", {ACE_FIELD(MeshRenderer, mesh, Asset), ACE_FIELD(MeshRenderer, texture, Asset),
                                              ACE_FIELD(MeshRenderer, color, Color4), ACE_FIELD(MeshRenderer, emissive, Color3),
                                              ACE_FIELD(MeshRenderer, worldTexels, Bool)}),
      makeType<SpriteRenderer>("SpriteRenderer", {ACE_FIELD(SpriteRenderer, texture, Asset), ACE_FIELD(SpriteRenderer, color, Color4),
                                                  ACE_FIELD(SpriteRenderer, size, Vec2), ACE_FIELD(SpriteRenderer, billboard, Bool),
                                                  ACE_FIELD(SpriteRenderer, columns, Int), ACE_FIELD(SpriteRenderer, rows, Int),
                                                  ACE_FIELD(SpriteRenderer, directions, Int), ACE_FIELD(SpriteRenderer, frame, Int), ACE_FIELD(SpriteRenderer, fps, Float),
                                                  ACE_FIELD(SpriteRenderer, flipX, Bool)}),
      makeType<Camera>("Camera", {ACE_FIELD(Camera, orthographic, Bool), ACE_FIELD(Camera, orthoSize, Float), ACE_FIELD(Camera, fov, Float),
                                  ACE_FIELD(Camera, nearPlane, Float), ACE_FIELD(Camera, farPlane, Float)}),
      makeType<Light>("Light", {ACE_FIELD(Light, color, Color3), ACE_FIELD(Light, intensity, Float)}),
      makeType<Collider>("Collider", {ACE_FIELD(Collider, size, Vec3), ACE_FIELD(Collider, offset, Vec3), ACE_FIELD(Collider, solid, Bool)}),
      makeType<Script>("Script", {ACE_FIELD(Script, path, Asset), ACE_FIELD(Script, params, String)}),
  };
  return types;
}

const std::vector<Field>& renderSettingsFields() {
  using R = RenderSettings;
  static const std::vector<Field> fields = {
      ACE_FIELD(R, pathTraced, Bool),     ACE_FIELD(R, pixelArt, Bool),         ACE_FIELD(R, pixelHeight, Int),
      ACE_FIELD(R, integerScale, Bool),
      ACE_FIELD(R, pixelSnap, Bool),      ACE_FIELD(R, toonBands, Int),         ACE_FIELD(R, dither, Float),  ACE_FIELD(R, texelLighting, Bool),
      ACE_FIELD(R, texelsPerUnit, Int),   ACE_FIELD(R, palette, Asset),       ACE_FIELD(R, rampShading, Bool), ACE_FIELD(R, rampShadowSteps, Int),         ACE_FIELD(R, shadows, Bool),  ACE_FIELD(R, outlines, Bool),
      ACE_FIELD(R, outlineStrength, Float), ACE_FIELD(R, highlightStrength, Float), ACE_FIELD(R, ambient, Color3),
      ACE_FIELD(R, skyTop, Color3),       ACE_FIELD(R, skyBottom, Color3),      ACE_FIELD(R, ptSamples, Int),
      ACE_FIELD(R, ptBounces, Int),       ACE_FIELD(R, exposure, Float),
  };
  return fields;
}

static int floatCount(FieldType t) {
  switch (t) {
    case FieldType::Float: return 1;
    case FieldType::Vec2: return 2;
    case FieldType::Vec3:
    case FieldType::Color3: return 3;
    case FieldType::Color4:
    case FieldType::Quat: return 4;
    default: return 0;
  }
}

json fieldsToJson(const void* object, const std::vector<Field>& fields) {
  json j = json::object();
  for (const Field& f : fields) {
    const u8* p = (const u8*)object + f.offset;
    switch (f.type) {
      case FieldType::Bool: j[f.name] = *(const bool*)p; break;
      case FieldType::Int: j[f.name] = *(const int*)p; break;
      case FieldType::Float: j[f.name] = *(const f32*)p; break;
      case FieldType::String:
      case FieldType::Asset: j[f.name] = *(const std::string*)p; break;
      default: {
        json a = json::array();
        for (int i = 0; i < floatCount(f.type); i++) a.push_back(((const f32*)p)[i]);
        j[f.name] = a;
      }
    }
  }
  return j;
}

// Tolerant by design: missing or mistyped keys keep the default, so old scene
// files keep loading as components gain fields.
void fieldsFromJson(void* object, const std::vector<Field>& fields, const json& j) {
  if (!j.is_object()) return;
  for (const Field& f : fields) {
    auto it = j.find(f.name);
    if (it == j.end()) continue;
    u8* p = (u8*)object + f.offset;
    switch (f.type) {
      case FieldType::Bool: if (it->is_boolean()) *(bool*)p = it->get<bool>(); break;
      case FieldType::Int: if (it->is_number()) *(int*)p = it->get<int>(); break;
      case FieldType::Float: if (it->is_number()) *(f32*)p = it->get<f32>(); break;
      case FieldType::String:
      case FieldType::Asset: if (it->is_string()) *(std::string*)p = it->get<std::string>(); break;
      default: {
        int n = floatCount(f.type);
        if (!it->is_array() || (int)it->size() != n) break;
        for (int i = 0; i < n; i++)
          if ((*it)[i].is_number()) ((f32*)p)[i] = (*it)[i].get<f32>();
      }
    }
  }
}

Entity Scene::create(const std::string& name, Entity parent) {
  Entity e = reg.create();
  reg.emplace<Name>(e, name);
  reg.emplace<Transform>(e).parent = parent;
  version++;
  return e;
}

void Scene::destroy(Entity e) {
  if (!reg.valid(e)) return;
  for (Entity c : children(e)) destroy(c);
  reg.destroy(e);
  version++;
}

Entity Scene::find(const std::string& name) {
  for (auto [e, n] : reg.view<Name>().each())
    if (n.value == name) return e;
  return kNoEntity;
}

std::vector<Entity> Scene::children(Entity parent) {
  std::vector<Entity> out;
  for (auto [e, t] : reg.view<Transform>().each()) {
    Entity p = reg.valid(t.parent) ? t.parent : kNoEntity;
    if (p == parent) out.push_back(e);
  }
  std::sort(out.begin(), out.end()); // entity ids grow with creation order
  return out;
}

void Scene::setParent(Entity child, Entity parent) {
  for (Entity p = parent; reg.valid(p); p = reg.get<Transform>(p).parent)
    if (p == child) return; // would create a cycle
  // Keep the world transform where it is.
  updateTransforms();
  Transform& t = reg.get<Transform>(child);
  mat4 parentWorld = reg.valid(parent) ? reg.get<Transform>(parent).world : mat4(1.0f);
  mat4 local = glm::inverse(parentWorld) * t.world;
  vec3 skew;
  vec4 persp;
  glm::decompose(local, t.scale, t.rotation, t.position, skew, persp);
  t.parent = parent;
  version++;
}

Entity Scene::duplicate(Entity src) {
  if (!reg.valid(src)) return kNoEntity;
  Entity e = reg.create();
  reg.emplace<Name>(e, reg.get<Name>(src).value + " copy");
  for (const ComponentType& type : componentTypes()) {
    if (void* from = type.get(reg, src)) fieldsFromJson(type.add(reg, e), type.fields, fieldsToJson(from, type.fields));
  }
  reg.get<Transform>(e).parent = reg.get<Transform>(src).parent;
  for (Entity c : children(src)) reg.get<Transform>(duplicate(c)).parent = e;
  version++;
  return e;
}

static void updateWorld(Scene& s, Entity e, const mat4& parentWorld) {
  Transform& t = s.reg.get<Transform>(e);
  t.world = parentWorld * t.local();
  for (Entity c : s.children(e)) updateWorld(s, c, t.world);
}

// shortcut: O(n^2) child lookup; fine for hundreds of entities, replace with a
// cached child list once scenes reach thousands.
void Scene::updateTransforms() {
  for (Entity root : children(kNoEntity)) updateWorld(*this, root, mat4(1.0f));
}

Aabb Scene::colliderBounds(Entity e) {
  Aabb box;
  const Collider* c = reg.valid(e) ? reg.try_get<Collider>(e) : nullptr;
  if (!c) return box;
  const mat4& world = reg.get<Transform>(e).world;
  vec3 scale(glm::length(vec3(world[0])), glm::length(vec3(world[1])), glm::length(vec3(world[2])));
  vec3 center = vec3(world[3]) + c->offset * scale, half = glm::abs(c->size * scale) * 0.5f;
  box.min = center - half;
  box.max = center + half;
  return box;
}

vec3 Scene::moveAndSlide(Entity e, vec3 delta) {
  Aabb me = colliderBounds(e);
  if (me.min.x > me.max.x) { // no collider: move freely
    reg.get<Transform>(e).position += delta;
    return delta;
  }
  std::vector<Aabb> others;
  for (auto [o, c] : reg.view<Collider>().each())
    if (o != e && c.solid) others.push_back(colliderBounds(o));

  const f32 skin = 1e-4f; // touching is not overlapping
  vec3 moved(0.0f);
  for (int axis = 0; axis < 3; axis++) {
    f32 d = delta[axis];
    if (d == 0.0f) continue;
    int u = (axis + 1) % 3, v = (axis + 2) % 3;
    for (const Aabb& o : others) {
      if (me.max[u] <= o.min[u] + skin || me.min[u] >= o.max[u] - skin || me.max[v] <= o.min[v] + skin || me.min[v] >= o.max[v] - skin) continue;
      if (d > 0.0f) {
        f32 gap = o.min[axis] - me.max[axis];
        if (gap >= -skin && gap < d) d = std::max(gap, 0.0f);
      } else {
        f32 gap = o.max[axis] - me.min[axis];
        if (gap <= skin && gap > d) d = std::min(gap, 0.0f);
      }
    }
    me.min[axis] += d;
    me.max[axis] += d;
    moved[axis] = d;
  }
  // shortcut: applies the world-space move to the local position, which is exact
  // only for entities whose parents are not rotated or scaled.
  reg.get<Transform>(e).position += moved;
  reg.get<Transform>(e).world[3] += vec4(moved, 0.0f);
  return moved;
}

std::vector<Entity> Scene::overlapping(Entity e) {
  std::vector<Entity> out;
  Aabb me = colliderBounds(e);
  if (me.min.x > me.max.x) return out;
  for (auto [o, c] : reg.view<Collider>().each()) {
    if (o == e) continue;
    Aabb b = colliderBounds(o);
    if (glm::all(glm::lessThan(me.min, b.max)) && glm::all(glm::greaterThan(me.max, b.min))) out.push_back(o);
  }
  std::sort(out.begin(), out.end());
  return out;
}

Scene::RayHit Scene::raycast(vec3 origin, vec3 direction, f32 maxDistance, Entity ignore, bool includeTriggers) {
  RayHit best;
  f32 len = glm::length(direction);
  if (len <= 0.0f) return best;
  vec3 d = direction / len;
  best.distance = maxDistance;
  for (auto [o, c] : reg.view<Collider>().each()) {
    if (o == ignore || (!c.solid && !includeTriggers)) continue;
    Aabb b = colliderBounds(o);
    f32 t0 = 0.0f, t1 = best.distance;
    int axis = -1;
    bool miss = false;
    for (int a = 0; a < 3 && !miss; a++) {
      if (fabsf(d[a]) < 1e-12f) {
        miss = origin[a] < b.min[a] || origin[a] > b.max[a];
        continue;
      }
      f32 lo = (b.min[a] - origin[a]) / d[a], hi = (b.max[a] - origin[a]) / d[a];
      if (lo > hi) std::swap(lo, hi);
      if (lo > t0) {
        t0 = lo;
        axis = a;
      }
      t1 = std::min(t1, hi);
      miss = t0 > t1;
    }
    if (miss || t0 >= best.distance) continue;
    best.entity = o;
    best.distance = t0;
    best.normal = vec3(0.0f);
    if (axis >= 0) best.normal[axis] = d[axis] > 0.0f ? -1.0f : 1.0f; // axis < 0: the ray started inside
  }
  if (best.entity == kNoEntity) return RayHit{};
  best.point = origin + d * best.distance;
  return best;
}

json Scene::entitiesToJson(const std::vector<Entity>& order) {
  auto indexOf = [&](Entity e) {
    auto it = std::find(order.begin(), order.end(), e);
    return it == order.end() ? -1 : (int)(it - order.begin());
  };
  json entities = json::array();
  for (Entity e : order) {
    json je;
    je["name"] = reg.get<Name>(e).value;
    je["parent"] = indexOf(reg.get<Transform>(e).parent); // -1 for roots and parents outside the list
    for (const ComponentType& type : componentTypes())
      if (void* c = type.get(reg, e)) je["components"][type.name] = fieldsToJson(c, type.fields);
    entities.push_back(je);
  }
  return entities;
}

std::vector<Entity> Scene::entitiesFromJson(const json& entities, Entity parent) {
  std::vector<Entity> created;
  std::vector<int> parents;
  if (!entities.is_array()) return created;
  for (const json& je : entities) {
    if (!je.is_object()) continue;
    Entity e = create(je.value("name", std::string("Entity")), parent);
    created.push_back(e);
    parents.push_back(je.contains("parent") && je["parent"].is_number_integer() ? je["parent"].get<int>() : -1);
    if (!je.contains("components") || !je["components"].is_object()) continue;
    for (const ComponentType& type : componentTypes()) {
      auto it = je["components"].find(type.name);
      if (it != je["components"].end()) fieldsFromJson(type.add(reg, e), type.fields, *it);
    }
    reg.get<Transform>(e).parent = parent; // the Transform component above must not override the attachment point
  }
  // Link parents, refusing anything that would form a cycle (a damaged file
  // could otherwise make destroy() recurse forever).
  for (size_t i = 0; i < created.size(); i++) {
    int p = parents[i];
    if (p < 0 || p >= (int)created.size() || p == (int)i) continue;
    bool cycle = false;
    for (Entity a = created[p]; reg.valid(a) && !cycle; a = reg.get<Transform>(a).parent) cycle = a == created[i];
    if (cycle) ACE_WARN("entity '%s': parent link would form a cycle; ignored", reg.get<Name>(created[i]).value.c_str());
    else reg.get<Transform>(created[i]).parent = created[p];
  }
  return created;
}

json Scene::toJson() {
  std::vector<Entity> order;
  for (Entity e : reg.view<Name>()) order.push_back(e);
  std::sort(order.begin(), order.end());
  return json{{"version", 1}, {"settings", fieldsToJson(&settings, renderSettingsFields())}, {"entities", entitiesToJson(order)}};
}

void Scene::fromJson(const json& j) {
  clear();
  settings = RenderSettings{};
  if (j.contains("settings")) fieldsFromJson(&settings, renderSettingsFields(), j["settings"]);
  if (j.contains("entities")) entitiesFromJson(j["entities"]);
  updateTransforms();
  version++;
}

json Scene::copyEntities(const std::vector<Entity>& roots) {
  updateTransforms();
  std::vector<Entity> list;
  std::vector<size_t> rootIndex;
  for (Entity r : roots) {
    if (!reg.valid(r) || std::find(list.begin(), list.end(), r) != list.end()) continue;
    rootIndex.push_back(list.size());
    size_t first = list.size();
    list.push_back(r);
    for (size_t i = first; i < list.size(); i++)
      for (Entity c : children(list[i])) list.push_back(c);
  }
  json entities = entitiesToJson(list);
  for (size_t i : rootIndex) {
    Transform world;
    vec3 skew;
    vec4 persp;
    glm::decompose(reg.get<Transform>(list[i]).world, world.scale, world.rotation, world.position, skew, persp);
    for (const ComponentType& type : componentTypes())
      if (strcmp(type.name, "Transform") == 0) entities[i]["components"]["Transform"] = fieldsToJson(&world, type.fields);
    entities[i]["parent"] = -1;
  }
  return json{{"version", 1}, {"entities", entities}};
}

std::vector<Entity> Scene::pasteEntities(const json& copied) {
  std::vector<Entity> roots;
  if (!copied.is_object() || !copied.contains("entities")) return roots;
  std::vector<Entity> created = entitiesFromJson(copied["entities"]);
  for (Entity e : created)
    if (!reg.valid(reg.get<Transform>(e).parent)) roots.push_back(e);
  updateTransforms();
  return roots;
}

bool Scene::savePrefab(Entity root, const std::string& path) {
  if (!reg.valid(root)) return false;
  json j = copyEntities({root}); // root first, in world space
  j["prefab"] = true;
  return writeTextFile(path, j.dump(2));
}

// shortcut: re-reads and parses the file on every spawn; cache parsed prefabs if
// spawning many per frame shows up in a profile.
Entity Scene::instantiatePrefab(const std::string& path, Entity parent) {
  std::string text;
  if (!readTextFile(path, text)) {
    ACE_ERROR("cannot open prefab %s", path.c_str());
    return kNoEntity;
  }
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded() || !j.contains("entities")) {
    ACE_ERROR("prefab %s is not valid", path.c_str());
    return kNoEntity;
  }
  std::vector<Entity> created = entitiesFromJson(j["entities"], parent);
  updateTransforms();
  return created.empty() ? kNoEntity : created[0];
}

void Scene::clear() {
  reg.clear();
  version++;
}

bool Scene::save(const std::string& path) { return writeTextFile(path, toJson().dump(2)); }

bool Scene::load(const std::string& path) {
  std::string text;
  if (!readTextFile(path, text)) {
    ACE_ERROR("cannot open scene %s", path.c_str());
    return false;
  }
  json j = json::parse(text, nullptr, false);
  if (j.is_discarded()) {
    ACE_ERROR("scene %s is not valid JSON", path.c_str());
    return false;
  }
  fromJson(j);
  return true;
}

} // namespace ace
