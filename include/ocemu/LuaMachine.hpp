// ---------------------------------------------------------------------------
//  The Lua 5.2 virtual machine that plays the role of the OpenComputers CPU.
//
//  Responsibilities:
//    * own the lua_State and the custom allocator's budget
//    * survive out-of-memory conditions without corrupting the C++ side
//    * expose the host components (screen, ram, internet, host services) to Lua
//
//  PANIC SAFETY
//  ------------
//  When our allocator refuses a request, Lua cannot report an ordinary error --
//  it panics, and the panic handler longjmps out of the middle of our C++ call
//  frames. To keep that from skipping destructors we funnel every entry into
//  Lua through reboot()/update() and place a setjmp target in the *outermost*
//  frame. The inner helpers that actually run Lua code (bootCore) therefore hold
//  no non-trivial C++ locals. C++ exceptions must never cross a Lua C frame, so
//  anything that can allocate host-side is wrapped in try/catch.
// ---------------------------------------------------------------------------
#pragma once

#include <csetjmp>
#include <functional>
#include <string>
#include <vector>

#include "ocemu/InternetCard.hpp"
#include "ocemu/MemoryAllocator.hpp"
#include "ocemu/ScreenBuffer.hpp"
#include "ocemu/Tiers.hpp"

struct lua_State;

namespace ocemu {

enum class MachineState {
  Stopped,     // no state yet
  Running,     // healthy
  OutOfMemory, // allocator refused a request; state discarded
  Faulted,     // Lua raised an unrecoverable error / panic
};

// Flat snapshot of everything the guest might want to know about its machine.
// Deliberately free of references to the config classes.
struct HostInfo {
  int gpuTier = 2;
  int screenTier = 2;
  int ramLimitKb = 512;  // -1 == infinite
  bool internetEnabled = false;
  int cols = 80;
  int rows = 25;
  int depth = 4;
};

struct LuaContext {
  ScreenBuffer* screen = nullptr;
  MemoryAllocator* allocator = nullptr;
  InternetCard* internet = nullptr;
  std::vector<std::string>* alerts = nullptr;
  std::vector<std::string>* logLines = nullptr;
  bool* rebootRequested = nullptr;
  bool* quitRequested = nullptr;
};

class LuaMachine {
 public:
  LuaMachine() = default;
  ~LuaMachine();
  LuaMachine(const LuaMachine&) = delete;
  LuaMachine& operator=(const LuaMachine&) = delete;

  void attach(LuaContext ctx) { ctx_ = ctx; }
  void setHostInfo(const HostInfo& info) { host_ = info; }
  [[nodiscard]] const HostInfo& hostInfo() const { return host_; }

  // Soft reboot: read the ROM, tear down any existing state, then run the ROM
  // against the allocator's current budget. Returns false if the state could
  // not be brought up (the reason lands in `error`).
  bool reboot(const std::string& romPath, std::string* error = nullptr);

  void shutdown();

  // One host tick: pumps the network and invokes the guest's host.onTick hook
  // if it defined one. Safe to call when no state exists.
  void update(double dtSeconds);

  // Compile and run a chunk in the current state, after the ROM has finished
  // booting. This is how the host injects things the guest needs but that can
  // only be built once a live state exists -- notably the elsa table that
  // OCEmu's main.lua expects to already be in place.
  // Returns false and fills `error` on a compile or runtime failure.
  bool runChunk(const std::string& source, const char* chunkName,
                std::string* error = nullptr);

  // As runChunkIn, but keeps the chunk's first string result in `out`. Used by
  // diagnostics to read structured answers back out of the guest sandbox.
  bool runChunkString(const std::string& source, const char* chunkName,
                      const char* envGlobal, std::string* out,
                      std::string* error = nullptr);

  // As runChunk, but the chunk is compiled with the given global table as its
  // _ENV. This is how the host reaches into a sandboxed guest environment (the
  // machine's `component`, `computer`, ... tables live there, not in _G).
  bool runChunkIn(const std::string& source, const char* chunkName,
                  const char* envGlobal, std::string* error = nullptr);

  // Runs `fn` inside the panic guard.
  //
  // Anything that calls into Lua from the host -- the machine tick, an OCEmu
  // handler dispatch -- must go through here. Without it a guest panic would
  // reach lua_atpanic with no active setjmp target and abort the process,
  // instead of being recovered from.
  bool guarded(const std::function<void()>& fn);

  [[nodiscard]] MachineState state() const { return state_; }
  [[nodiscard]] bool alive() const { return state_ == MachineState::Running && L_ != nullptr; }
  [[nodiscard]] lua_State* L() const { return L_; }
  // The allocator actually backing the Lua state. The host owns it and passes
  // it in through LuaContext, so that a RAM limit set on the host side really
  // does bound the VM. ownedAllocator_ is only a fallback for embedders that do
  // not supply one.
  [[nodiscard]] MemoryAllocator& alloc() { return ctx_.allocator ? *ctx_.allocator : ownedAllocator_; }
  [[nodiscard]] const MemoryAllocator& alloc() const {
    return ctx_.allocator ? *ctx_.allocator : ownedAllocator_;
  }
  [[nodiscard]] MemoryAllocator& allocator() { return alloc(); }
  [[nodiscard]] const MemoryAllocator& allocator() const { return alloc(); }
  [[nodiscard]] const std::string& lastError() const { return lastError_; }
  void clearError() { lastError_.clear(); }

  // Push an alert that the host UI will display.
  void pushAlert(const char* msg);

 private:

  // --- panic plumbing -----------------------------------------------------
  static int luaPanicTrampoline(lua_State* L);
  [[nodiscard]] std::jmp_buf& panicJump() { return panic_; }
  void markPanic(const char* msg);

  // NOTE: no non-trivial C++ locals may exist in this function.
  void bootCore();
  void closeStateNoThrow();
  void captureTopError();
  [[nodiscard]] std::string describeOom() const;
  void runRom();
  void registerApi();

  // The Lua bindings are plain functions (lua_CFunction has no `this`), so both
  // of these recover what they need from the registry instead of a member.
  [[nodiscard]] static LuaContext* context(lua_State* L);
  [[nodiscard]] static const HostInfo* hostInfoFor(lua_State* L);

  // Binding implementations (all declared `noexcept` where practical).
  static int l_screen_clear(lua_State* L);
  static int l_screen_write(lua_State* L);
  static int l_screen_setCursor(lua_State* L);
  static int l_screen_getCursor(lua_State* L);
  static int l_screen_setColor(lua_State* L);
  static int l_screen_getColor(lua_State* L);
  static int l_screen_setCell(lua_State* L);
  static int l_screen_getCell(lua_State* L);
  static int l_screen_getResolution(lua_State* L);
  static int l_screen_getLimits(lua_State* L);
  static int l_screen_requestResolution(lua_State* L);
  static int l_screen_invalidate(lua_State* L);

  static int l_ram_used(lua_State* L);
  static int l_ram_total(lua_State* L);
  static int l_ram_isInfinite(lua_State* L);
  static int l_ram_rejected(lua_State* L);

  static int l_inet_request(lua_State* L);
  static int l_inet_poll(lua_State* L);
  static int l_inet_read(lua_State* L);
  static int l_inet_close(lua_State* L);
  static int l_inet_available(lua_State* L);

  static int l_host_log(lua_State* L);
  static int l_host_alert(lua_State* L);
  static int l_host_sleep(lua_State* L);
  static int l_host_reboot(lua_State* L);
  static int l_host_quit(lua_State* L);
  static int l_host_tiers(lua_State* L);
  static int l_host_internetEnabled(lua_State* L);

  // Registered once in the Lua registry; fetched by every lua_pcall so guest
  // errors come back with a traceback instead of a bare message.
  static int tracebackHandler(lua_State* L);

  lua_State* L_ = nullptr;
  std::jmp_buf panic_{};
  MemoryAllocator ownedAllocator_{};  // fallback when no host allocator is supplied
  HostInfo host_{};
  LuaContext ctx_{};
  MachineState state_ = MachineState::Stopped;
  bool panicFlag_ = false;
  std::string romSource_;  // cached ROM text, loaded before the setjmp target
  std::string lastError_;
  double lastTick_ = 0.0;
};

}  // namespace ocemu