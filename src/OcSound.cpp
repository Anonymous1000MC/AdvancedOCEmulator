#include "ocemu/OcSound.hpp"

#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <cstdio>

extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

namespace ocemu {
namespace {

// Registry key holding the one live sound card, so the static callbacks can
// reach it. Mirrors OcScreen's kRegistryTag.
char kInstanceTag = 0;

// Mirrors OcScreen's helpers so the component reads the same way.
OcSound* self(lua_State* L) {
  void* p = nullptr;
  lua_rawgetp(L, LUA_REGISTRYINDEX, &kInstanceTag);
  p = lua_touserdata(L, -1);
  lua_pop(L, 1);
  return static_cast<OcSound*>(p);
}

double argNum(lua_State* L, int idx, double dflt) {
  return lua_isnumber(L, idx) ? static_cast<double>(lua_tonumber(L, idx)) : dflt;
}

int argInt(lua_State* L, int idx, int dflt) {
  return lua_isnumber(L, idx) ? static_cast<int>(lua_tointeger(L, idx)) : dflt;
}

double waveform(int wave, double phase) {
  // phase is in turns: 0..1.
  switch (wave) {
    case 1:  // square
      return phase < 0.5 ? 1.0 : -1.0;
    case 2:  // triangle
      return phase < 0.5 ? (4.0 * phase - 1.0) : (3.0 - 4.0 * phase);
    case 3:  // sawtooth
      return 2.0 * phase - 1.0;
    default:  // sine
      return std::sin(phase * 6.283185307179586);
  }
}

}  // namespace

OcSound* OcSound::active_ = nullptr;

void OcSound::fail(const std::string& what) {
  error_ = 1;
  std::fprintf(stderr, "sound: %s\n", what.c_str());
}

int OcSound::addSpeaker() {
  for (std::size_t i = 0; i < speakers_.size(); ++i) {
    if (!speakers_[i].open) {
      speakers_[i] = Speaker{};
      speakers_[i].open = true;
      return static_cast<int>(i + 1);
    }
  }
  // All handles in use: reusing the first slot mirrors the card's own
  // behaviour of having a fixed number of speakers.
  speakers_[0] = Speaker{};
  speakers_[0].open = true;
  return 1;
}

OcSound::Speaker* OcSound::speaker(int handle) {
  if (handle < 1 || handle > kMaxSpeakers) return nullptr;
  Speaker& s = speakers_[static_cast<std::size_t>(handle - 1)];
  return s.open ? &s : nullptr;
}

void OcSound::render(Speaker* sp, double amplitude) {
  // dtUs is the period the guest pushes at; the card generates that many frames
  // now so the guest only has to supply one number per period.
  int frames = static_cast<int>(static_cast<double>(sp->dtUs) * kSampleRate / 1e6);
  if (frames < 1) frames = 1;
  if (frames > kSampleRate / 4) frames = kSampleRate / 4;  // clamp a silly dt

  if (sp->delayUs > 0) {
    const int silent = static_cast<int>(static_cast<double>(sp->delayUs) * kSampleRate / 1e6);
    if (silent > 0) {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (int i = 0; i < silent; ++i) {
        ring_.push_back(0);
        ring_.push_back(0);
      }
    }
  }

  const double gain = amplitude * sp->vol;
  std::lock_guard<std::mutex> lock(mutex_);
  for (int i = 0; i < frames; ++i) {
    const double v = waveform(sp->wave, sp->phase) * gain;
    sp->phase += sp->freq * sp->pitch / kSampleRate;
    sp->phase -= std::floor(sp->phase);
    // Additive mixing: several speakers share the output, like real hardware.
    const auto s = static_cast<std::int16_t>(
        std::clamp(v, -1.0, 1.0) * 32767.0);
    ring_.push_back(s);
    ring_.push_back(s);  // mono speaker, duplicated to both channels
  }
  // If the guest stops pushing (a crashed program, a paused machine) the ring
  // would grow forever. Drop the oldest audio instead.
  constexpr std::size_t kMaxRing = static_cast<std::size_t>(kSampleRate) * 2;
  while (ring_.size() > kMaxRing) {
    ring_.pop_front();
    ring_.pop_front();
    ++overrun_;
  }
}

void OcSound::audioCallback(void* user, std::uint8_t* stream, int len) {
  auto* self = static_cast<OcSound*>(user);
  auto* out = reinterpret_cast<std::int16_t*>(stream);
  const int frames = len / static_cast<int>(sizeof(std::int16_t) * 2);
  const std::lock_guard<std::mutex> lock(self->mutex_);
  for (int i = 0; i < frames; ++i) {
    if (self->ring_.size() < 2) {
      out[i * 2] = 0;      // underrun: silence, never repeat stale audio
      out[i * 2 + 1] = 0;
      continue;
    }
    out[i * 2] = self->ring_.front();
    self->ring_.pop_front();
    out[i * 2 + 1] = self->ring_.front();
    self->ring_.pop_front();
  }
}

bool OcSound::openAudio() {
  if (device_ != 0) return true;
  active_ = this;
  SDL_AudioSpec want{};
  SDL_AudioSpec have{};
  want.freq = kSampleRate;
  want.format = AUDIO_S16SYS;
  want.channels = 2;
  want.samples = 1024;
  want.callback = &OcSound::audioCallback;
  want.userdata = this;
  // No ALLOW_FREQUENCY_CHANGE: the guest is told 44100 and that is what it must
  // get, otherwise every sound would play at the wrong pitch.
  device_ = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
  if (device_ == 0) {
    fail("could not open an audio device: " + std::string(SDL_GetError()));
    return false;
  }
  SDL_PauseAudioDevice(device_, 0);
  return true;
}

void OcSound::closeAudio() {
  if (device_ != 0) {
    SDL_CloseAudioDevice(device_);
    device_ = 0;
  }
  if (active_ == this) active_ = nullptr;
}

int OcSound::lGetSampleRate(lua_State* L) {
  if (self(L) == nullptr) return 0;
  lua_pushinteger(L, kSampleRate);
  return 1;
}

int OcSound::lOpenSpeaker(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  const int handle = s->addSpeaker();
  Speaker* sp = s->speaker(handle);
  if (sp == nullptr) return 0;
  // openSpeaker(speaker, dt, freq, volume, delay, mode)
  sp->dtUs = argInt(L, 2, 100000 / 60);
  sp->freq = argNum(L, 3, 440.0);
  sp->vol = argNum(L, 4, 1.0);
  sp->delayUs = argInt(L, 5, 0);
  sp->wave = argInt(L, 6, 0);
  lua_pushinteger(L, handle);
  return 1;
}

int OcSound::lCloseSpeaker(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  if (Speaker* sp = s->speaker(argInt(L, 1, 0))) sp->open = false;
  return 0;
}

int OcSound::lSetFrequency(lua_State* L) {
  auto* s = self(L);
  if (s != nullptr) {
    if (Speaker* sp = s->speaker(argInt(L, 1, 0))) sp->freq = argNum(L, 2, sp->freq);
  }
  return 0;
}

int OcSound::lSetVolume(lua_State* L) {
  auto* s = self(L);
  if (s != nullptr) {
    if (Speaker* sp = s->speaker(argInt(L, 1, 0))) sp->vol = argNum(L, 2, sp->vol);
  }
  return 0;
}

int OcSound::lSetPitch(lua_State* L) {
  auto* s = self(L);
  if (s != nullptr) {
    if (Speaker* sp = s->speaker(argInt(L, 1, 0))) sp->pitch = argNum(L, 2, sp->pitch);
  }
  return 0;
}

int OcSound::lSetSpeed(lua_State* L) {
  auto* s = self(L);
  if (s != nullptr) {
    if (Speaker* sp = s->speaker(argInt(L, 1, 0))) sp->speed = argNum(L, 2, sp->speed);
  }
  return 0;
}

int OcSound::lSetWave(lua_State* L) {
  auto* s = self(L);
  if (s != nullptr) {
    if (Speaker* sp = s->speaker(argInt(L, 1, 0))) sp->wave = argInt(L, 2, 0);
  }
  return 0;
}

int OcSound::lPushSample(lua_State* L) {
  auto* s = self(L);
  if (s == nullptr) return 0;
  Speaker* sp = s->speaker(argInt(L, 1, 0));
  if (sp == nullptr) return 0;
  // OC passes either a bare number or {left=, right=}; use the louder channel so
  // a stereo push is still audible on a mono card.
  double amp = 0.0;
  if (lua_istable(L, 2)) {
    lua_getfield(L, 2, "left");
    const double l = argNum(L, -1, 0.0);
    lua_pop(L, 1);
    lua_getfield(L, 2, "right");
    const double r = argNum(L, -1, 0.0);
    lua_pop(L, 1);
    amp = std::max(std::abs(l), std::abs(r));
  } else {
    amp = std::abs(argNum(L, 2, 0.0));
  }
  s->render(sp, amp);
  return 0;
}

int OcSound::lGetError(lua_State* L) {
  if (auto* s = self(L)) lua_pushinteger(L, static_cast<lua_Integer>(s->error_));
  return 1;
}

int OcSound::lIsOn(lua_State* L) {
  if (auto* s = self(L)) lua_pushboolean(L, s->audioOpen());
  return 1;
}

int OcSound::luaComponent(lua_State* L) {
  // One sound card per machine, like one screen.
  static OcSound card;
  active_ = &card;
  lua_pushlightuserdata(L, &card);
  lua_rawsetp(L, LUA_REGISTRYINDEX, &kInstanceTag);

  lua_newtable(L);  // component table

  // Must advertise the REAL OpenComputers type. apis/component.lua does
  // `proxy.type = proxy.type or info[1]`, so without this the internal name
  // ("sound_native") leaks into component.list() and OpenOS never sees a sound
  // card at all.
  lua_pushstring(L, "sound");
  lua_setfield(L, -2, "type");

  // Everything except the constructor is exposed directly on the component, the
  // way a screen/keyboard component is.
  const luaL_Reg methods[] = {
      {"getSampleRate", &OcSound::lGetSampleRate},
      {"openSpeaker", &OcSound::lOpenSpeaker},
      {"closeSpeaker", &OcSound::lCloseSpeaker},
      {"setFrequency", &OcSound::lSetFrequency},
      {"setVolume", &OcSound::lSetVolume},
      {"setPitch", &OcSound::lSetPitch},
      {"setSpeed", &OcSound::lSetSpeed},
      {"setWave", &OcSound::lSetWave},
      {"pushSample", &OcSound::lPushSample},
      {"getError", &OcSound::lGetError},
      {"isOn", &OcSound::lIsOn},
  };
  for (const auto& m : methods) {
    lua_pushcfunction(L, m.func);
    lua_setfield(L, -2, m.name);
  }

  // api/component.lua expects (proxy, cec, mai, di) and indexes mai[k] for
  // every function on the proxy, so all four have to be real tables.
  //
  // mai: method metadata; apis/component.lua fills in the defaults.
  lua_newtable(L);

  // di: device info.
  lua_newtable(L);
  lua_pushstring(L, "sound");
  lua_setfield(L, -2, "class");
  lua_pushstring(L, "Audio interface");
  lua_setfield(L, -2, "description");
  lua_pushstring(L, "Yanaki Sound Systems");
  lua_setfield(L, -2, "vendor");
  lua_pushstring(L, "MinoSound 244-X");
  lua_setfield(L, -2, "product");

  // cec aliases the proxy, as OcScreen does.
  lua_pushvalue(L, -3);
  lua_insert(L, -3);  // -> [proxy][proxy][mai][di]
  return 4;
}

}  // namespace ocemu
