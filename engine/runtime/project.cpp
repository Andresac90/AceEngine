#include "runtime/project.h"
#include <nlohmann/json.hpp>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image_write.h>

namespace ace {

using json = nlohmann::json;

bool Project::load(const std::string& directory) {
  dir = directory;
  if (!dir.empty() && dir.back() != '/') dir += '/';
  std::string text;
  if (!readTextFile(dir + "project.json", text)) {
    ACE_ERROR("no project.json in %s", dir.c_str());
    return false;
  }
  json j = json::parse(text, nullptr, false);
  if (!j.is_object()) {
    ACE_ERROR("%sproject.json is not valid JSON", dir.c_str());
    return false;
  }
  name = j.value("name", name);
  startScene = j.value("startScene", startScene);
  width = j.value("width", width);
  height = j.value("height", height);
  return true;
}

bool Project::save() const {
  json j = {{"name", name}, {"startScene", startScene}, {"width", width}, {"height", height}};
  return writeTextFile(dir + "project.json", j.dump(2));
}

bool savePng(const std::string& path, u32 width, u32 height, const u8* rgba) {
  return stbi_write_png(path.c_str(), (int)width, (int)height, 4, rgba, (int)width * 4) != 0;
}

} // namespace ace
