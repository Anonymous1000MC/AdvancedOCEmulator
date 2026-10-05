#include "ocemu/MemoryAllocator.hpp"

#include <cstddef>
#include <cstdlib>

namespace ocemu {
namespace {

// Every block we hand out is prefixed with its true reserved size. Lua only ever
// tells us the size it *asked* for (as `osize`), which is not what we malloc'd,
// so the header is the only trustworthy source for accounting.
struct alignas(std::max_align_t) BlockHeader {
  std::size_t size;
};

constexpr std::size_t kHeaderSize = sizeof(BlockHeader);

inline char* rawPtr(void* payload) { return static_cast<char*>(payload) - kHeaderSize; }

inline void* payloadPtr(void* raw) { return static_cast<char*>(raw) + kHeaderSize; }

inline std::size_t* sizeSlot(void* raw) { return reinterpret_cast<std::size_t*>(raw); }

inline std::size_t blockSizeOf(void* payload) {
  return *sizeSlot(rawPtr(payload));
}

inline void setBlockSize(void* raw, std::size_t size) { *sizeSlot(raw) = size; }

}  // namespace

double MemoryAllocator::usedFraction() const {
  if (infinite() || limit_ == 0) return 0.0;
  return static_cast<double>(used_) / static_cast<double>(limit_);
}

std::size_t MemoryAllocator::blockSizeFor(std::size_t nsize) {
  // Reserve power-of-two blocks so a modest string append usually fits in the
  // slack of the existing block and costs no allocation at all.
  std::size_t block = kMinBlock;
  const std::size_t need = nsize + kHeaderSize;

  while (block < need) {
    if (block > (static_cast<std::size_t>(-1) / 2)) return need;  // overflow guard
    block <<= 1;
  }
  return block;
}

bool MemoryAllocator::charge(std::size_t extra) {
  if (extra == 0) return true;

  // Even with no cap we must keep accounting: the Component Manager reports
  // live usage, and peak tracking drives the diagnostics view.
  if (!infinite()) {
    // Written this way round so `used_ + extra` cannot wrap.
    if (used_ >= limit_ || extra > limit_ - used_) {
      refused_ += extra;
      lastRefused_ = extra;
      ++refusedCount_;
      oom_ = true;
      return false;
    }
  }

  used_ += extra;
  if (used_ > peak_) peak_ = used_;
  return true;
}

void MemoryAllocator::uncharge(std::size_t amount) {
  if (amount >= used_) {
    used_ = 0;
  } else {
    used_ -= amount;
  }
}

void* MemoryAllocator::alloc(void* p, std::size_t osize, std::size_t nsize) {
  (void)osize;  // our own header is authoritative

  if (nsize == 0) {
    if (p != nullptr) {
      const std::size_t sz = blockSizeOf(p);
      uncharge(sz);
      std::free(rawPtr(p));
    }
    return nullptr;
  }

  const std::size_t need = blockSizeFor(nsize);

  if (p == nullptr) {
    if (!charge(need)) return nullptr;
    void* raw = std::malloc(need);
    if (raw == nullptr) {
      uncharge(need);
      ++refusedCount_;
      oom_ = true;
      return nullptr;
    }
    setBlockSize(raw, need);
    return payloadPtr(raw);
  }

  const std::size_t have = blockSizeOf(p);

  if (need <= have) {
    if (need == have) return p;  // fits the existing slack: no work to do

    // Shrinking. realloc on a shrink is not allowed to fail, but if it somehow
    // does we simply keep the larger block and leave accounting untouched.
    void* raw = std::realloc(rawPtr(p), need);
    if (raw == nullptr) return p;
    uncharge(have - need);
    setBlockSize(raw, need);
    return payloadPtr(raw);
  }

  const std::size_t extra = need - have;
  if (!charge(extra)) return nullptr;  // refusal leaves the old block untouched

  void* raw = std::realloc(rawPtr(p), need);
  if (raw == nullptr) {
    uncharge(extra);
    ++refusedCount_;
    oom_ = true;
    return nullptr;
  }
  setBlockSize(raw, need);
  return payloadPtr(raw);
}

void* MemoryAllocator::luaThunk(void* ud, void* ptr, std::size_t osize, std::size_t nsize) {
  auto* self = static_cast<MemoryAllocator*>(ud);
  if (self == nullptr) return nullptr;
  return self->alloc(ptr, osize, nsize);
}

}  // namespace ocemu