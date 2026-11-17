#include "audio/audio.h"
#include <SDL3/SDL.h>
#include <thread>

namespace ace {

constexpr size_t kMaxVoices = 64; // beyond this, new sounds are dropped (with a warning)

Audio::Audio(std::string projectDir) : root_(std::move(projectDir)), opener_(std::make_shared<Opener>()) {
  if (!root_.empty() && root_.back() != '/') root_ += '/';
  if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    ACE_WARN("audio disabled: %s", SDL_GetError());
    opener_->done = true;
    return;
  }
  std::thread([shared = opener_] {
    u32 device = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    std::lock_guard<std::mutex> lock(shared->mutex);
    if (shared->abandoned) { // the Audio object is gone: clean up here
      if (device) SDL_CloseAudioDevice(device);
      return;
    }
    shared->device = device;
    if (!device) shared->error = SDL_GetError();
    shared->done = true;
  }).detach();
}

Audio::~Audio() {
  for (auto& [id, v] : voices_) SDL_DestroyAudioStream(v.stream);
  std::lock_guard<std::mutex> lock(opener_->mutex);
  if (!opener_->done) opener_->abandoned = true; // the thread closes the device if it ever opens
  else if (opener_->device) SDL_CloseAudioDevice(opener_->device);
}

bool Audio::checkOpener() {
  if (device_) return true;
  std::lock_guard<std::mutex> lock(opener_->mutex);
  if (!opener_->done) return false;
  if (!opener_->error.empty()) {
    ACE_WARN("audio disabled: no playback device (%s)", opener_->error.c_str()); // logged on the main thread
    opener_->error.clear();
  }
  device_ = opener_->device;
  return device_ != 0;
}

bool Audio::waitForDevice(u32 timeoutMs) {
  for (u32 waited = 0; !checkOpener(); waited += 5) {
    {
      std::lock_guard<std::mutex> lock(opener_->mutex);
      if (opener_->done) return checkOpener();
    }
    if (waited >= timeoutMs) return false;
    SDL_Delay(5);
  }
  return true;
}

const Audio::Clip* Audio::load(const std::string& path) {
  auto it = clips_.find(path);
  if (it != clips_.end()) return it->second.get();
  auto clip = std::make_unique<Clip>();
  SDL_AudioSpec spec;
  u8* buffer = nullptr;
  u32 length = 0;
  if (!SDL_LoadWAV((root_ + path).c_str(), &spec, &buffer, &length)) {
    ACE_WARN("sound '%s' could not be loaded: %s", path.c_str(), SDL_GetError());
    clips_[path] = nullptr; // do not retry (and warn) every frame
    return nullptr;
  }
  clip->data.assign(buffer, buffer + length);
  SDL_free(buffer);
  clip->format = (int)spec.format;
  clip->channels = spec.channels;
  clip->freq = spec.freq;
  return (clips_[path] = std::move(clip)).get();
}

u32 Audio::play(const std::string& path, f32 volume, bool loop) {
  if (!checkOpener()) return 0; // still opening, or no device
  const Clip* clip = load(path);
  if (!clip) return 0;
  if (voices_.size() >= kMaxVoices) {
    ACE_WARN("too many sounds playing (%zu); '%s' dropped", voices_.size(), path.c_str());
    return 0;
  }
  SDL_AudioSpec src{(SDL_AudioFormat)clip->format, clip->channels, clip->freq};
  SDL_AudioSpec dst;
  if (!SDL_GetAudioDeviceFormat(device_, &dst, nullptr)) dst = src;
  SDL_AudioStream* stream = SDL_CreateAudioStream(&src, &dst);
  if (!stream) {
    ACE_WARN("sound '%s': %s", path.c_str(), SDL_GetError());
    return 0;
  }
  SDL_SetAudioStreamGain(stream, volume < 0.0f ? 0.0f : (volume > 4.0f ? 4.0f : volume));
  SDL_PutAudioStreamData(stream, clip->data.data(), (int)clip->data.size());
  if (!loop) SDL_FlushAudioStream(stream);
  if (!SDL_BindAudioStream(device_, stream)) {
    ACE_WARN("sound '%s': %s", path.c_str(), SDL_GetError());
    SDL_DestroyAudioStream(stream);
    return 0;
  }
  u32 id = nextId_++;
  voices_[id] = Voice{stream, clip, loop};
  return id;
}

void Audio::stop(u32 id) {
  auto it = voices_.find(id);
  if (it == voices_.end()) return;
  SDL_DestroyAudioStream(it->second.stream); // also unbinds it
  voices_.erase(it);
}

void Audio::update() {
  checkOpener();
  for (auto it = voices_.begin(); it != voices_.end();) {
    Voice& v = it->second;
    int queued = SDL_GetAudioStreamQueued(v.stream);
    if (v.loop && queued < (int)v.clip->data.size()) {
      SDL_PutAudioStreamData(v.stream, v.clip->data.data(), (int)v.clip->data.size()); // keep one copy ahead
    } else if (!v.loop && queued == 0 && SDL_GetAudioStreamAvailable(v.stream) == 0) {
      SDL_DestroyAudioStream(v.stream);
      it = voices_.erase(it);
      continue;
    }
    ++it;
  }
}

} // namespace ace
