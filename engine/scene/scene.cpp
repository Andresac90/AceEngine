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
                                              ACE_FIELD(MeshRenderer, color, Color4), ACE_FIELD(MeshRenderer, emissive, Color3)}),
      makeType<SpriteRenderer>("SpriteRenderer", {ACE_FIELD(SpriteRenderer, texture, Asset), ACE_FIELD(SpriteRenderer, color, Color4),
                                                  ACE_FIELD(SpriteRenderer, size, Vec2)}),
      makeType<Camera>("Camera", {ACE_FIELD(Camera, orthographic, Bool), ACE_FIELD(Camera, orthoSize, Float), ACE_FIELD(Camera, fov, Float),
                                  ACE_FIELD(Camera, nearPlane, Float), ACE_FIELD(Camera, farPlane, Float)}),
      makeType<Light>("Light", {ACE_FIELD(Light, color, Color3), ACE_FIELD(Light, intensity, Float)}),
      makeType<Script>("Script", {ACE_FIELD(Script, path, Asset)}),
  };
  return types;
}

const std::vector<Field>& renderSettingsFields() {
  using R = RenderSettings;
  static const std::vector<Field> fields = {
      ACE_FIELD(R, pathTraced, Bool),     ACE_FIELD(R, pixelArt, Bool),         ACE_FIELD(R, pixelHeight, Int),
      ACE_FIELD(R, pixelSnap, Bool),      ACE_FIELD(R, toonBands, Int),         ACE_FIELD(R, shadows, Bool),  ACE_FIELD(R, outlines, Bool),
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

json Scene::toJson() {
  std::vector<Entity> order;
  for (Entity e : reg.view<Name>()) order.push_back(e);
  std::sort(order.begin(), order.end());
  auto indexOf = [&](Entity e) {
    auto it = std::find(order.begin(), order.end(), e);
    return it == order.end() ? -1 : (int)(it - order.begin());
  };
  json entities = json::array();
  for (Entity e : order) {
    json je;
    je["name"] = reg.get<Name>(e).value;
    je["parent"] = indexOf(reg.get<Transform>(e).parent);
    for (const ComponentType& type : componentTypes())
      if (void* c = type.get(reg, e)) je["components"][type.name] = fieldsToJson(c, type.fields);
    entities.push_back(je);
  }
  return json{{"version", 1}, {"settings", fieldsToJson(&settings, renderSettingsFields())}, {"entities", entities}};
}

void Scene::fromJson(const json& j) {
  clear();
  settings = RenderSettings{};
  if (j.contains("settings")) fieldsFromJson(&settings, renderSettingsFields(), j["settings"]);
  std::vector<Entity> created;
  std::vector<int> parents;
  if (j.contains("entities") && j["entities"].is_array()) {
    for (const json& je : j["entities"]) {
      Entity e = create(je.value("name", std::string("Entity")));
      created.push_back(e);
      parents.push_back(je.value("parent", -1));
      if (!je.contains("components")) continue;
      for (const ComponentType& type : componentTypes()) {
        auto it = je["components"].find(type.name);
        if (it != je["components"].end()) fieldsFromJson(type.add(reg, e), type.fields, *it);
      }
    }
  }
  for (size_t i = 0; i < created.size(); i++)
    if (parents[i] >= 0 && parents[i] < (int)created.size() && parents[i] != (int)i)
      reg.get<Transform>(created[i]).parent = created[parents[i]];
  updateTransforms();
  version++;
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
