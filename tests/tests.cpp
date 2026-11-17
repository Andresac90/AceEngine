// CPU-only engine tests (no window or GPU needed). Run with: ctest --test-dir build
#include "render/path_tracer.h"
#include "scene/scene.h"

using namespace ace;

static int failures = 0;
#define CHECK(cond)                                                \
  do {                                                             \
    if (!(cond)) {                                                 \
      printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
      failures++;                                                  \
    }                                                              \
  } while (0)

static void testSceneRoundTrip() {
  Scene a;
  Entity parent = a.create("Parent");
  a.reg.get<Transform>(parent).position = {1, 2, 3};
  Entity child = a.create("Child", parent);
  a.reg.get<Transform>(child).position = {0, 1, 0};
  auto& mr = a.reg.emplace<MeshRenderer>(child);
  mr.mesh = "builtin:sphere";
  mr.color = {0.5f, 0.25f, 1.0f, 1.0f};
  a.settings.pixelHeight = 144;
  a.settings.pathTraced = true;

  Scene b;
  b.fromJson(a.toJson());
  Entity bc = b.find("Child");
  CHECK(bc != kNoEntity);
  CHECK(b.reg.get<Transform>(bc).parent == b.find("Parent"));
  CHECK(b.reg.get<MeshRenderer>(bc).mesh == "builtin:sphere");
  CHECK(b.reg.get<MeshRenderer>(bc).color.y == 0.25f);
  CHECK(b.settings.pixelHeight == 144 && b.settings.pathTraced);
  b.updateTransforms();
  CHECK(glm::distance(vec3(b.reg.get<Transform>(bc).world[3]), vec3(1, 3, 3)) < 1e-5f);
  CHECK(a.toJson() == b.toJson());
}

static void testSceneTolerantLoad() {
  Scene s;
  // Wrong types, missing keys and a self-parent must not crash or corrupt.
  s.fromJson(nlohmann::json::parse(R"({"entities":[{"name":"A","parent":0,
    "components":{"Transform":{"position":"oops","scale":[1,2]},"Camera":{"fov":"wide"}}},{"parent":99}]})"));
  Entity a = s.find("A");
  CHECK(a != kNoEntity);
  CHECK(s.reg.get<Transform>(a).parent == kNoEntity);
  CHECK(s.reg.get<Transform>(a).scale == vec3(1.0f));
  CHECK(s.reg.get<Camera>(a).fov == 50.0f);
}

static void testHierarchy() {
  Scene s;
  Entity a = s.create("a"), b = s.create("b", a), c = s.create("c", b);
  s.setParent(a, c); // cycle: must be refused
  CHECK(s.reg.get<Transform>(a).parent == kNoEntity);
  s.reg.get<Transform>(a).position = {5, 0, 0};
  s.setParent(c, kNoEntity); // keeps world position
  s.updateTransforms();
  CHECK(glm::distance(vec3(s.reg.get<Transform>(c).world[3]), vec3(5, 0, 0)) < 1e-4f);
  s.destroy(a); // destroys b too, not c
  CHECK(!s.reg.valid(b) && s.reg.valid(c));
}

// Every triangle must be reachable from the root, inside its ancestors' boxes.
static void testBvh() {
  std::vector<PathTracer::Triangle> tris;
  for (int i = 0; i < 1000; i++) {
    PathTracer::Triangle t{};
    t.v0 = vec4((f32)(i % 10), (f32)((i / 10) % 10), (f32)(i / 100), 0);
    t.e1 = vec4(0.5f, 0, 0, 0);
    t.e2 = vec4(0, 0.5f, 0, 0);
    tris.push_back(t);
  }
  std::vector<PathTracer::Node> nodes;
  PathTracer::buildBvh(tris, nodes);
  size_t seen = 0;
  std::vector<u32> stack{0};
  while (!stack.empty()) {
    PathTracer::Node n = nodes[stack.back()];
    stack.pop_back();
    if (n.count == 0) {
      for (u32 c : {n.leftFirst, n.leftFirst + 1}) {
        CHECK(glm::all(glm::greaterThanEqual(nodes[c].min, n.min - 1e-4f)) && glm::all(glm::lessThanEqual(nodes[c].max, n.max + 1e-4f)));
        stack.push_back(c);
      }
      continue;
    }
    for (u32 i = n.leftFirst; i < n.leftFirst + n.count; i++) {
      CHECK(glm::all(glm::greaterThanEqual(vec3(tris[i].v0), n.min - 1e-4f)));
      seen++;
    }
  }
  CHECK(seen == tris.size());
  std::vector<PathTracer::Triangle> none;
  PathTracer::buildBvh(none, nodes);
  CHECK(nodes.size() == 1 && nodes[0].count == 0);
}

static void testMoveAndSlide() {
  Scene s;
  Entity player = s.create("player"), wall = s.create("wall");
  s.reg.emplace<Collider>(player).size = vec3(1.0f);
  s.reg.emplace<Collider>(wall).size = vec3(1.0f, 1.0f, 10.0f);
  s.reg.get<Transform>(wall).position = {3, 0, 0};
  s.updateTransforms();
  // Walking diagonally into the wall: X stops at contact (gap of 2), Z keeps going.
  vec3 moved = s.moveAndSlide(player, {5, 0, 1});
  CHECK(fabsf(moved.x - 2.0f) < 1e-4f && moved.z == 1.0f);
  CHECK(fabsf(s.reg.get<Transform>(player).position.x - 2.0f) < 1e-4f);
  // Already touching: cannot go further in, can back away, and never tunnels.
  CHECK(s.moveAndSlide(player, {0.5f, 0, 0}).x == 0.0f);
  CHECK(s.moveAndSlide(player, {100, 0, 0}).x == 0.0f);
  CHECK(s.moveAndSlide(player, {-1, 0, 0}).x == -1.0f);
  // Non-solid colliders and missing colliders do not block.
  s.reg.get<Collider>(wall).solid = false;
  CHECK(s.moveAndSlide(player, {10, 0, 0}).x == 10.0f);
  Entity ghost = s.create("ghost");
  CHECK(s.moveAndSlide(ghost, {1, 2, 3}) == vec3(1, 2, 3));
}

int main() {
  testSceneRoundTrip();
  testSceneTolerantLoad();
  testHierarchy();
  testBvh();
  testMoveAndSlide();
  printf(failures ? "%d check(s) failed\n" : "all tests passed\n", failures);
  return failures ? 1 : 0;
}
