// Sound playback through SDL3 audio streams. WAV files only.
// One Audio per play session: destroying it stops every sound it started.
#pragma once
#include "core/base.h"
#include <atomic>
#include <mutex>
#include <unordered_map>

struct SDL_AudioStream;

namespace ace {

class Audio {
public:
  // Opens the playback device on a background thread so a slow or stuck OS audio
  // service can never freeze the game; sounds played before it is ready are skipped.
  explicit Audio(std::string projectDir);
  ~Audio();
  // Plays a project-relative .wav. Returns an id for stop(), or 0 on failure.
  u32 play(const std::string& path, f32 volume = 1.0f, bool loop = false);
  void stop(u32 id);
  void update(); // once per frame: refills looping sounds, frees finished ones
  bool enabled() const { return device_ != 0; }
  // Blocks until the device is open (or failed); returns enabled(). For tests and tools.
  bool waitForDevice(u32 timeoutMs);

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
  u32 device_ = 0; // valid once the opener thread has finished
  struct Opener {     // shared with the opener thread, which may outlive this object
    std::mutex mutex;
    bool done = false, abandoned = false;
    u32 device = 0;
    std::string error;
  };
  std::shared_ptr<Opener> opener_;
  bool checkOpener(); // main thread: picks up the opener's result once
  u32 nextId_ = 1;
  std::unordered_map<std::string, std::unique_ptr<Clip>> clips_;
  std::unordered_map<u32, Voice> voices_;
};

} // namespace ace
