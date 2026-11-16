// A project is a folder with a project.json, scenes, scripts and assets.
#pragma once
#include "core/base.h"

namespace ace {

struct Project {
  std::string dir; // ends with '/'
  std::string name = "Untitled";
  std::string startScene = "scenes/main.scene.json";
  int width = 1280, height = 720;

  bool load(const std::string& directory);
  bool save() const;
  std::string path(const std::string& relative) const { return dir + relative; }
};

bool savePng(const std::string& path, u32 width, u32 height, const u8* rgba);

} // namespace ace
