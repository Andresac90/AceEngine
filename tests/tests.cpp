// CPU-only engine tests (no window or GPU needed). Run with: ctest --test-dir build
#include "render/crowd.h"
#include "render/path_tracer.h"
#include "render/renderer.h"
#include "render/texel_splat.h"
#include "assets/assets.h"
#include "scene/scene.h"
#include "script/script.h"
#include "ui_text.h" // editor/
#include <filesystem>

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

static void testQueries() {
  Scene s;
  Entity box = s.create("box"), trigger = s.create("trigger"), player = s.create("player");
  s.reg.emplace<Collider>(box).size = vec3(2.0f);
  s.reg.get<Transform>(box).position = {0, 0, -10};
  Collider& tc = s.reg.emplace<Collider>(trigger);
  tc.solid = false;
  tc.size = vec3(4.0f);
  s.reg.emplace<Collider>(player);
  s.reg.get<Transform>(player).position = {1, 0, 0};
  s.updateTransforms();

  Scene::RayHit hit = s.raycast({0, 0, 0}, {0, 0, -5}, 100.0f, player); // direction need not be unit length
  CHECK(hit.entity == box && fabsf(hit.distance - 9.0f) < 1e-4f && hit.normal == vec3(0, 0, 1));
  CHECK(fabsf(hit.point.z + 9.0f) < 1e-4f);
  CHECK(s.raycast({0, 0, 0}, {0, 0, -1}, 5.0f, player).entity == kNoEntity);       // out of range
  CHECK(s.raycast({0, 0, 0}, {0, 1, 0}, 100.0f, player).entity == kNoEntity);      // triggers skipped...
  CHECK(s.raycast({0, 5, 0}, {0, -1, 0}, 100.0f, player, true).entity == trigger); // ...unless asked for

  std::vector<Entity> over = s.overlapping(player);
  CHECK(over.size() == 1 && over[0] == trigger);
  s.reg.get<Transform>(player).position = {10, 0, 0};
  s.updateTransforms();
  CHECK(s.overlapping(player).empty());
}

static void testScriptParams() {
  auto p = parseScriptParams(" target = scenes/house.scene.json ; speed=2.5;\nloop=true;bad name=1;entity=3;=x;novalue");
  CHECK(p.size() == 3);
  CHECK(p.size() >= 3 && p[0].first == "target" && p[0].second == "scenes/house.scene.json");
  CHECK(p.size() >= 3 && p[1].first == "speed" && p[1].second == "2.5" && p[2].first == "loop" && p[2].second == "true");
  CHECK(parseScriptParams("").empty());
}

static void testPrefabs() {
  std::string path = (std::filesystem::temp_directory_path() / "ace_test.prefab.json").string();
  Scene s;
  Entity tree = s.create("Tree"), top = s.create("Top", tree);
  s.reg.get<Transform>(tree).position = {5, 0, 5};
  s.reg.get<Transform>(top).position = {0, 2, 0};
  s.reg.emplace<MeshRenderer>(top).mesh = "builtin:sphere";
  CHECK(s.savePrefab(tree, path));

  Scene level;
  Entity a = level.instantiatePrefab(path), b = level.instantiatePrefab(path);
  CHECK(a != kNoEntity && b != kNoEntity && a != b);
  CHECK(level.reg.get<Name>(a).value == "Tree" && level.children(a).size() == 1 && level.children(b).size() == 1);
  Entity aTop = level.children(a)[0];
  CHECK(level.reg.get<MeshRenderer>(aTop).mesh == "builtin:sphere");
  level.reg.get<Transform>(a).position = {0, 0, 0}; // copies are independent
  level.updateTransforms();
  CHECK(glm::distance(vec3(level.reg.get<Transform>(aTop).world[3]), vec3(0, 2, 0)) < 1e-5f);
  CHECK(level.reg.get<Transform>(b).position == vec3(5, 0, 5));
  CHECK(level.instantiatePrefab(path + ".missing") == kNoEntity);
  std::filesystem::remove(path);

  // A damaged file with a parent cycle must load without linking the cycle.
  Scene bad;
  bad.fromJson(nlohmann::json::parse(R"({"entities":[{"name":"A","parent":1},{"name":"B","parent":0}]})"));
  Entity ea = bad.find("A"), eb = bad.find("B");
  CHECK(!(bad.reg.get<Transform>(ea).parent == eb && bad.reg.get<Transform>(eb).parent == ea));
  bad.destroy(ea); // would recurse forever with a cycle
  CHECK(!bad.reg.valid(ea));
}

static void testSpriteDirections() {
  SpriteRenderer s;
  s.directions = 4;
  s.columns = 4;
  mat4 world(1.0f); // at the origin, facing -Z
  CHECK(spriteDirection(s, world, {0, 2, -5}) == 0); // viewer in front
  CHECK(spriteDirection(s, world, {-5, 2, 0}) == 1); // viewer on the entity's left (it faces -Z, so its left is -X)
  CHECK(spriteDirection(s, world, {0, 2, 5}) == 2);  // behind
  CHECK(spriteDirection(s, world, {5, 2, 0}) == 3);  // right
  mat4 turned = glm::rotate(mat4(1.0f), glm::radians(90.0f), vec3(0, 1, 0)); // now faces -X
  CHECK(spriteDirection(s, turned, {-5, 0, 0}) == 0);
  s.directions = 8;
  CHECK(spriteDirection(s, world, {-5, 0, -5}) == 1); // front-left diagonal
  s.directions = 1;
  CHECK(spriteDirection(s, world, {0, 0, 5}) == 0);
  // Frame selection: row from the direction, column from the animation.
  s.directions = 4;
  s.frame = 1;
  vec4 r = spriteUvRect(s, 0.0, 2);
  CHECK(fabsf(r.x - 0.25f) < 1e-6f && fabsf(r.y - 0.5f) < 1e-6f && fabsf(r.z - 0.25f) < 1e-6f && fabsf(r.w - 0.25f) < 1e-6f);
}

static void testCopyPaste() {
  Scene s;
  Entity parent = s.create("Parent"), child = s.create("Child", parent), grandchild = s.create("Grandchild", child);
  s.reg.get<Transform>(parent).position = {10, 0, 0};
  s.reg.get<Transform>(parent).rotation = glm::angleAxis(glm::radians(90.0f), vec3(0, 1, 0));
  s.reg.get<Transform>(child).position = {0, 0, -2};
  s.reg.emplace<Collider>(grandchild).size = vec3(3.0f);
  s.updateTransforms();
  vec3 childWorld = vec3(s.reg.get<Transform>(child).world[3]);

  nlohmann::json copied = s.copyEntities({child, grandchild}); // grandchild is inside child: copied once
  std::vector<Entity> roots = s.pasteEntities(nlohmann::json::parse(copied.dump())); // through text, like the clipboard
  CHECK(roots.size() == 1);
  if (roots.size() == 1) {
    Entity pasted = roots[0];
    CHECK(!s.reg.valid(s.reg.get<Transform>(pasted).parent)); // pasted at the top level...
    CHECK(glm::distance(vec3(s.reg.get<Transform>(pasted).world[3]), childWorld) < 1e-4f); // ...where it was
    std::vector<Entity> kids = s.children(pasted);
    CHECK(kids.size() == 1 && kids.size() == 1 && s.reg.get<Collider>(kids[0]).size == vec3(3.0f));
  }
  CHECK(s.pasteEntities(nlohmann::json::parse("[1, 2]")).empty()); // foreign clipboard text is ignored
}

// Capture (glm::lookAt + 90 degree projection) and splatting (texel_splat.slang:
// p = origin + (F + R*cx + U*cy) * d) must agree on where every texel is.
static void testTexelSplatGeometry() {
  vec3 origin(4, 2, -4);
  mat4 proj = glm::perspectiveRH_ZO(glm::radians(90.0f), 1.0f, 0.05f, 400.0f);
  const vec3 points[] = {{9, 3, -3}, {-1, 1, -5}, {4.5f, 8, -3.5f}, {3, -6, -4}, {5, 2.5f, 3}, {3.5f, 1, -12}};
  for (int f = 0; f < 6; f++) {
    TexelSplatter::Face face = TexelSplatter::face(f);
    mat4 view = glm::lookAt(origin, origin + face.forward, face.up);
    vec3 p = points[f]; // each lies inside face f's frustum
    vec4 clip = proj * view * vec4(p, 1.0f);
    vec2 ndc = vec2(clip) / clip.w;
    CHECK(fabsf(ndc.x) < 1.0f && fabsf(ndc.y) < 1.0f);
    f32 d = -(view * vec4(p, 1.0f)).z; // what mesh.slang stores as depth
    vec3 rebuilt = origin + (face.forward + face.right * ndc.x + face.up * ndc.y) * d;
    CHECK(glm::distance(rebuilt, p) < 1e-3f);
  }
  CHECK(TexelSplatter::snapOrigin({1.9f, -0.1f, 6.1f}, 4.0f) == vec3(0, 0, 8));

  // Face selection: a camera at the origin looking down -Z (32 degree fov, 16:9)
  // sees only the -Z face of a probe at its own position...
  mat4 camProj = glm::perspectiveRH_ZO(glm::radians(32.0f), 16.0f / 9.0f, 0.1f, 200.0f);
  mat4 camView = glm::lookAt(vec3(0), vec3(0, 0, -1), vec3(0, 1, 0));
  CHECK(TexelSplatter::visibleFaces(vec3(0), glm::inverse(camProj * camView), vec3(0), 400.0f) == (1u << 5));
  // ...turned 45 degrees it straddles -Z and +X (both needed)...
  camView = glm::lookAt(vec3(0), vec3(1, 0, -1), vec3(0, 1, 0));
  u32 diagonal = TexelSplatter::visibleFaces(vec3(0), glm::inverse(camProj * camView), vec3(0), 400.0f);
  CHECK((diagonal & (1u << 5)) && (diagonal & (1u << 0)) && !(diagonal & (1u << 1)) && !(diagonal & (1u << 4)));
  // ...and a probe 3 units behind it still needs only faces covering the view.
  camView = glm::lookAt(vec3(0), vec3(0, 0, -1), vec3(0, 1, 0));
  u32 behind = TexelSplatter::visibleFaces(vec3(0, 0, 3), glm::inverse(camProj * camView), vec3(0), 400.0f);
  CHECK((behind & (1u << 5)) && !(behind & (1u << 4)));
  CHECK(TexelSplatter::snapOrigin({2.1f, 0, 0}, 4.0f) == vec3(4, 0, 0));
}

static void testRaycastMesh() {
  // A 2x2 floor quad at y = 0 (two triangles), raised to y = 3 and scaled x2 by its transform.
  Mesh m;
  for (vec3 p : {vec3(-1, 0, -1), vec3(1, 0, -1), vec3(1, 0, 1), vec3(-1, 0, 1)}) m.vertices.push_back({p, vec3(0, 1, 0), vec2(0), {255, 255, 255, 255}});
  m.indices = {0, 1, 2, 0, 2, 3};
  for (const Vertex& v : m.vertices) m.bounds.grow(v.position);
  mat4 world = glm::translate(mat4(1.0f), vec3(0, 3, 0)) * glm::scale(mat4(1.0f), vec3(2.0f));
  f32 t = 0;
  CHECK(raycastMesh(m, world, vec3(1.5f, 10, 1.5f), vec3(0, -1, 0), 100.0f, t) && fabsf(t - 7.0f) < 1e-4f); // inside after scaling
  CHECK(!raycastMesh(m, world, vec3(2.5f, 10, 0), vec3(0, -1, 0), 100.0f, t));  // outside the scaled quad
  CHECK(!raycastMesh(m, world, vec3(0, 10, 0), vec3(0, -1, 0), 5.0f, t));       // too short
  CHECK(raycastMesh(m, world, vec3(0, -5, 0), vec3(0, 1, 0), 100.0f, t) && fabsf(t - 8.0f) < 1e-4f); // from below
}

static void testUiText() {
  CHECK(ui::humanize("probeResolution") == "Probe resolution");
  CHECK(ui::humanize("MeshRenderer") == "Mesh renderer");
  CHECK(ui::humanize("ptSamples") == "PT samples");
  CHECK(ui::fuzzyScore("crcu", "Create cube") >= 0);
  CHECK(ui::fuzzyScore("cubex", "Create cube") < 0);       // every letter must appear, in order
  CHECK(ui::fuzzyScore("", "anything") >= 0);              // empty query matches everything
  CHECK(ui::fuzzyScore("sun", "Sun") > ui::fuzzyScore("sun", "Sound unit")); // word starts and runs win
  CHECK(ui::fuzzyScore("tree", "Tree") > ui::fuzzyScore("tree", "assets/tree_big.gltf")); // shorter wins ties
}

// Crowds: the box BVH, pack validation, and the property the whole design
// rests on: every pose a person can show (any frame, any crossfade weight)
// stays inside the precomputed pair bounds, so trees built on them never miss.
static void testCrowd() {
  using CR = CrowdRenderer;
  std::vector<Aabb> boxes;
  u32 seed = 7;
  auto rnd = [&] { seed = seed * 1664525u + 1013904223u; return (f32)(seed >> 8) / 16777216.0f; };
  for (int i = 0; i < 300; i++) {
    vec3 c(rnd() * 50, rnd() * 5, rnd() * 50);
    boxes.push_back(Aabb{c - vec3(0.3f, 0.9f, 0.3f), c + vec3(0.3f, 0.9f, 0.3f)});
  }
  std::vector<u32> order;
  std::vector<CR::Node> nodes;
  CR::buildBoxBvh(boxes, order, nodes, 1);
  std::vector<int> seen(boxes.size(), 0);
  std::vector<u32> stack{0};
  while (!stack.empty()) {
    CR::Node n = nodes[stack.back()];
    stack.pop_back();
    if (n.count == 0) {
      for (u32 c : {n.leftFirst, n.leftFirst + 1}) {
        CHECK(glm::all(glm::greaterThanEqual(nodes[c].min, n.min)) && glm::all(glm::lessThanEqual(nodes[c].max, n.max)));
        stack.push_back(c);
      }
      continue;
    }
    CHECK(n.count == 1);
    const Aabb& b = boxes[order[n.leftFirst]];
    CHECK(glm::all(glm::greaterThanEqual(b.min, n.min)) && glm::all(glm::lessThanEqual(b.max, n.max)));
    seen[order[n.leftFirst]]++;
  }
  CHECK(std::all_of(seen.begin(), seen.end(), [](int k) { return k == 1; }));
  CR::buildBoxBvh({}, order, nodes, 1);
  CHECK(nodes.size() == 1 && nodes[0].min.x > nodes[0].max.x); // empty root: every ray misses

  // Validation at the trust boundary.
  CR::Pack pack;
  std::string error;
  CHECK(!CR::parsePack(nlohmann::json::parse(R"({"bones": 3})"), pack, error) && !error.empty());
  nlohmann::json frame = nlohmann::json::array();
  for (u32 b = 0; b < CR::kBones; b++) frame.push_back({0, 0, 0, 1, 0, 0, 0});
  nlohmann::json bad = {{"bones", CR::kBones}, {"clips", {{{"name", "idle"}}}}, {"models", {{{"name", "m"}, {"parts", nlohmann::json::array()}}}},
                        {"poses", {{{frame, nlohmann::json::array({0, 0})}}}}}; // second frame has 2 bones
  CHECK(!CR::parsePack(bad, pack, error));

  // The shipped pack: random frames of two clips, random crossfade weights.
  std::vector<u8> bytes;
  if (!readFile(std::string(ACE_SOURCE_DIR) + "/projects/arena/assets/fans.crowd.json", bytes)) {
    printf("FAIL: cannot read the arena crowd pack\n");
    failures++;
    return;
  }
  CHECK(CR::parsePack(nlohmann::json::parse(bytes.begin(), bytes.end()), pack, error));
  const u32 clips = (u32)pack.clipNames.size();
  int outside = 0, tested = 0;
  for (const auto& model : pack.models) {
    for (int trial = 0; trial < 4000; trial++) {
      u32 a = (u32)(rnd() * clips) % clips, b = (u32)(rnd() * clips) % clips;
      const auto& fa = model.clips[a][(size_t)(rnd() * model.clips[a].size()) % model.clips[a].size()];
      const auto& fb = model.clips[b][(size_t)(rnd() * model.clips[b].size()) % model.clips[b].size()];
      f32 w = a == b ? 0.0f : rnd();
      for (const auto& part : model.parts) {
        quat qa = fa[part.bone].q, qb = glm::dot(qa, fb[part.bone].q) < 0.0f ? -fb[part.bone].q : fb[part.bone].q;
        quat q = glm::normalize(qa * (1.0f - w) + qb * w); // as crowd.slangh blends
        vec3 t = glm::mix(fa[part.bone].t, fb[part.bone].t, w);
        const Aabb& box = model.pairPartBounds[(a * clips + b) * CR::kBones + part.bone];
        const Aabb& person = model.pairBounds[a * clips + b];
        for (vec3 p : part.positions) {
          vec3 x = q * p + t;
          tested++;
          if (glm::any(glm::lessThan(x, box.min - 1e-4f)) || glm::any(glm::greaterThan(x, box.max + 1e-4f)) ||
              glm::any(glm::lessThan(x, person.min - 1e-4f)) || glm::any(glm::greaterThan(x, person.max + 1e-4f)) ||
              glm::any(glm::lessThan(x, model.bounds.min - 1e-4f)) || glm::any(glm::greaterThan(x, model.bounds.max + 1e-4f)))
            outside++;
        }
      }
    }
  }
  CHECK(tested > 1000000 && outside == 0);
  if (outside) printf("  %d of %d sampled pose vertices left their bounds\n", outside, tested);
}

int main() {
  testSceneRoundTrip();
  testSceneTolerantLoad();
  testHierarchy();
  testBvh();
  testMoveAndSlide();
  testQueries();
  testScriptParams();
  testPrefabs();
  testSpriteDirections();
  testCopyPaste();
  testTexelSplatGeometry();
  testRaycastMesh();
  testUiText();
  testCrowd();
  printf(failures ? "%d check(s) failed\n" : "all tests passed\n", failures);
  return failures ? 1 : 0;
}
