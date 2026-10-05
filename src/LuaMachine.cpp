#include "ocemu/LuaMachine.hpp"

// Lua 5.2's headers predate the `extern "C"` guards that arrived in 5.3, so
// without this wrapper every Lua symbol gets C++ name mangling and fails to
// link against liblua5.2.
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <utility>

namespace ocemu {
namespace {

// The machine currently executing Lua. There is exactly one Lua state per
// process, so a single pointer is enough for the panic trampoline to locate the
// active setjmp target.
LuaMachine* g_activeMachine = nullptr;

// Registry keys. These only need stable addresses.
const char kContextKey = 0;
const char kErrorHandlerKey = 0;
const char kHostInfoKey = 0;

// Append to the host's alert queue, never throwing: this runs underneath a Lua
// frame, and an exception unwinding through one is undefined behaviour.
void queueAlert(LuaContext* ctx, const char* msg) {
  if (ctx == nullptr || ctx->alerts == nullptr || msg == nullptr) return;
  try {
    ctx->alerts->emplace_back(msg);
    if (ctx->alerts->size() > 64) ctx->alerts->erase(ctx->alerts->begin());
  } catch (...) {
  }
}

bool readFileToString(const std::string& path, std::string& out, std::string* err) {
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    if (err != nullptr) *err = "cannot open ROM: " + path;
    return false;
  }
  try {
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
  } catch (const std::exception& e) {
    if (err != nullptr) *err = std::string("cannot read ROM: ") + e.what();
    return false;
  }
  if (out.empty()) {
    if (err != nullptr) *err = "ROM is empty: " + path;
    return false;
  }
  return true;
}

// Component method tables live inside registerApi() instead of here: they name
// private static members, so they must be declared in a scope with access to
// the class. See the function-local statics below.

}  // namespace

LuaMachine::~LuaMachine() { shutdown(); }

// ---------------------------------------------------------------------------
//  Panic handling
// ---------------------------------------------------------------------------

int LuaMachine::luaPanicTrampoline(lua_State* L) {
  LuaMachine* self = g_activeMachine;
  if (self == nullptr) {
    std::fputs("ocemu: unprotected Lua error with no active machine\n", stderr);
    std::abort();
  }

  // lua_tostring on the error object does not allocate, so it is safe to call
  // from inside a panic while the allocator may be refusing requests.
  const char* msg = lua_tostring(L, -1);
  self->markPanic(msg);

  std::fprintf(stderr, "ocemu: Lua panic: %s\n", msg != nullptr ? msg : "(unknown error)");
  std::fflush(stderr);

  std::longjmp(self->panicJump(), 1);
}

void LuaMachine::markPanic(const char* msg) {
  panicFlag_ = true;
  try {
    if (msg != nullptr && lastError_.empty()) lastError_ = msg;
  } catch (...) {
  }
}

// ---------------------------------------------------------------------------
//  Lifecycle
// ---------------------------------------------------------------------------

bool LuaMachine::reboot(const std::string& romPath, std::string* error) {
  lastError_.clear();
  panicFlag_ = false;
  state_ = MachineState::Stopped;
  lastTick_ = 0.0;
  alloc().clearOutOfMemory();

  // Cache the ROM text *before* entering the protected region: reading it needs
  // non-trivial C++ locals that a longjmp coming back from Lua must not skip.
  std::string readErr;
  if (!readFileToString(romPath, romSource_, &readErr)) {
    lastError_ = readErr;
    if (error != nullptr) *error = readErr;
    return false;
  }

  if (setjmp(panicJump()) == 0) {
    g_activeMachine = this;
    closeStateNoThrow();
    bootCore();

    // bootCore returned normally, but the ROM may still have died on a refused
    // allocation (Lua reports that as a catchable error, not a panic).
    if (alloc().outOfMemory()) {
      state_ = MachineState::OutOfMemory;
      lastError_ = describeOom();
      if (error != nullptr) *error = lastError_;
      closeStateNoThrow();
      g_activeMachine = nullptr;
      return false;
    }

    state_ = MachineState::Running;
    g_activeMachine = nullptr;
    return true;
  }

  // ---- recovered from a panic / allocator refusal ----
  g_activeMachine = nullptr;

  const bool oom = alloc().outOfMemory();
  state_ = oom ? MachineState::OutOfMemory : MachineState::Faulted;

  std::string msg;
  if (oom) {
    msg = "out of memory: allocator refused a request of " +
          std::to_string(alloc().lastRefusedBytes()) + " bytes (" +
          std::to_string(alloc().refusedCount()) + " total)";
  }
  if (!lastError_.empty()) {
    if (!msg.empty()) msg += " | ";
    msg += lastError_;
  }
  if (msg.empty()) msg = "Lua state lost while booting the ROM";
  lastError_ = msg;

  if (error != nullptr) *error = msg;
  closeStateNoThrow();
  return false;
}

void LuaMachine::shutdown() {
  if (setjmp(panicJump()) == 0) {
    g_activeMachine = this;
    closeStateNoThrow();
    g_activeMachine = nullptr;
  } else {
    // Something went wrong even while tearing down: abandon the state rather
    // than let a second panic escape.
    g_activeMachine = nullptr;
    L_ = nullptr;
  }
  state_ = MachineState::Stopped;
}

// NOTE: deliberately free of non-trivial C++ locals -- a Lua panic longjmps
// straight past this frame's destructor chain.
void LuaMachine::bootCore() {
  // lua_newstate (not luaL_newstate) so the ROM's budget also covers the
  // registry, string interning and the metatable tables Lua creates up front.
  L_ = lua_newstate(&MemoryAllocator::luaThunk, &alloc());
  if (L_ == nullptr) {
    panicFlag_ = true;
    if (lastError_.empty()) lastError_ = "lua_newstate() returned NULL";
    return;
  }

  lua_atpanic(L_, &LuaMachine::luaPanicTrampoline);

  luaL_openlibs(L_);  // may OOM -> panic -> longjmp
  registerApi();      // may OOM -> panic -> longjmp
  runRom();           // may OOM -> panic -> longjmp
}

void LuaMachine::closeStateNoThrow() {
  if (L_ == nullptr) return;
  lua_State* dying = L_;
  L_ = nullptr;
  try {
    lua_close(dying);
  } catch (...) {
    // lua_close is not expected to throw; never let it cross this boundary.
  }
}

bool LuaMachine::guarded(const std::function<void()>& fn) {
  if (!fn) return true;

  if (setjmp(panicJump()) == 0) {
    g_activeMachine = this;
    fn();
    g_activeMachine = nullptr;
    return true;
  }

  // Recovered from a panic: the state can no longer be trusted.
  g_activeMachine = nullptr;
  state_ = alloc().outOfMemory() ? MachineState::OutOfMemory : MachineState::Faulted;
  try {
    if (lastError_.empty()) lastError_ = "panic in host callback";
  } catch (...) {
  }
  closeStateNoThrow();
  return false;
}

bool LuaMachine::runChunk(const std::string& source, const char* chunkName,
                          std::string* error) {
  return runChunkIn(source, chunkName, nullptr, error);
}

bool LuaMachine::runChunkIn(const std::string& source, const char* chunkName,
                            const char* envGlobal, std::string* error) {
  if (L_ == nullptr || state_ != MachineState::Running) {
    if (error != nullptr) *error = "no running Lua state";
    return false;
  }

  if (setjmp(panicJump()) == 0) {
    g_activeMachine = this;

    // Resolve the sandbox environment and park it well below the working area so
    // it cannot disturb the errfunc offset used by lua_pcall below.
    if (envGlobal != nullptr) {
      lua_getglobal(L_, envGlobal);
      if (!lua_istable(L_, -1)) lua_pop(L_, 1);
    }

    lua_rawgetp(L_, LUA_REGISTRYINDEX, const_cast<char*>(&kErrorHandlerKey));

    if (luaL_loadbuffer(L_, source.data(), source.size(), chunkName) != LUA_OK) {
      captureTopError();
      g_activeMachine = nullptr;
      if (error != nullptr) *error = lastError_;
      return false;
    }

    // Give the chunk the sandbox as its _ENV, if we found one.
    if (envGlobal != nullptr && lua_gettop(L_) > 2) {
      lua_pushvalue(L_, 1);
      lua_setupvalue(L_, -2, 1);
    }
    // Stack is [env][errfunc][chunk]; with no arguments errfunc is at -(0+2).
    if (lua_pcall(L_, 0, 0, -2) != LUA_OK) {
      captureTopError();
      g_activeMachine = nullptr;
      if (error != nullptr) *error = lastError_;
      return false;
    }
    lua_settop(L_, 0);

    g_activeMachine = nullptr;

    if (alloc().outOfMemory()) {
      state_ = MachineState::OutOfMemory;
      lastError_ = describeOom();
      if (error != nullptr) *error = lastError_;
      closeStateNoThrow();
      return false;
    }
    return true;
  }

  // Panicked while running the injected chunk: the state is not trustworthy.
  g_activeMachine = nullptr;
  state_ = alloc().outOfMemory() ? MachineState::OutOfMemory : MachineState::Faulted;
  try {
    if (lastError_.empty()) lastError_ = "panic while running injected chunk";
  } catch (...) {
  }
  closeStateNoThrow();
  if (error != nullptr) *error = lastError_;
  return false;
}

void LuaMachine::captureTopError() {
  if (L_ == nullptr) return;
  const char* msg = lua_tostring(L_, -1);
  try {
    lastError_ = (msg != nullptr) ? msg : "unknown Lua error";
  } catch (...) {
  }
  lua_settop(L_, 0);

  // A refused allocation does NOT reach the panic handler: lua_pcall catches
  // LUA_ERRMEM and returns it as an ordinary error. The allocator still knows
  // it refused something, so that is what decides the resulting state.
  if (alloc().outOfMemory() && state_ == MachineState::Running) {
    state_ = MachineState::OutOfMemory;
    try {
      lastError_ = describeOom() + " | " + lastError_;
    } catch (...) {
    }
  }
}

std::string LuaMachine::describeOom() const {
  std::string msg = "out of memory: the allocator refused a request";
  if (!alloc().infinite()) {
    msg += " of " + std::to_string(alloc().lastRefusedBytes()) + " bytes (cap " +
           std::to_string(alloc().limitBytes()) + " bytes, " +
           std::to_string(alloc().refusedCount()) + " refusals)";
  } else {
    msg += " (host allocation failed)";
  }
  return msg;
}

void LuaMachine::runRom() {
  if (L_ == nullptr) return;

  // errfunc for the pcall, fetched from the registry.
  lua_rawgetp(L_, LUA_REGISTRYINDEX, const_cast<char*>(&kErrorHandlerKey));

  if (luaL_loadbuffer(L_, romSource_.data(), romSource_.size(), "=rom/boot.lua") != LUA_OK) {
    captureTopError();
    return;
  }

  // Stack is [errfunc][chunk] with no arguments, so the error function sits at
  // -(nargs + 2) == -2. lua_pcall pops both the chunk and the handler.
  if (lua_pcall(L_, 0, 0, -2) != LUA_OK) {
    captureTopError();
  }
}

int LuaMachine::tracebackHandler(lua_State* L) {
  const char* msg = lua_tostring(L, 1);
  if (msg == nullptr) {
    if (luaL_callmeta(L, 1, "__tostring") == 0 && lua_type(L, -1) == LUA_TSTRING) {
      return 1;
    }
    lua_pop(L, 1);
    const char* typeName = luaL_typename(L, 1);
    lua_pushfstring(L, "(error object is a %s value)", typeName != nullptr ? typeName : "?");
    if (lua_tostring(L, -1) == nullptr) {
      lua_pop(L, 1);
      lua_pushliteral(L, "(error object could not be stringified)");
    }
  }
  luaL_traceback(L, L, msg, 1);
  return 1;
}

void LuaMachine::update(double dtSeconds) {
  lastTick_ = dtSeconds;

  // Advance the network even when the guest is idle or never calls poll().
  if (ctx_.internet != nullptr) ctx_.internet->pump();

  if (L_ == nullptr || state_ != MachineState::Running) return;

  if (setjmp(panicJump()) == 0) {
    g_activeMachine = this;

    lua_getglobal(L_, "host");
    if (lua_istable(L_, -1)) {
      lua_getfield(L_, -1, "onTick");
      if (lua_isfunction(L_, -1)) {
        lua_rawgetp(L_, LUA_REGISTRYINDEX, const_cast<char*>(&kErrorHandlerKey));
        lua_insert(L_, -2);  // -> [host][errfunc][onTick]
        lua_pushnumber(L_, dtSeconds);
        // One argument follows the callee, so errfunc is at -(nargs + 2) == -3.
        if (lua_pcall(L_, 1, 0, -3) != LUA_OK) {
          captureTopError();
        }
      } else {
        lua_pop(L_, 1);
      }
    }
    lua_settop(L_, 0);

    // A refusal inside the tick handler leaves us out of memory even though the
    // pcall returned normally.
    if (alloc().outOfMemory()) {
      state_ = MachineState::OutOfMemory;
      try {
        lastError_ = describeOom();
      } catch (...) {
      }
      closeStateNoThrow();
    }

    g_activeMachine = nullptr;
    return;
  }

  // ---- panicked while ticking: the state can no longer be trusted ----
  g_activeMachine = nullptr;
  const bool oom = alloc().outOfMemory();
  state_ = oom ? MachineState::OutOfMemory : MachineState::Faulted;
  try {
    if (oom && !alloc().infinite()) {
      lastError_ = "out of memory while ticking the Lua state (limit " +
                   std::to_string(alloc().limitBytes()) + " bytes)";
    } else if (lastError_.empty()) {
      lastError_ = "Lua panicked in host.onTick";
    }
  } catch (...) {
  }
  closeStateNoThrow();
}

void LuaMachine::pushAlert(const char* msg) { queueAlert(&ctx_, msg); }

// ---------------------------------------------------------------------------
//  Binding helpers
// ---------------------------------------------------------------------------

LuaContext* LuaMachine::context(lua_State* L) {
  lua_pushlightuserdata(L, const_cast<char*>(&kContextKey));
  lua_rawget(L, LUA_REGISTRYINDEX);
  auto* ctx = static_cast<LuaContext*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return ctx;
}

const HostInfo* LuaMachine::hostInfoFor(lua_State* L) {
  lua_pushlightuserdata(L, const_cast<char*>(&kHostInfoKey));
  lua_rawget(L, LUA_REGISTRYINDEX);
  const auto* info = static_cast<const HostInfo*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return info;
}

// ---------------------------------------------------------------------------
//  Component registration
// ---------------------------------------------------------------------------

void LuaMachine::registerApi() {
  // Function-local statics: these name private members, and being aggregates of
  // constant expressions they need no runtime initialisation guard.
  static const luaL_Reg screenRegs[] = {
      {"clear", &LuaMachine::l_screen_clear},
      {"write", &LuaMachine::l_screen_write},
      {"setCursor", &LuaMachine::l_screen_setCursor},
      {"getCursor", &LuaMachine::l_screen_getCursor},
      {"setColor", &LuaMachine::l_screen_setColor},
      {"getColor", &LuaMachine::l_screen_getColor},
      {"setCell", &LuaMachine::l_screen_setCell},
      {"getCell", &LuaMachine::l_screen_getCell},
      {"getResolution", &LuaMachine::l_screen_getResolution},
      {"getLimits", &LuaMachine::l_screen_getLimits},
      {"requestResolution", &LuaMachine::l_screen_requestResolution},
      {"invalidate", &LuaMachine::l_screen_invalidate},
      {nullptr, nullptr},
  };

  static const luaL_Reg ramRegs[] = {
      {"used", &LuaMachine::l_ram_used},
      {"total", &LuaMachine::l_ram_total},
      {"isInfinite", &LuaMachine::l_ram_isInfinite},
      {"rejected", &LuaMachine::l_ram_rejected},
      {nullptr, nullptr},
  };

  static const luaL_Reg internetRegs[] = {
      {"request", &LuaMachine::l_inet_request},
      {"poll", &LuaMachine::l_inet_poll},
      {"read", &LuaMachine::l_inet_read},
      {"close", &LuaMachine::l_inet_close},
      {"available", &LuaMachine::l_inet_available},
      {nullptr, nullptr},
  };

  static const luaL_Reg hostRegs[] = {
      {"log", &LuaMachine::l_host_log},
      {"alert", &LuaMachine::l_host_alert},
      {"sleep", &LuaMachine::l_host_sleep},
      {"reboot", &LuaMachine::l_host_reboot},
      {"quit", &LuaMachine::l_host_quit},
      {"tiers", &LuaMachine::l_host_tiers},
      {"internetEnabled", &LuaMachine::l_host_internetEnabled},
      {nullptr, nullptr},
  };

  const bool bindInternet = host_.internetEnabled && ctx_.internet != nullptr;

  // Stash the context so every binding can find it without a global.
  lua_pushlightuserdata(L_, const_cast<char*>(&kContextKey));
  lua_pushlightuserdata(L_, static_cast<void*>(&ctx_));
  lua_rawset(L_, LUA_REGISTRYINDEX);

  // Same trick for the machine description, which the static bindings read.
  lua_pushlightuserdata(L_, const_cast<char*>(&kHostInfoKey));
  lua_pushlightuserdata(L_, static_cast<void*>(&host_));
  lua_rawset(L_, LUA_REGISTRYINDEX);

  // Message handler, reused as the errfunc of every protected call.
  lua_pushcfunction(L_, &LuaMachine::tracebackHandler);
  lua_rawsetp(L_, LUA_REGISTRYINDEX, const_cast<char*>(&kErrorHandlerKey));

  // --- component table ---
  lua_newtable(L_);

  lua_newtable(L_);
  luaL_setfuncs(L_, screenRegs, 0);
  lua_setfield(L_, -2, "screen");

  lua_newtable(L_);
  luaL_setfuncs(L_, ramRegs, 0);
  lua_setfield(L_, -2, "ram");

  // The internet card is only bound when the operator enabled it.
  if (bindInternet) {
    lua_newtable(L_);
    luaL_setfuncs(L_, internetRegs, 0);
    lua_setfield(L_, -2, "internet");
  }

  lua_setglobal(L_, "component");

  // --- convenience globals (the same tables under short names) ---
  lua_newtable(L_);
  luaL_setfuncs(L_, screenRegs, 0);
  lua_setglobal(L_, "screen");

  lua_newtable(L_);
  luaL_setfuncs(L_, ramRegs, 0);
  lua_setglobal(L_, "ram");

  if (bindInternet) {
    lua_newtable(L_);
    luaL_setfuncs(L_, internetRegs, 0);
    lua_setglobal(L_, "internet");
  }

  lua_newtable(L_);
  luaL_setfuncs(L_, hostRegs, 0);
  lua_setglobal(L_, "host");

  // `ocemu` mirrors `host` so guest code can namespace its calls if it wants.
  lua_newtable(L_);
  luaL_setfuncs(L_, hostRegs, 0);
  lua_setglobal(L_, "ocemu");
}

// ---------------------------------------------------------------------------
//  screen
// ---------------------------------------------------------------------------

int LuaMachine::l_screen_clear(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  ctx->screen->clear();
  return 0;
}

int LuaMachine::l_screen_write(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  std::size_t len = 0;
  const char* s = luaL_checklstring(L, 1, &len);
  ctx->screen->writeUtf8(s, len);
  return 0;
}

int LuaMachine::l_screen_setCursor(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  const int x = static_cast<int>(luaL_checkinteger(L, 1));
  const int y = static_cast<int>(luaL_checkinteger(L, 2));
  ctx->screen->setCursor(x, y);
  return 0;
}

int LuaMachine::l_screen_getCursor(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  lua_pushinteger(L, ctx->screen->cursorX());
  lua_pushinteger(L, ctx->screen->cursorY());
  return 2;
}

int LuaMachine::l_screen_setColor(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  const Rgb fg = rgbFromU32(static_cast<std::uint32_t>(luaL_checkinteger(L, 1)));
  const Rgb bg = rgbFromU32(static_cast<std::uint32_t>(luaL_optinteger(L, 2, 0)));
  ctx->screen->setPalette(fg, bg);
  return 0;
}

int LuaMachine::l_screen_getColor(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  lua_pushinteger(L, static_cast<lua_Integer>(packRgb(ctx->screen->paletteFg())));
  lua_pushinteger(L, static_cast<lua_Integer>(packRgb(ctx->screen->paletteBg())));
  return 2;
}

int LuaMachine::l_screen_setCell(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  const int x = static_cast<int>(luaL_checkinteger(L, 1));
  const int y = static_cast<int>(luaL_checkinteger(L, 2));
  const auto glyph =
      static_cast<std::uint16_t>(luaL_checkinteger(L, 3) & 0xFFFF);
  const Rgb fg = rgbFromU32(static_cast<std::uint32_t>(luaL_optinteger(L, 4, 0xFFFFFF)));
  const Rgb bg = rgbFromU32(static_cast<std::uint32_t>(luaL_optinteger(L, 5, 0)));
  ctx->screen->setCell(x, y, glyph, fg, bg);
  return 0;
}

int LuaMachine::l_screen_getCell(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  const int x = static_cast<int>(luaL_checkinteger(L, 1));
  const int y = static_cast<int>(luaL_checkinteger(L, 2));
  const Cell& c = ctx->screen->cell(x, y);
  lua_pushinteger(L, c.glyph);
  lua_pushinteger(L, static_cast<lua_Integer>(packRgb(c.fg)));
  lua_pushinteger(L, static_cast<lua_Integer>(packRgb(c.bg)));
  return 3;
}

int LuaMachine::l_screen_getResolution(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  lua_pushinteger(L, ctx->screen->cols());
  lua_pushinteger(L, ctx->screen->rows());
  lua_pushinteger(L, ctx->screen->depth());
  return 3;
}

int LuaMachine::l_screen_getLimits(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  const TierSpec& lim = ctx->screen->limits();
  lua_pushinteger(L, lim.cols);
  lua_pushinteger(L, lim.rows);
  lua_pushinteger(L, lim.depth);
  return 3;
}

int LuaMachine::l_screen_requestResolution(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  const HostInfo* info = hostInfoFor(L);
  const int defaultDepth = (info != nullptr) ? info->depth : 4;

  const int wantCols = static_cast<int>(luaL_checkinteger(L, 1));
  const int wantRows = static_cast<int>(luaL_checkinteger(L, 2));
  const int wantDepth = static_cast<int>(luaL_optinteger(L, 3, defaultDepth));

  const bool exact = ctx->screen->requestResolution(wantCols, wantRows, wantDepth);

  lua_pushboolean(L, exact);
  lua_pushinteger(L, ctx->screen->requestedCols());
  lua_pushinteger(L, ctx->screen->requestedRows());
  lua_pushinteger(L, ctx->screen->requestedDepth());
  return 4;
}

int LuaMachine::l_screen_invalidate(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->screen == nullptr) {
    return luaL_error(L, "screen component unavailable");
  }
  ctx->screen->invalidate();
  return 0;
}

// ---------------------------------------------------------------------------
//  ram
// ---------------------------------------------------------------------------

int LuaMachine::l_ram_used(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->allocator == nullptr) {
    return luaL_error(L, "ram component unavailable");
  }
  lua_pushinteger(L, static_cast<lua_Integer>(ctx->allocator->usedBytes()));
  return 1;
}

int LuaMachine::l_ram_total(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->allocator == nullptr) {
    return luaL_error(L, "ram component unavailable");
  }
  // -1 is the sentinel the Component Manager panel uses for "infinite".
  if (ctx->allocator->infinite()) {
    lua_pushinteger(L, -1);
  } else {
    lua_pushinteger(L, static_cast<lua_Integer>(ctx->allocator->limitBytes()));
  }
  return 1;
}

int LuaMachine::l_ram_isInfinite(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->allocator == nullptr) {
    return luaL_error(L, "ram component unavailable");
  }
  lua_pushboolean(L, ctx->allocator->infinite());
  return 1;
}

int LuaMachine::l_ram_rejected(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->allocator == nullptr) {
    return luaL_error(L, "ram component unavailable");
  }
  lua_pushinteger(L, static_cast<lua_Integer>(ctx->allocator->refusedBytes()));
  lua_pushinteger(L, static_cast<lua_Integer>(ctx->allocator->refusedCount()));
  return 2;
}

// ---------------------------------------------------------------------------
//  internet
//
//  Only these bindings run when the allocator refuses a push, so all locals
//  below are plain-old-data: a longjmp has nothing to leak.
// ---------------------------------------------------------------------------

int LuaMachine::l_inet_request(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->internet == nullptr) {
    return luaL_error(L, "internet component is not installed");
  }
  const char* url = luaL_checkstring(L, 1);

  char err[256] = {0};
  const std::uint64_t handle = ctx->internet->request(url, err, sizeof(err));
  if (handle == 0) {
    lua_pushnil(L);
    lua_pushstring(L, err[0] != '\0' ? err : "request failed");
    return 2;
  }
  lua_pushinteger(L, static_cast<lua_Integer>(handle));
  return 1;
}

int LuaMachine::l_inet_poll(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->internet == nullptr) {
    return luaL_error(L, "internet component is not installed");
  }
  const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));

  InternetPoll info{};
  ctx->internet->poll(handle, &info);

  lua_pushstring(L, internetStatusName(info.status));
  if (info.hasChunk) {
    lua_pushlstring(L, info.chunk, info.chunkLen);
  } else {
    lua_pushnil(L);
  }
  lua_pushstring(L, info.error[0] != '\0' ? info.error : "");
  lua_pushinteger(L, static_cast<lua_Integer>(info.httpCode));
  return 4;
}

int LuaMachine::l_inet_read(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->internet == nullptr) {
    return luaL_error(L, "internet component is not installed");
  }
  const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));

  char buf[4096];
  std::size_t total = 0;
  const std::size_t got = ctx->internet->copyResponse(handle, buf, sizeof(buf), &total);

  lua_pushlstring(L, buf, got);
  lua_pushinteger(L, static_cast<lua_Integer>(total));
  lua_pushboolean(L, total > got);
  return 3;
}

int LuaMachine::l_inet_close(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx == nullptr || ctx->internet == nullptr) {
    return luaL_error(L, "internet component is not installed");
  }
  const auto handle = static_cast<std::uint64_t>(luaL_checkinteger(L, 1));
  ctx->internet->close(handle);
  return 0;
}

int LuaMachine::l_inet_available(lua_State* L) {
  LuaContext* ctx = context(L);
  lua_pushboolean(L, ctx != nullptr && ctx->internet != nullptr && ctx->internet->ready());
  return 1;
}

// ---------------------------------------------------------------------------
//  host services
// ---------------------------------------------------------------------------

int LuaMachine::l_host_log(lua_State* L) {
  LuaContext* ctx = context(L);
  const char* msg = luaL_checkstring(L, 1);
  if (ctx != nullptr && ctx->logLines != nullptr) {
    try {
      ctx->logLines->emplace_back(msg);
      if (ctx->logLines->size() > 512) ctx->logLines->erase(ctx->logLines->begin());
    } catch (...) {
    }
  }
  return 0;
}

int LuaMachine::l_host_alert(lua_State* L) {
  const char* msg = luaL_checkstring(L, 1);
  queueAlert(context(L), msg);
  return 0;
}

int LuaMachine::l_host_sleep(lua_State* L) {
  LuaContext* ctx = context(L);
  const double seconds = static_cast<double>(luaL_checknumber(L, 1));

  const bool* stop = (ctx != nullptr) ? ctx->quitRequested : nullptr;
  const double actual =
      (ctx != nullptr && ctx->internet != nullptr) ? ctx->internet->pumpFor(seconds, stop) : 0.0;

  lua_pushnumber(L, actual);
  return 1;
}

int LuaMachine::l_host_reboot(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx != nullptr && ctx->rebootRequested != nullptr) *ctx->rebootRequested = true;
  return 0;
}

int LuaMachine::l_host_quit(lua_State* L) {
  LuaContext* ctx = context(L);
  if (ctx != nullptr && ctx->quitRequested != nullptr) *ctx->quitRequested = true;
  return 0;
}

int LuaMachine::l_host_tiers(lua_State* L) {
  const HostInfo* info = hostInfoFor(L);
  if (info == nullptr) {
    lua_pushinteger(L, 2);
    lua_pushinteger(L, 2);
    lua_pushinteger(L, 512);
    lua_pushboolean(L, false);
    return 4;
  }
  lua_pushinteger(L, info->gpuTier);
  lua_pushinteger(L, info->screenTier);
  lua_pushinteger(L, info->ramLimitKb);
  lua_pushboolean(L, info->internetEnabled);
  return 4;
}

int LuaMachine::l_host_internetEnabled(lua_State* L) {
  const HostInfo* info = hostInfoFor(L);
  lua_pushboolean(L, info != nullptr && info->internetEnabled);
  return 1;
}

}  // namespace ocemu