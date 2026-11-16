// Asset loading and caching. Paths are project-relative; "builtin:*" meshes are
// generated. Assets are loaded on first use and cached for the project lifetime.
#pragma once
#include "core/base.h"
#include "core/math.h"
#include "rhi/rhi.h"
#include <unordered_map>

namespace ace {

struct Vertex {
  vec3 position;
  vec3 normal;
  vec2 uv;
  u8 color[4]; // sRGB-encoded RGB + alpha
};

struct Mesh {
  std::vector<Vertex> vertices; // kept on the CPU for picking and the path tracer's BVH
  std::vector<u32> indices;
  rhi::BufferHandle vertexBuffer, indexBuffer;
  Aabb bounds;
};

struct Texture {
  rhi::TextureHandle handle;
  u32 width = 0, height = 0;
  std::vector<u8> pixels; // RGBA8, kept for the path tracer's texture atlas
};

class Assets {
public:
  Assets(rhi::Device& device, std::string projectDir);
  ~Assets();

  // Never return null: failures log once and fall back to a cube / white texture.
  const Mesh* mesh(const std::string& path);
  const Texture* texture(const std::string& path); // empty path = white
  std::string resolve(const std::string& path) const;
  const std::string& projectDir() const { return root_; }
  void reload(); // drop file-backed assets so they are re-read (editor hot reload)

private:
  Mesh* upload(Mesh&& mesh);
  rhi::Device& device_;
  std::string root_;
  std::unordered_map<std::string, std::unique_ptr<Mesh>> meshes_;
  std::unordered_map<std::string, std::unique_ptr<Texture>> textures_;
};

} // namespace ace
