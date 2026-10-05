#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

struct lua_State;

namespace ocemu {

// The sound card.
//
// OCEmu's own component/sound_card.lua is a stub: every method is `--STUB`, and
// the file cannot even load under us because it needs a real `elsa.SDL` audio
// device (elsa.SDL is false here). Real OpenComputers synthesises on the card
// and the guest only pushes one amplitude per speaker period, which is exactly
// what this implements: the guest calls openSpeaker() to get a handle, then
// pushSample(handle, sample) once per dt, and we generate that speaker's
// waveform into a ring buffer that the SDL audio callback drains.
//
// Speakers and the ring buffer are shared between the Lua thread and SDL's
// audio thread, so the buffer is guarded.
class OcSound {
 public:
  static constexpr int kSampleRate = 44100;
  static constexpr int kMaxSpeakers = 8;

  // Called by SDL on the audio thread. Fills `stream` with interleaved stereo.
  static void audioCallback(void* user, std::uint8_t* stream, int len);

  [[nodiscard]] static OcSound* active() { return active_; }

  static int luaComponent(lua_State* L);

  // Audio device lifecycle, owned by App.
  bool openAudio();
  void closeAudio();
  [[nodiscard]] bool audioOpen() const { return device_ != 0; }

  [[nodiscard]] std::uint32_t error() const { return error_; }
  void fail(const std::string& what);

 private:
  struct Speaker {
    bool open = false;
    double phase = 0.0;
    double freq = 440.0;
    double vol = 1.0;
    double pitch = 1.0;
    double speed = 1.0;
    std::int32_t dtUs = 0;   // period this speaker is pushed at
    std::int32_t delayUs = 0; // how long to wait before the first sample
    int wave = 0;             // 0 sine, 1 square, 2 triangle, 3 sawtooth
  };

  static int lGetSampleRate(lua_State* L);
  static int lOpenSpeaker(lua_State* L);
  static int lCloseSpeaker(lua_State* L);
  static int lSetFrequency(lua_State* L);
  static int lSetVolume(lua_State* L);
  static int lSetPitch(lua_State* L);
  static int lSetSpeed(lua_State* L);
  static int lSetWave(lua_State* L);
  static int lPushSample(lua_State* L);
  static int lGetError(lua_State* L);
  static int lIsOn(lua_State* L);

  [[nodiscard]] int addSpeaker();
  [[nodiscard]] Speaker* speaker(int handle);
  // Renders `frames` of a speaker's waveform into the ring buffer.
  void render(Speaker* sp, double amplitude);

  std::array<Speaker, kMaxSpeakers> speakers_{};
  std::deque<std::int16_t> ring_;      // interleaved stereo, guarded by mutex_
  std::mutex mutex_;
  std::uint32_t device_ = 0;
  std::uint32_t error_ = 0;
  std::size_t overrun_ = 0;

  static OcSound* active_;
};

}  // namespace ocemu
