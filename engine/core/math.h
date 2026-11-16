#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/matrix_decompose.hpp>

namespace ace {
using glm::mat3;
using glm::mat4;
using glm::quat;
using glm::vec2;
using glm::vec3;
using glm::vec4;

struct Aabb {
  vec3 min{1e30f}, max{-1e30f};
  void grow(vec3 p) {
    min = glm::min(min, p);
    max = glm::max(max, p);
  }
};

// Colors in scene files and the editor are sRGB; shading happens in linear space.
inline vec3 toLinear(vec3 c) { return glm::pow(glm::max(c, vec3(0.0f)), vec3(2.2f)); }
inline vec4 toLinear(vec4 c) { return vec4(toLinear(vec3(c)), c.a); }

// Conventions (identical on every backend): right-handed world, +Y up, cameras
// look down -Z, clip-space depth 0..1, +Y up in NDC, counter-clockwise front faces.
} // namespace ace
