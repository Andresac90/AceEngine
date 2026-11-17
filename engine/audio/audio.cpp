#include "audio/audio.h"
#include <SDL3/SDL.h>

namespace ace {

constexpr size_t kMaxVoices = 64; // beyond this, new sounds are dropped (with a warning)

Audio::Audio(std::string projectDir) : root_(std::move(projectDir)) {
  if (!root_.empty() && root_.back() != '/') root_ += '/';
  if (!SDL_WasInit(SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    ACE_WARN("audio disabled: %s", SDL_GetError());
    return;
  }
  device_ = SDL_OpenAudioDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
  if (!device_) ACE_WARN("audio disabled: no playback device (%s)", SDL_GetError());
}

Audio::~Audio() {
  for (auto& [id, v] : voices_) SDL_DestroyAudioStream(v.stream);
  if (device_) SDL_CloseAudioDevice(device_);
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
  if (!device_) return 0;
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
