// ---------------------------------------------------------------------------
//  Custom Lua allocator with a byte-accurate budget.
//
//  Installed through lua_setallocf(). Every block handed to Lua carries a small
//  header recording its true (rounded) size, which lets the allocator:
//
//    * track live byte usage exactly, independent of what Lua believes it asked
//      for (Lua passes the *requested* size as `osize`, which is not the size we
//      actually malloc'd);
//    * grow or shrink a block in place whenever it still fits, so ordinary
//      string appends do not churn the heap;
//    * reject an allocation that would push usage past the configured cap and
//      raise the out-of-memory flag instead of silently letting the process
//      balloon.
//
//  When infinite memory is active the cap checks are bypassed entirely.
//
//  Threading: this allocator is single-threaded, exactly like the Lua state it
//  serves. All access happens on the thread that owns the lua_State.
// ---------------------------------------------------------------------------
#pragma once

#include <cstddef>
#include <cstdint>

namespace ocemu {

class MemoryAllocator {
 public:
  // Sentinel meaning "no cap at all".
  static constexpr std::size_t kInfiniteLimit = 0;
  static constexpr std::size_t kMinBlock = 32;

  // --- configuration -------------------------------------------------------
  // `bytes == kInfiniteLimit` (0) disables capping.
  void setLimitBytes(std::size_t bytes) { limit_ = bytes; }
  void setInfinite(bool infinite) { limit_ = infinite ? kInfiniteLimit : limit_; }
  [[nodiscard]] bool infinite() const { return limit_ == kInfiniteLimit; }
  [[nodiscard]] std::size_t limitBytes() const { return limit_; }

  static std::size_t bytesFromKb(int kb) {
    return kb <= 0 ? kInfiniteLimit : static_cast<std::size_t>(kb) * 1024u;
  }

  // --- accounting ----------------------------------------------------------
  [[nodiscard]] std::size_t usedBytes() const { return used_; }
  [[nodiscard]] std::size_t peakBytes() const { return peak_; }
  [[nodiscard]] std::size_t refusedBytes() const { return refused_; }
  [[nodiscard]] std::uint64_t refusedCount() const { return refusedCount_; }
  [[nodiscard]] double usedFraction() const;

  // --- OOM signalling ------------------------------------------------------
  [[nodiscard]] bool outOfMemory() const { return oom_; }
  [[nodiscard]] std::size_t lastRefusedBytes() const { return lastRefused_; }
  void clearOutOfMemory() {
    oom_ = false;
    lastRefused_ = 0;
  }

  void reset() {
    used_ = 0;
    peak_ = 0;
    refused_ = 0;
    refusedCount_ = 0;
    oom_ = false;
    lastRefused_ = 0;
  }

  // --- the allocator itself ------------------------------------------------
  // Mirrors lua_Alloc: `p` is the existing block (or null), `osize` its old
  // *requested* size (unused, we read our own header), `nsize` the new size.
  // Returns null when the request is denied or the host allocator fails.
  void* alloc(void* p, std::size_t osize, std::size_t nsize);

  // Same signature as lua_Alloc so it can be handed to lua_setallocf directly.
  static void* luaThunk(void* ud, void* ptr, std::size_t osize, std::size_t nsize);

  // Round an allocation up to the block size we actually reserve.
  static std::size_t blockSizeFor(std::size_t nsize);

 private:
  [[nodiscard]] bool charge(std::size_t extra);
  void uncharge(std::size_t amount);

  std::size_t limit_ = kInfiniteLimit;
  std::size_t used_ = 0;
  std::size_t peak_ = 0;
  std::size_t refused_ = 0;
  std::size_t lastRefused_ = 0;
  std::uint64_t refusedCount_ = 0;
  bool oom_ = false;
};

}  // namespace ocemu