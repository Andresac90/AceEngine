#include "assets/assets.h"

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#include <stb_image.h>
#define CGLTF_IMPLEMENTATION
#include <cgltf.h>

namespace ace {

static void addQuad(Mesh& m, vec3 origin, vec3 u, vec3 v) {
  vec3 n = glm::normalize(glm::cross(u, v));
  u32 base = (u32)m.vertices.size();
  const vec2 uvs[4] = {{0, 1}, {1, 1}, {1, 0}, {0, 0}};
  const vec3 pos[4] = {origin, origin + u, origin + u + v, origin + v};
  for (int i = 0; i < 4; i++) m.vertices.push_back({pos[i], n, uvs[i], {255, 255, 255, 255}});
  for (u32 i : {0u, 1u, 2u, 0u, 2u, 3u}) m.indices.push_back(base + i);
}

static Mesh makeBuiltin(const std::string& name) {
  Mesh m;
  if (name == "builtin:quad") {
    addQuad(m, {-0.5f, -0.5f, 0}, {1, 0, 0}, {0, 1, 0});
  } else if (name == "builtin:plane") {
    addQuad(m, {-0.5f, 0, 0.5f}, {1, 0, 0}, {0, 0, -1});
  } else if (name == "builtin:sphere") {
    const int rings = 12, segs = 20;
    for (int r = 0; r <= rings; r++) {
      for (int s = 0; s <= segs; s++) {
        f32 phi = glm::pi<f32>() * r / rings, theta = glm::two_pi<f32>() * s / segs;
        vec3 n{sinf(phi) * cosf(theta), cosf(phi), sinf(phi) * sinf(theta)};
        m.vertices.push_back({n * 0.5f, n, {(f32)s / segs, (f32)r / rings}, {255, 255, 255, 255}});
      }
    }
    for (int r = 0; r < rings; r++) {
      for (int s = 0; s < segs; s++) {
        u32 a = r * (segs + 1) + s, b = a + segs + 1;
        for (u32 i : {a, a + 1, b, a + 1, b + 1, b}) m.indices.push_back(i);
      }
    }
  } else { // builtin:cube, also the fallback for anything that failed to load
    const f32 h = 0.5f;
    addQuad(m, {-h, -h, h}, {1, 0, 0}, {0, 1, 0});
    addQuad(m, {h, -h, -h}, {-1, 0, 0}, {0, 1, 0});
    addQuad(m, {h, -h, h}, {0, 0, -1}, {0, 1, 0});
    addQuad(m, {-h, -h, -h}, {0, 0, 1}, {0, 1, 0});
    addQuad(m, {-h, h, h}, {1, 0, 0}, {0, 0, -1});
    addQuad(m, {-h, -h, -h}, {1, 0, 0}, {0, 0, 1});
  }
  return m;
}

// Flattens every triangle primitive of a glTF file into one mesh, baking node
// transforms and material base colors into the vertices.
// shortcut: no skinning, animation or per-primitive materials yet.
static bool loadGltf(const std::string& file, Mesh& m) {
  std::vector<u8> bytes;
  if (!readFile(file, bytes)) return false;
  cgltf_options options{};
  cgltf_data* data = nullptr;
  if (cgltf_parse(&options, bytes.data(), bytes.size(), &data) != cgltf_result_success) return false;
  if (cgltf_load_buffers(&options, data, file.c_str()) != cgltf_result_success) {
    cgltf_free(data);
    return false;
  }
  for (cgltf_size ni = 0; ni < data->nodes_count; ni++) {
    cgltf_node& node = data->nodes[ni];
    if (!node.mesh) continue;
    mat4 world;
    cgltf_node_transform_world(&node, &world[0][0]);
    mat3 normalMatrix = glm::transpose(glm::inverse(mat3(world)));
    for (cgltf_size pi = 0; pi < node.mesh->primitives_count; pi++) {
      cgltf_primitive& prim = node.mesh->primitives[pi];
      if (prim.type != cgltf_primitive_type_triangles) continue;
      const cgltf_accessor *pos = nullptr, *nrm = nullptr, *uv = nullptr, *col = nullptr;
      for (cgltf_size ai = 0; ai < prim.attributes_count; ai++) {
        cgltf_attribute& a = prim.attributes[ai];
        if (a.type == cgltf_attribute_type_position) pos = a.data;
        if (a.type == cgltf_attribute_type_normal) nrm = a.data;
        if (a.type == cgltf_attribute_type_texcoord && a.index == 0) uv = a.data;
        if (a.type == cgltf_attribute_type_color && a.index == 0) col = a.data;
      }
      if (!pos) continue;
      vec4 base{1.0f};
      if (prim.material && prim.material->has_pbr_metallic_roughness)
        memcpy(&base, prim.material->pbr_metallic_roughness.base_color_factor, sizeof(base));
      u32 first = (u32)m.vertices.size();
      for (cgltf_size v = 0; v < pos->count; v++) {
        Vertex vert{};
        f32 p[3] = {}, n[3] = {0, 1, 0}, t[2] = {}, c[4] = {1, 1, 1, 1};
        cgltf_accessor_read_float(pos, v, p, 3);
        if (nrm) cgltf_accessor_read_float(nrm, v, n, 3);
        if (uv) cgltf_accessor_read_float(uv, v, t, 2);
        if (col) cgltf_accessor_read_float(col, v, c, 4);
        vert.position = vec3(world * vec4(p[0], p[1], p[2], 1.0f));
        vert.normal = glm::normalize(normalMatrix * vec3(n[0], n[1], n[2]));
        vert.uv = {t[0], t[1]};
        // glTF colors are linear; vertex colors are stored sRGB-encoded (8 bits
        // hold dark tones far better that way) and linearized again when shading.
        for (int i = 0; i < 4; i++) {
          f32 value = glm::clamp(c[i] * base[i], 0.0f, 1.0f);
          if (i < 3) value = powf(value, 1.0f / 2.2f);
          vert.color[i] = (u8)(value * 255.0f + 0.5f);
        }
        m.vertices.push_back(vert);
      }
      if (prim.indices) {
        for (cgltf_size i = 0; i < prim.indices->count; i++) m.indices.push_back(first + (u32)cgltf_accessor_read_index(prim.indices, i));
      } else {
        for (cgltf_size i = 0; i < pos->count; i++) m.indices.push_back(first + (u32)i);
      }
    }
  }
  cgltf_free(data);
  return !m.indices.empty();
}

Assets::Assets(rhi::Device& device, std::string projectDir) : device_(device), root_(std::move(projectDir)) {
  if (!root_.empty() && root_.back() != '/') root_ += '/';
}

Assets::~Assets() { reload(); }

void Assets::reload() {
  device_.waitIdle();
  for (auto& [k, m] : meshes_) {
    device_.destroy(m->vertexBuffer);
    device_.destroy(m->indexBuffer);
  }
  for (auto& [k, t] : textures_) device_.destroy(t->handle);
  meshes_.clear();
  textures_.clear();
}

std::string Assets::resolve(const std::string& path) const { return root_ + path; }

Mesh* Assets::upload(Mesh&& mesh) {
  auto m = std::make_unique<Mesh>(std::move(mesh));
  for (const Vertex& v : m->vertices) m->bounds.grow(v.position);
  m->vertexBuffer = device_.createBuffer({m->vertices.size() * sizeof(Vertex), rhi::BufferUsage_Vertex, "mesh.vb"});
  m->indexBuffer = device_.createBuffer({m->indices.size() * sizeof(u32), rhi::BufferUsage_Index, "mesh.ib"});
  device_.writeBuffer(m->vertexBuffer, 0, m->vertices.data(), m->vertices.size() * sizeof(Vertex));
  device_.writeBuffer(m->indexBuffer, 0, m->indices.data(), m->indices.size() * sizeof(u32));
  return m.release();
}

const Mesh* Assets::mesh(const std::string& path) {
  auto it = meshes_.find(path);
  if (it != meshes_.end()) return it->second.get();
  Mesh m;
  if (path.rfind("builtin:", 0) == 0) {
    m = makeBuiltin(path);
  } else if (!loadGltf(resolve(path), m)) {
    ACE_WARN("mesh '%s' could not be loaded; using a cube", path.c_str());
    m = makeBuiltin("builtin:cube");
  }
  meshes_[path].reset(upload(std::move(m)));
  return meshes_[path].get();
}

const Texture* Assets::texture(const std::string& path) {
  auto it = textures_.find(path);
  if (it != textures_.end()) return it->second.get();
  auto t = std::make_unique<Texture>();
  std::vector<u8> bytes;
  int w = 0, h = 0, n = 0;
  stbi_uc* pixels = nullptr;
  if (!path.empty()) {
    if (readFile(resolve(path), bytes)) pixels = stbi_load_from_memory(bytes.data(), (int)bytes.size(), &w, &h, &n, 4);
    if (!pixels) ACE_WARN("texture '%s' could not be loaded; using white", path.c_str());
  }
  if (pixels) {
    t->pixels.assign(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
  } else {
    w = h = 1;
    t->pixels = {255, 255, 255, 255};
  }
  t->width = (u32)w;
  t->height = (u32)h;
  t->handle = device_.createTexture({t->width, t->height, rhi::Format::RGBA8_SRGB, rhi::TextureUsage_Sampled, "texture"});
  device_.writeTexture(t->handle, t->pixels.data());
  textures_[path] = std::move(t);
  return textures_[path].get();
}

} // namespace ace
