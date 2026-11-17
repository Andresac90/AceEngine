// Sound playback through SDL3 audio streams. WAV files only.
// One Audio per play session: destroying it stops every sound it started.
#pragma once
#include "core/base.h"
#include <unordered_map>

struct SDL_AudioStream;

namespace ace {

class Audio {
public:
  explicit Audio(std::string projectDir); // logs and stays silent if no audio device is available
  ~Audio();
  // Plays a project-relative .wav. Returns an id for stop(), or 0 on failure.
  u32 play(const std::string& path, f32 volume = 1.0f, bool loop = false);
  void stop(u32 id);
  void update(); // once per frame: refills looping sounds, frees finished ones
  bool enabled() const { return device_ != 0; }

private:
  struct Clip {
    std::vector<u8> data;
    int format = 0, channels = 0, freq = 0;
  };
  struct Voice {
    SDL_AudioStream* stream = nullptr;
    const Clip* clip = nullptr;
    bool loop = false;
  };
  const Clip* load(const std::string& path);
  std::string root_;
  u32 device_ = 0;
  u32 nextId_ = 1;
  std::unordered_map<std::string, std::unique_ptr<Clip>> clips_;
  std::unordered_map<u32, Voice> voices_;
};

} // namespace ace
