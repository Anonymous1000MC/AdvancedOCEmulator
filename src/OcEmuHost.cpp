#include "ocemu/OcEmuHost.hpp"

#include "ocemu/OcScreen.hpp"

// Lua 5.2 headers have no extern "C" guards (added in 5.3).
extern "C" {
#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cctype>
#include <limits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ocemu {
namespace {

// The elsa table and this host live side by side in the registry, so the static
// thunks can find the instance without capturing a `this`.
const char kElsaKey = 0;

OcEmuHost* self(lua_State* L) {
  lua_pushlightuserdata(L, const_cast<char*>(&kElsaKey));
  lua_rawget(L, LUA_REGISTRYINDEX);
  auto* h = static_cast<OcEmuHost*>(lua_touserdata(L, -1));
  lua_pop(L, 1);
  return h;
}

[[noreturn]] void fail(lua_State* L, const std::string& msg) {
  lua_pushnil(L);
  lua_pushstring(L, msg.c_str());
  lua_error(L);
  // lua_error does not return
  __builtin_unreachable();
}

}  // namespace

OcEmuHost::~OcEmuHost() = default;

// ---------------------------------------------------------------------------
//  Path resolution
//
//  OCEmu's components use two kinds of path:
//    * absolute, built as getSaveDirectory() .. "/" .. address  (machine data)
//    * relative, resolved against OCEmu's own src/ (lua/bios.lua, font.hex,
//      loot/openos, config.lua, ...)
//  So: absolute means "as-is", relative means "under src/".
// ---------------------------------------------------------------------------
std::string OcEmuHost::resolvePath(const std::string& relative) const {
  if (!relative.empty() && relative.front() == '/') return relative;
  return (fs::path(ocsrcDir_) / relative).string();
}

// ---------------------------------------------------------------------------
//  elsa.filesystem
// ---------------------------------------------------------------------------

int OcEmuHost::luaFsExists(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  std::error_code ec;
  lua_pushboolean(L, fs::exists(self(L)->resolvePath(p), ec));
  return 1;
}

int OcEmuHost::luaFsIsDirectory(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  std::error_code ec;
  lua_pushboolean(L, fs::is_directory(self(L)->resolvePath(p), ec));
  return 1;
}

int OcEmuHost::luaFsCreateDirectory(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  std::error_code ec;
  const auto full = self(L)->resolvePath(p);
  if (fs::is_directory(full, ec)) {
    lua_pushboolean(L, true);
    return 1;
  }
  lua_pushboolean(L, fs::create_directories(full, ec));
  return 1;
}

// --- native keyboard ------------------------------------------------------
// OpenComputers key codes (lib/keyboard.lua in the guest uses these).
namespace {

constexpr int kMaxKeys = 512;

struct KeyboardState {
  bool down[kMaxKeys] = {false};
  bool shift = false;
  bool control = false;
  bool alt = false;
};

KeyboardState g_keys;

// Defined below, with the key-event queue.
int keycodeFromName(const std::string& name);

// ---- host-side key state ----------------------------------------------------
//
// OC's keyboard component exposes these to the guest for polling modifiers and
// held keys. We answer from a small table the SDL layer keeps up to date, so
// OpenOS's cursor sees Ctrl-C, Shift and friends correctly.

static bool keyIndex(lua_State* L, int* code) {
  if (lua_isnumber(L, 1)) {
    *code = static_cast<int>(lua_tointeger(L, 1));
  } else if (lua_isstring(L, 1)) {
    const char* name = lua_tostring(L, 1);
    *code = name != nullptr ? keycodeFromName(name) : 0;
  } else {
    return false;
  }
  return *code >= 0 && *code < kMaxKeys;
}

static int kbIsKeyDown(lua_State* L) {
  int code = 0;
  lua_pushboolean(L, keyIndex(L, &code) && g_keys.down[code]);
  return 1;
}
static int kbIsShiftDown(lua_State* L) {
  lua_pushboolean(L, g_keys.shift);
  return 1;
}
static int kbIsControlDown(lua_State* L) {
  lua_pushboolean(L, g_keys.control);
  return 1;
}
static int kbIsAltDown(lua_State* L) {
  lua_pushboolean(L, g_keys.alt);
  return 1;
}
static int kbSetKeyDown(lua_State* L) {
  int code = 0;
  if (keyIndex(L, &code)) g_keys.down[code] = true;
  return 0;
}
static int kbSetKeyUp(lua_State* L) {
  int code = 0;
  if (keyIndex(L, &code)) g_keys.down[code] = false;
  return 0;
}
static int kbModifier(lua_State* L) {
  const char* which = luaL_checkstring(L, 1);
  const bool on = lua_toboolean(L, 2) != 0;
  if (std::strcmp(which, "shift") == 0) g_keys.shift = on;
  else if (std::strcmp(which, "control") == 0) g_keys.control = on;
  else if (std::strcmp(which, "alt") == 0) g_keys.alt = on;
  return 0;
}

// Returns (obj, cec, mai, di) like every OCEmu component constructor.

// The guest's keyboard address, needed to tag queued events the way
// keyboard_sdl2.lua does (`{type = "key_down", addr = address, code = ...}`).
static const char* g_kbAddress = nullptr;

// Appends one entry to OCEmu's host-global `kbdcodes` queue. main.lua's
// elsa.update drains ONE entry per tick and re-shapes it into
// machine.signals as {type, addr, char, code}.
//
// Shape it exactly like keyboard_sdl2.lua does:
//   key_down -> {type = "key_down", addr = address, code = <lwjgl keycode>}
// Note it deliberately carries NO `char`, so the guest sees char = 0 and reads
// the printable character from `code` (LWJGL keycodes for letters and digits
// equal their ASCII value). Sending a char here made the shell mis-decode
// every keystroke.
static void queueKeyEvent(lua_State* L, const char* type, int code, int ch) {
  lua_getglobal(L, "kbdcodes");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }
  const lua_Integer n = static_cast<lua_Integer>(lua_rawlen(L, -1));
  lua_newtable(L);
  lua_pushstring(L, type);
  lua_setfield(L, -2, "type");
  lua_pushstring(L, g_kbAddress ? g_kbAddress : "");
  lua_setfield(L, -2, "addr");
  lua_pushinteger(L, code);
  lua_setfield(L, -2, "code");
  // The character the key delivers. OpenOS echoes `char` for a printable key;
  // without it the guest gets an event with nothing to draw, so the cursor
  // blink pauses but no text ever appears.
  lua_pushinteger(L, ch);
  lua_setfield(L, -2, "char");
  lua_rawseti(L, -2, n + 1);
  lua_pop(L, 1);
}

// LWJGL keycodes and the character each named key delivers.
//
// These are OCEmu's own numbers, taken from support/sdl_to_lwjgl.lua (the
// scancode -> LWJGL table plus its LWJGL -> char companion). They are NOT
// GLFW's: GLFW says Enter is 257 where OCEmu says 28. Using the wrong table
// makes the guest receive keys it cannot interpret.
struct NamedKey {
  const char* name;
  int code;    // LWJGL keycode
  int ch;      // character the key delivers, or 0 for none
};

enum : int {
  kKeyEscape = 1,
  kKeyBackspace = 14,
  kKeyTab = 15,
  kKeyEnter = 28,
  kKeyDelete = 211,
  kKeyRight = 205,
  kKeyLeft = 203,
  kKeyUp = 200,
  kKeyDown = 208,
  kKeyHome = 199,
  kKeyEnd = 207,
  kKeyPageUp = 201,
  kKeyPageDown = 209,
};

static const NamedKey kNamedKeys[] = {
    {"escape", kKeyEscape, 0},
    {"esc", kKeyEscape, 0},
    {"backspace", kKeyBackspace, 8},
    {"tab", kKeyTab, 9},
    {"enter", kKeyEnter, 13},
    {"return", kKeyEnter, 13},
    {"delete", kKeyDelete, 127},
    {"right", kKeyRight, 0},
    {"left", kKeyLeft, 0},
    {"up", kKeyUp, 0},
    {"down", kKeyDown, 0},
    {"home", kKeyHome, 0},
    {"end", kKeyEnd, 0},
    {"pageup", kKeyPageUp, 0},
    {"pagedown", kKeyPageDown, 0},
    {"space", ' ', ' '},
};

// The character for a raw LWJGL keycode, using the same LWJGL -> char mapping
// OCEmu keeps in support/sdl_to_lwjgl.lua.
int charForCode(int code) {
  for (const auto& k : kNamedKeys) {
    if (k.code == code) return k.ch;
  }
  // A printable ASCII keycode IS its own character, so this preserves case.
  if (code >= 32 && code < 127) return code;
  return 0;
}

std::string toLower(const std::string& name) {
  std::string lower;
  lower.reserve(name.size());
  for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return lower;
}

// SDL_GetKeyName() reports these; they are modifiers, not characters.
static bool isModifierName(const std::string& lower) {
  return lower == "shift" || lower == "ctrl" || lower == "control" ||
         lower == "alt" || lower == "altgr" || lower == "lshift" ||
         lower == "rshift" || lower == "lctrl" || lower == "rctrl" ||
         lower == "lalt" || lower == "ralt" || lower == "capslock" ||
         lower == "numlock" || lower == "scrolllock";
}

// The character a named key delivers, or 0 if it is a pure navigation key.
// A single printable character is its own character, case preserved.
int charForName(const std::string& name) {
  for (const auto& k : kNamedKeys) {
    if (toLower(name) == k.name) return k.ch;
  }
  if (name.size() == 1) {
    const unsigned char c = static_cast<unsigned char>(name[0]);
    if (c >= 32 && c < 127) return c;
  }
  return 0;
}

int keycodeFromName(const std::string& name) {
  // Named keys are matched case-insensitively (SDL reports "Return", "Escape").
  const std::string lower = toLower(name);
  if (isModifierName(lower)) return 0;
  for (const auto& k : kNamedKeys) {
    if (lower == k.name) return k.code;
  }
  // A single character is a printable key and MUST keep its case: SDL reports
  // "A" for Shift+a and for CapsLock+a, and the guest needs the uppercase
  // character. Lowercasing here silently turned every capital into a lowercase
  // letter, so Shift and CapsLock appeared to do nothing.
  if (name.size() == 1) {
    const unsigned char c = static_cast<unsigned char>(name[0]);
    if (c >= 32 && c < 127) return c;
  }
  if (lower.size() >= 2 && lower[0] == 'f') {
    const int n = std::atoi(lower.c_str() + 1);
    if (n >= 1 && n <= 12) return 58 + n;  // F1..F12
  }
  return 0;
}


// pushKey(key) -- accepts an LWJGL keycode or a key name ("enter", "a", ...).
static int kbPushKey(lua_State* L) {
  int code = 0;
  std::string name;
  if (lua_isnumber(L, 1)) {
    code = static_cast<int>(lua_tointeger(L, 1));
  } else if (lua_isstring(L, 1)) {
    name = lua_tostring(L, 1);
    code = keycodeFromName(name);
  }
  if (code == 0) return 0;
  // A raw keycode carries no name to look up, so derive the character from the
  // codes OCEmu itself uses (Enter -> 13, Backspace -> 8, ...).
  int ch = charForName(name);
  if (ch == 0) ch = charForCode(code);
  queueKeyEvent(L, "key_down", code, ch);
  return 0;
}

// pushChar(char) -- a printable character. The guest derives the character from
// the keycode, so a printable char and its keycode are the same number.
static int kbPushChar(lua_State* L) {
  int ch = 0;
  if (lua_isnumber(L, 1)) {
    ch = static_cast<int>(lua_tointeger(L, 1));
  } else if (lua_isstring(L, 1)) {
    size_t len = 0;
    const char* str = lua_tolstring(L, 1, &len);
    ch = (str != nullptr && len > 0) ? static_cast<unsigned char>(str[0]) : 0;
  }
  queueKeyEvent(L, "key_down", ch, ch);
  return 0;
}

// signalKey(code [, char [, modifiers]]) -- the shape OC's keyboard uses for a
// synthetic keypress. Modifiers are accepted and ignored: OpenOS reads them
// from gpu.isControlDown()/isShiftDown(), which we answer separately.
static int kbSignalKey(lua_State* L) {
  int code = 0;
  std::string name;
  if (lua_isnumber(L, 1)) code = static_cast<int>(lua_tointeger(L, 1));
  else if (lua_isstring(L, 1)) {
    name = lua_tostring(L, 1);
    code = keycodeFromName(name);
  }
  if (code == 0) return 0;
  int ch = charForName(name);
  if (ch == 0) ch = charForCode(code);
  queueKeyEvent(L, "key_down", code, ch);
  return 0;
}

int luaKeyboardComponent(lua_State* L) {
  struct { const char* name; lua_CFunction fn; } fns[] = {
      {"isKeyDown", &kbIsKeyDown},
      {"isShiftDown", &kbIsShiftDown},
      {"isControlDown", &kbIsControlDown},
      {"isAltDown", &kbIsAltDown},
      {"setKeyDown", &kbSetKeyDown},
      {"setKeyUp", &kbSetKeyUp},
      {"setModifier", &kbModifier},
      // Guest-facing input API. OpenOS's console blocks on key events, so
      // without these it never gets past its prompt.
      {"pushKey", &kbPushKey},
      {"pushChar", &kbPushChar},
      {"signalKey", &kbSignalKey},
  };
  // Record the OC address so queued events are attributed to this keyboard.
  if (lua_gettop(L) >= 1 && lua_type(L, 1) == LUA_TSTRING) {
    g_kbAddress = lua_tostring(L, 1);
  }

  lua_newtable(L);
  // Real OpenComputers type name, not our internal component name.
  lua_pushstring(L, "keyboard");
  lua_setfield(L, -2, "type");
  for (const auto& f : fns) {
    lua_pushcfunction(L, f.fn);
    lua_setfield(L, -2, f.name);
  }
  lua_newtable(L);  // cec (none: keyboard methods are all proxied)
  lua_newtable(L);  // mai
  lua_newtable(L);  // di
  lua_pushstring(L, "keyboard");
  lua_setfield(L, -2, "class");
  lua_pushstring(L, "Keyboard");
  lua_setfield(L, -2, "description");
  lua_pushstring(L, "ocemu");
  lua_setfield(L, -2, "vendor");
  lua_pushstring(L, "ocemu Keyboard");
  lua_setfield(L, -2, "product");
  return 4;
}

}  // namespace

// elsa.filesystem.load(path [, mode [, env]]) -> chunk | nil, err
//
// Mirrors loadfile(): reads the file and compiles it. When `env` is supplied it
// becomes the chunk's single upvalue, which is how OCEmu sandboxes BIOS and
// persisted EEPROM code.
int OcEmuHost::luaFsLoad(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  const char* mode = luaL_optstring(L, 2, nullptr);

  // Components we implement natively. apis/component.lua builds the path as
  // "component/<type>.lua" and then calls the result with the component's
  // arguments, expecting (proxy, cec, mai, di) back -- so handing it a C
  // constructor here needs no change on OCEmu's side at all.
  if (p != nullptr) {
    if (std::strcmp(p, "component/screen_native.lua") == 0) {
      lua_pushcfunction(L, &OcScreen::luaComponent);
      return 1;
    }
    if (std::strcmp(p, "component/keyboard_native.lua") == 0) {
      lua_pushcfunction(L, &luaKeyboardComponent);
      return 1;
    }
  }

  const auto full = self(L)->resolvePath(p);
  std::ifstream in(full, std::ios::binary);
  if (!in) {
    lua_pushnil(L);
    lua_pushfstring(L, "cannot open %s: no such file", p);
    return 2;
  }

  std::ostringstream ss;
  ss << in.rdbuf();

  const std::string data = ss.str();
  const std::string chunkName = "=" + std::string(p);

  if (luaL_loadbuffer(L, data.data(), data.size(), chunkName.c_str()) != LUA_OK) {
    // luaL_loadbuffer leaves the error on the stack; hand it back as nil, err.
    lua_pushnil(L);
    lua_insert(L, -2);
    return 2;
  }

  // Optional custom environment for the loaded chunk.
  if (!lua_isnoneornil(L, 3)) {
    const int envIdx = lua_absindex(L, 3);
    lua_pushvalue(L, envIdx);
    lua_setupvalue(L, -2, 1);

    // Hand the sandbox OCEmu's *global* component.cecinvoke.
    //
    // env.component only carries list/type/slot/methods/invoke, and its invoke
    // is cost-accounted: it bails out returning NO values once the per-tick call
    // budget is spent, and its direct path evaluates 1/limit where `limit` is
    // often unset or the string "inf". The global cecinvoke goes straight to
    // emuicc[address][method] with neither problem -- it is the path OCEmu's own
    // gpu.lua uses successfully. Without it OpenOS cannot read a single file.
    lua_getglobal(L, "component");
    if (lua_istable(L, -1)) {
      lua_getfield(L, -1, "cecinvoke");
      if (lua_isfunction(L, -1)) {
        lua_setfield(L, envIdx, "__ocemuCecInvoke");
      } else {
        lua_pop(L, 1);
      }
    }
    lua_pop(L, 1);
  }
  (void)mode;
  return 1;
}

// elsa.filesystem.read(path) -> data, #data | nil, err
int OcEmuHost::luaFsRead(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  std::ifstream in(self(L)->resolvePath(p), std::ios::binary);
  if (!in) {
    lua_pushnil(L);
    lua_pushfstring(L, "cannot read %s", p);
    return 2;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string data = ss.str();
  lua_pushlstring(L, data.data(), data.size());
  lua_pushinteger(L, static_cast<lua_Integer>(data.size()));
  return 2;
}

int OcEmuHost::luaFsWrite(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  std::size_t len = 0;
  const char* data = luaL_checklstring(L, 2, &len);

  const fs::path full = self(L)->resolvePath(p);
  std::error_code ec;
  if (full.has_parent_path()) fs::create_directories(full.parent_path(), ec);

  std::ofstream out(full, std::ios::binary | std::ios::trunc);
  if (!out) {
    lua_pushboolean(L, false);
    lua_pushfstring(L, "cannot write %s", p);
    return 2;
  }
  out.write(data, static_cast<std::streamsize>(len));
  const bool ok = out.good();
  out.close();
  lua_pushboolean(L, ok);
  if (!ok) lua_pushfstring(L, "short write to %s", p);
  return ok ? 1 : 2;
}

// elsa.filesystem.newFile(path, mode) -> file handle | nil, err
// Delegated to Lua's io so the guest gets a genuine Lua file object.
int OcEmuHost::luaFsNewFile(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  const char* mode = luaL_checkstring(L, 2);
  const fs::path full = self(L)->resolvePath(p);

  std::error_code ec;
  if (full.has_parent_path()) fs::create_directories(full.parent_path(), ec);

  lua_getglobal(L, "io");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    fail(L, "io library unavailable");
  }
  lua_getfield(L, -1, "open");
  lua_remove(L, -2);
  lua_pushstring(L, full.c_str());
  lua_pushstring(L, mode);
  lua_call(L, 2, LUA_MULTRET);
  return 1;
}

// elsa.filesystem.lines(path) -> iterator
int OcEmuHost::luaFsLines(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  const auto full = self(L)->resolvePath(p);
  lua_getglobal(L, "io");
  lua_getfield(L, -1, "lines");
  lua_remove(L, -2);
  lua_pushstring(L, full.c_str());
  lua_call(L, 1, LUA_MULTRET);
  return 1;
}

int OcEmuHost::luaFsGetDirectoryItems(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  const auto full = self(L)->resolvePath(p);

  std::vector<std::string> items;
  std::error_code ec;
  if (fs::is_directory(full, ec)) {
    for (const auto& entry : fs::directory_iterator(full, ec)) {
      items.push_back(entry.path().filename().string());
    }
  }
  std::sort(items.begin(), items.end());

  lua_createtable(L, static_cast<int>(items.size()), 0);
  for (std::size_t i = 0; i < items.size(); ++i) {
    lua_pushlstring(L, items[i].data(), items[i].size());
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
  return 1;
}

int OcEmuHost::luaFsGetLastModified(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  std::error_code ec;
  const auto ft = fs::last_write_time(self(L)->resolvePath(p), ec);
  if (ec) {
    lua_pushnil(L);
    return 1;
  }
  // Convert to a plain integer seconds-since-epoch; OC only uses this for
  // ordering and display.
  const auto sysDur = ft.time_since_epoch();
  lua_pushnumber(L, static_cast<lua_Number>(
                       std::chrono::duration_cast<std::chrono::seconds>(sysDur).count()));
  return 1;
}

int OcEmuHost::luaFsGetSize(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  std::error_code ec;
  const auto sz = fs::file_size(self(L)->resolvePath(p), ec);
  if (ec) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushinteger(L, static_cast<lua_Integer>(sz));
  return 1;
}

// elsa.filesystem.remove(path) -- file or whole directory tree.
int OcEmuHost::luaFsRemove(lua_State* L) {
  const char* p = luaL_checkstring(L, 1);
  const fs::path full = self(L)->resolvePath(p);

  std::error_code ec;
  const fs::file_status st = fs::symlink_status(full, ec);
  if (ec || !fs::exists(st)) {
    lua_pushboolean(L, false);
    return 1;
  }

  auto opts = fs::remove_all(full, ec);
  if (ec) {
    lua_pushboolean(L, false);
    lua_pushstring(L, ec.message().c_str());
    return 2;
  }
  lua_pushboolean(L, opts > 0);
  return 1;
}

int OcEmuHost::luaFsGetSaveDirectory(lua_State* L) {
  lua_pushstring(L, self(L)->machineDir_.c_str());
  return 1;
}

// ---------------------------------------------------------------------------
//  elsa.timer / system / misc
// ---------------------------------------------------------------------------

int OcEmuHost::luaTimerGetTime(lua_State* L) {
  auto* h = self(L);
  if (h == nullptr) {
    lua_pushnumber(L, 0.0);
    return 1;
  }
  // Once the host starts feeding us deltas, report the accumulated virtual time
  // rather than wall-clock.
  //
  // This matters: OCEmu's tick loop compares elsa.timer.getTime() against
  // machine.deadline to decide whether to resume the kernel. With a wall-clock
  // timer, driving the machine faster than real time (or from a headless test)
  // never reaches the deadline, so the guest sits suspended forever.
  if (h->virtualTime_ > 0.0) {
    lua_pushnumber(L, h->virtualTime_);
    return 1;
  }
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  const double t = std::chrono::duration<double>(now).count();
  if (h->startTime_ == 0.0) h->startTime_ = t;
  lua_pushnumber(L, t - h->startTime_);
  return 1;
}

int OcEmuHost::luaSystemGetOS(lua_State* L) {
  lua_pushstring(L, "Linux");
  return 1;
}

int OcEmuHost::luaGetError(lua_State* L) {
  auto* h = self(L);
  lua_pushstring(L, (h != nullptr && !h->lastError_.empty()) ? h->lastError_.c_str() : "");
  return 1;
}

// elsa.update(dt) / elsa.draw() are invoked BY us from the frame loop; OCEmu
// registers handlers on them. These thunks exist so a stray call from Lua does
// not crash.
// Input event stubs: main.lua/OCEmu register handlers on these names. Real
// events are delivered by the C++ side calling the handler list directly.
int OcEmuHost::luaKeyDown(lua_State* L) {
  (void)L;
  return 0;
}
int OcEmuHost::luaKeyUp(lua_State* L) {
  (void)L;
  return 0;
}
int OcEmuHost::luaTextInput(lua_State* L) {
  (void)L;
  return 0;
}
int OcEmuHost::luaWindowClose(lua_State* L) {
  (void)L;
  return 0;
}

// ---------------------------------------------------------------------------
//  elsa's metatable: the mechanism OCEmu uses to register handlers.
//
//  From boot.lua, essentially:
//
//      setmetatable(elsa, {
//        __index    = function(t,k) return function(...) for _,h in ipairs(handlers[k]) do h(...) end end end,
//        __newindex = function(t,k,v) handlers[k] = handlers[k] or {}; table.insert(handlers[k], v) end,
//      })
//
//  That is how components register `elsa.draw = function() ... end`, and how the
//  host dispatches a frame by calling `elsa.draw()`. Reproducing it faithfully
//  is what lets OCEmu's components run unmodified.
// ---------------------------------------------------------------------------

namespace {

// Dispatcher returned by __index for any name that is not a real elsa field:
// calling it invokes every handler registered under that name.
int elsaDispatch(lua_State* L) {
  // Upvalue 1: the event name.
  const char* key = lua_tostring(L, lua_upvalueindex(1));
  if (key == nullptr) return 0;

  // Arguments arrive on the stack; they must sit directly above the function
  // when we call it, so remember the count before touching anything else.
  const int args = lua_gettop(L);

  lua_getglobal(L, "elsa");
  lua_getfield(L, -1, "handlers");
  lua_remove(L, -2);
  lua_getfield(L, -1, key);
  lua_remove(L, -2);  // [args...][list]

  if (!lua_istable(L, -1)) {
    lua_settop(L, 0);
    return 0;
  }

  const lua_Integer n = static_cast<lua_Integer>(luaL_len(L, -1));
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L, -1, i);  // [args...][list][fn]
    lua_remove(L, -2);      // [args...][fn]
    if (lua_isfunction(L, -1)) {
      lua_insert(L, 1);  // [fn][args...] -- function below its arguments
      lua_call(L, args, 0);
    } else {
      lua_pop(L, 1);
    }
  }
  lua_settop(L, 0);
  return 0;
}

// Both metamethods receive the accessed key as argument 2. __index returns a
// dispatcher closure bound to THAT key, because OCEmu's protocol calls the
// result with no arguments (`elsa.draw()`), so the name has to be captured.

// __index: real field, or a fresh dispatcher closure bound to the name.
int elsaMetaIndex(lua_State* L) {
  const char* key = lua_tostring(L, 2);
  if (key != nullptr) lua_pushstring(L, key);
  else lua_pushnil(L);

  lua_pushcclosure(L, &elsaDispatch, 1);
  return 1;
}

// __newindex: append to the handler list for that name.
int elsaMetaNewIndex(lua_State* L) {
  // Entry stack is [table][key][value]. Drop the table and key so that the
  // value sits at a known index and the relative offsets below stay correct.
  const char* key = lua_tostring(L, 2);
  lua_remove(L, 1);  // table
  lua_remove(L, 1);  // key
  // [value]
  if (key == nullptr) return 0;
  constexpr int kValueIdx = 1;

  lua_getglobal(L, "elsa");
  lua_getfield(L, -1, "handlers");
  lua_remove(L, -2);  // [value][handlers]
  if (!lua_istable(L, -1)) {
    lua_settop(L, 0);
    return 0;
  }

  lua_getfield(L, -1, key);  // [value][handlers][list]
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    lua_newtable(L);
  }

  const lua_Integer n = static_cast<lua_Integer>(luaL_len(L, -1));
  lua_pushvalue(L, kValueIdx);
  lua_rawseti(L, -2, n + 1);  // list[#list+1] = value

  lua_setfield(L, -2, key);  // handlers[key] = list  ->  [value][handlers]
  lua_settop(L, 0);
  return 0;
}

// Registers `fn` as `name` on the table currently at the top of the stack.
void regFn(lua_State* L, const char* name, lua_CFunction fn) {
  lua_pushcfunction(L, fn);
  lua_setfield(L, -2, name);
}

}  // namespace

void OcEmuHost::callHandlers(const char* key, bool passDt) {
  if (L_ == nullptr || key == nullptr) return;

  // This runs inside our own C frame (via LuaMachine::guarded), so the stack
  // must be restored to its entry height on the way out -- lua_settop(L, 0)
  // would wipe our frame and corrupt the caller's stack.
  const int base = lua_gettop(L_);

  lua_getglobal(L_, "elsa");
  if (!lua_istable(L_, -1)) { lua_settop(L_, base); return; }
  lua_getfield(L_, -1, "handlers");
  lua_remove(L_, -2);
  if (!lua_istable(L_, -1)) { lua_settop(L_, base); return; }
  lua_getfield(L_, -1, key);
  if (!lua_istable(L_, -1)) { lua_settop(L_, base); return; }

  const lua_Integer n = static_cast<lua_Integer>(luaL_len(L_, -1));
  for (lua_Integer i = 1; i <= n; ++i) {
    lua_rawgeti(L_, -1, i);
    if (lua_isfunction(L_, -1)) {
      if (passDt) lua_pushnumber(L_, lastDt_);
      lua_call(L_, passDt ? 1 : 0, 0);
    } else {
      lua_pop(L_, 1);
    }
  }
  lua_settop(L_, base);
}

std::string OcEmuHost::machineStatus() const {
  if (machine_ == nullptr) return "state=unset";
  return "state=" + std::to_string(static_cast<int>(machine_->state())) +
         " lastError=" + machine_->lastError() +
         " rev=" + std::to_string(OcScreen::active() ? OcScreen::active()->revision() : 0);
}

void OcEmuHost::update(double dtSeconds) {
  ++tickCount_;
  lastDt_ = dtSeconds;
  // Advance the machine's clock before dispatching, so a handler that schedules
  // work relative to "now" sees the new time. Exactly once: adding it twice made
  // the guest's clock run at double speed against its own deadlines.
  if (dtSeconds > 0.0) virtualTime_ += dtSeconds;

  // Neutralise OCEmu's per-tick component call budget.
  //
  // Every cost-accounted component method (filesystem.open/exists/list,
  // gpu.maxResolution, ...) begins with
  //   if not machine.consumeCallBudget(cost) then return end
  // and `return` with NO values, so the guest sees nil rather than an error.
  // main.lua hands out only maxCallBudget = 1.5 units per tick and resets it in
  // elsa.update; a booting OpenOS exhausts that mid-tick, every filesystem read
  // then yields nothing, and package.lua concludes every module is missing.
  //
  // This must be done on the HOST's `machine` table, because OCEmu's component
  // files are compiled without a custom environment and therefore close over the
  // host global -- not the sandbox copy a Lua-side fix would reach.
  // machine.lua still bounds runaway guests with its own wall-clock deadline,
  // and the host gives each frame a bounded number of ticks.
  lua_getglobal(L_, "machine");
  if (lua_istable(L_, -1)) {
    lua_pushnumber(L_, std::numeric_limits<double>::infinity());
    lua_setfield(L_, -2, "callBudget");
    lua_pushcfunction(L_, &OcEmuHost::luaConsumeCallBudget);
    lua_setfield(L_, -2, "consumeCallBudget");
  }
  lua_pop(L_, 1);

  // Dispatch runs guest code, so it must be inside the VM's panic guard -- and
  // must not run at all once the guest has faulted.
  if (liveState() == nullptr) return;
  machine_->guarded([this]() {
    for (int i = 0; i < tickMultiplier_; ++i) callHandlers("update", true);
  });
}

// Always grants the call, and keeps the budget topped up while doing so.
//
// Setting machine.callBudget before dispatch is not enough: main.lua's
// elsa.update assigns `machine.callBudget = maxCallBudget` (1.5) immediately
// before it resumes the guest, overwriting whatever the host set. Every
// cost-accounted component method begins with
//   if not machine.consumeCallBudget(cost) then return end
// and that `return` yields NO values, so the guest sees nil rather than an
// error -- OpenOS's package.lua reads a nil fs.exists as "module missing" and
// the boot dies at the first require(). Refilling here works because
// env.component.invoke calls consumeCallBudget *before* its own
// `if machine.callBudget < 0 then return end` check.
int OcEmuHost::luaConsumeCallBudget(lua_State* L) {
  lua_getglobal(L, "machine");
  if (lua_istable(L, -1)) {
    lua_pushnumber(L, std::numeric_limits<double>::infinity());
    lua_setfield(L, -2, "callBudget");
  }
  lua_pop(L, 1);
  lua_pushboolean(L, 1);
  return 1;
}

void OcEmuHost::setTickMultiplier(int n) {
  tickMultiplier_ = n < 1 ? 1 : (n > 64 ? 64 : n);
}

// Returns the Lua state to drive, or nullptr when the machine is not in a
// condition to be touched.
//
// Once the guest has raised an unrecoverable error, or the allocator gave up,
// the state is discarded and its lua_State must not be used again. Calling into
// it anyway segfaults inside liblua (and leaves freed heap metadata looking
// corrupted), so every host entry point checks here first.
lua_State* OcEmuHost::liveState() const {
  if (machine_ == nullptr) return nullptr;
  if (machine_->state() != MachineState::Running) return nullptr;
  return machine_->L();
}

// Feeds a real key press from SDL into the guest, translating it to the LWJGL
// keycode OC expects. Returns false for keys the guest has no use for.
bool OcEmuHost::pushKey(const std::string& name) {
  const int code = keycodeFromName(name);
  if (code == 0) return false;
  if (liveState() == nullptr) return false;
  bool queued = false;
  machine_->guarded([&]() {
    lua_State* L = machine_->L();
    // charForName only knows the named keys (space, enter, backspace, ...), so
    // it returns 0 for an ordinary letter and the guest would receive a keydown
    // with no character -- which is why only space and enter ever appeared.
    // charForCode covers printable ASCII, whose code IS its character.
    int ch = charForName(name);
    if (ch == 0) ch = charForCode(code);
    queueKeyEvent(L, "key_down", code, ch);
    lua_getglobal(L, "kbdcodes");
    queued = lua_istable(L, -1) && lua_rawlen(L, -1) > 0;
    lua_pop(L, 1);
  });
  return queued;
}

void OcEmuHost::setKeyState(const std::string& name, bool down) {
  const int code = keycodeFromName(name);
  if (code <= 0 || code >= kMaxKeys) return;
  g_keys.down[code] = down;
}

void OcEmuHost::setModifier(const std::string& which, bool down) {
  if (which == "shift") g_keys.shift = down;
  else if (which == "control") g_keys.control = down;
  else if (which == "alt") g_keys.alt = down;
}

void OcEmuHost::draw() {
  if (liveState() == nullptr) return;
  const auto dispatch = [this] { callHandlers("draw", false); };
  machine_->guarded(dispatch);
}

namespace {

// OCEmu's Lua sources require modules that live in the user's luarocks tree
// (lua-utf8 in particular, which apis/unicode.lua and component/gpu.lua both
// need). Our VM's default package.path/cpath does not include it, so extend both
// with the usual per-user and system locations.
void extendPackagePaths(lua_State* L, const std::string& ocsrcDir) {
  std::vector<std::string> luaDirs;
  std::vector<std::string> cDirs;

  // OCEmu's own modules are required by bare name from its components
  // (require("support.serialization")), which resolves against its src/
  // because boot.lua runs with that as the working directory.
  if (!ocsrcDir.empty()) {
    luaDirs.push_back(ocsrcDir + "/?.lua");
    luaDirs.push_back(ocsrcDir + "/?/init.lua");
  }

  if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
    luaDirs.push_back(std::string(home) + "/.luarocks/share/lua/5.2/?.lua");
    luaDirs.push_back(std::string(home) + "/.luarocks/share/lua/5.2/?/init.lua");
    cDirs.push_back(std::string(home) + "/.luarocks/lib/lua/5.2/?.so");
    cDirs.push_back(std::string(home) + "/.luarocks/lib/lua/5.2/?/?.so");
  }
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg != nullptr && *xdg != '\0') {
    luaDirs.push_back(std::string(xdg) + "/lua/5.2/?.lua");
    cDirs.push_back(std::string(xdg) + "/lib/lua/5.2/?.so");
    cDirs.push_back(std::string(xdg) + "/lib/lua/5.2/?/?.so");
  }
  luaDirs.emplace_back("/usr/share/lua/5.2/?.lua");
  luaDirs.emplace_back("/usr/share/lua/5.2/?/init.lua");
  luaDirs.emplace_back("/usr/local/share/lua/5.2/?.lua");
  luaDirs.emplace_back("/usr/local/share/lua/5.2/?/init.lua");
  cDirs.emplace_back("/usr/lib/lua/5.2/?.so");
  cDirs.emplace_back("/usr/lib/lua/5.2/?/?.so");
  cDirs.emplace_back("/usr/local/lib/lua/5.2/?.so");
  cDirs.emplace_back("/usr/local/lib/lua/5.2/?/?.so");

  lua_getglobal(L, "package");
  if (!lua_istable(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  const struct { const char* field; const std::vector<std::string>* dirs; } kTables[] = {
      {"path", &luaDirs},
      {"cpath", &cDirs},
  };

  for (const auto& spec : kTables) {
    lua_getfield(L, -1, spec.field);
    std::string existing = (lua_isstring(L, -1) != 0) ? lua_tostring(L, -1) : "";

    // Our entries go first (in order), Lua's defaults are kept at the end.
    // Each entry already carries its own root, so nothing is inserted between
    // them -- prefixing a "." here would corrupt every absolute path.
    std::string merged;
    for (const std::string& dir : *spec.dirs) {
      if (existing.find(dir) != std::string::npos) continue;
      if (!merged.empty()) merged += ';';
      merged += dir;
    }
    if (!existing.empty()) {
      if (!merged.empty()) merged += ';';
      merged += existing;
    }

    lua_pop(L, 1);
    lua_pushstring(L, merged.c_str());
    lua_setfield(L, -2, spec.field);
  }
  lua_pop(L, 1);
}

}  // namespace

void OcEmuHost::install(lua_State* L, std::string ocsrcDir, std::string machineDir) {
  L_ = L;
  ocsrcDir_ = std::move(ocsrcDir);
  extendPackagePaths(L, ocsrcDir_);
  machineDir_ = std::move(machineDir);
  startTime_ = 0.0;
  virtualTime_ = 0.0;
  lastError_.clear();

  components_ = buildComponentList(3, false);

  // OCEmu's boot.lua runs with its own src/ as the working directory, and
  // main.lua relies on that: `loadfile("biglist.lua")` is a bare path. Match it,
  // since the host process may well have started somewhere else entirely.
  if (!ocsrcDir_.empty()) {
    std::error_code cwdEc;
    fs::current_path(ocsrcDir_, cwdEc);
  }

  // Remember this host so the static thunks can find it.
  lua_pushlightuserdata(L, const_cast<char*>(&kElsaKey));
  lua_pushlightuserdata(L, static_cast<void*>(this));
  lua_rawset(L, LUA_REGISTRYINDEX);

  // --- the elsa table -----------------------------------------------------
  lua_newtable(L);

  lua_newtable(L);  // elsa.handlers
  lua_setfield(L, -2, "handlers");

  lua_newtable(L);  // elsa.filesystem
  regFn(L, "exists", &OcEmuHost::luaFsExists);
  regFn(L, "isDirectory", &OcEmuHost::luaFsIsDirectory);
  regFn(L, "createDirectory", &OcEmuHost::luaFsCreateDirectory);
  regFn(L, "load", &OcEmuHost::luaFsLoad);
  regFn(L, "read", &OcEmuHost::luaFsRead);
  regFn(L, "write", &OcEmuHost::luaFsWrite);
  regFn(L, "newFile", &OcEmuHost::luaFsNewFile);
  regFn(L, "lines", &OcEmuHost::luaFsLines);
  regFn(L, "getDirectoryItems", &OcEmuHost::luaFsGetDirectoryItems);
  regFn(L, "getLastModified", &OcEmuHost::luaFsGetLastModified);
  regFn(L, "getSize", &OcEmuHost::luaFsGetSize);
  regFn(L, "remove", &OcEmuHost::luaFsRemove);
  regFn(L, "getSaveDirectory", &OcEmuHost::luaFsGetSaveDirectory);
  lua_setfield(L, -2, "filesystem");

  lua_newtable(L);  // elsa.timer
  regFn(L, "getTime", &OcEmuHost::luaTimerGetTime);
  lua_setfield(L, -2, "timer");

  lua_newtable(L);  // elsa.system
  regFn(L, "getOS", &OcEmuHost::luaSystemGetOS);
  lua_setfield(L, -2, "system");

  lua_pushcfunction(L, &OcEmuHost::luaGetError);
  lua_setfield(L, -2, "getError");

  // elsa.SDL is set to false, NOT left unset: __index would otherwise return a
  // dispatcher function for it, and `if not machine.beep and elsa.SDL` in
  // main.lua would be truthy and take the FFI path we are avoiding.
  lua_pushboolean(L, 0);
  lua_setfield(L, -2, "SDL");

  // NOTE: update/draw/quit are deliberately NOT real fields. Components assign
  // `elsa.draw = function() ... end`, and that only reaches __newindex (and so
  // the handler list) while no real field shadows the name.

  // args: positional arguments plus an `options` field, mirroring arg_parse.
  lua_newtable(L);
  lua_newtable(L);  // args.options
  lua_setfield(L, -2, "options");
  lua_setfield(L, -2, "args");

  // opts: the parsed option table.
  lua_newtable(L);
  lua_setfield(L, -2, "opts");

  // --- the metatable that makes `elsa.draw = f` register a handler --------
  lua_newtable(L);  // metatable
  lua_pushcfunction(L, &elsaMetaIndex);
  lua_setfield(L, -2, "__index");
  lua_pushcfunction(L, &elsaMetaNewIndex);
  lua_setfield(L, -2, "__newindex");
  lua_setmetatable(L, -2);

  lua_setglobal(L, "elsa");

  // NOTE: elsa.SDL is deliberately left unset. main.lua guards all of its SDL
  // audio behind `if not machine.beep and elsa.SDL`, so an absent SDL table
  // removes luaffifb from the dependency graph instead of forcing us to stub
  // dozens of entry points. machine.beep / machine.sleep are pre-defined below.
}

bool OcEmuHost::isNativeComponent(const std::string& type) {
  return type == kNativeScreen || type == kNativeKeyboard;
}

std::vector<ComponentSpec> OcEmuHost::buildComponentList(int tier, bool internetCard) {
  // Addresses are stable on purpose. They identify the EEPROM and the machine's
  // writable filesystem on disk (~/.local/share/ocemu/<address>), so keeping the
  // ones OCEmu already assigned is what lets us boot the OpenOS install that is
  // already there rather than starting from an empty machine.
  const int depth = kOcTierDepth[std::clamp(tier, 1, 3)];
  const TierSpec spec = tierSpec(std::clamp(tier, 1, 3));

  std::vector<ComponentSpec> out;

  // maxwidth/maxheight/maxtier: OCEmu's gpu and screen constructors take the
  // tier as their third trailing argument, and clamp internally with
  // `tier = math.min(depth, maxtier)`.
  auto add = [&out](const char* type, const char* address, int slot,
                    std::vector<std::string> args) {
    ComponentSpec s;
    s.type = type;
    s.address = address;
    s.slot = slot;
    s.args = std::move(args);
    out.push_back(std::move(s));
  };

  const std::string w = std::to_string(spec.cols);
  const std::string h = std::to_string(spec.rows);
  const std::string t = std::to_string(std::clamp(tier, 1, 3));

  add("gpu", "30d80051-1103-4fb0-9782-f956b3c30ef3", 0, {w, h, t});
  add("modem", "aa39c428-d560-450b-b88c-713a1579f646", 1, {"false"});

  // EEPROM holds the BIOS (lua/bios.lua); its data block records the boot
  // address, which is how the existing machine finds its OpenOS filesystem.
  add("eeprom", "52f69764-fa8b-4158-830e-aec2d1bcb67c", 9, {"lua/bios.lua"});

  // Read-only OpenOS distribution, writable machine (labelled "OpenOS"), and
  // tmpfs. The writable one is our existing install.
  add("filesystem", "f57aa5ca-dafa-4dbd-a147-f89bc1eee113", 7,
      {"loot/openos", "openos", "true", "1"});
  add("filesystem", "058f63e0-8939-490f-a096-ab41858d556b", 5,
      {"nil", "OpenOS", "false", "4"});
  add("filesystem", "e57878df-048c-4312-a6e6-d46da38be998", -1,
      {"tmpfs", "tmpfs", "false", "5"});

  if (internetCard) add("internet", "06cd4b0a-5a83-41f6-9af2-630d7d4da583", 2, {});

  add("computer", "dc096366-4201-47c3-a622-051eaf5a89b5", -1, {});
  add("ocemu", "28d4c083-57de-47c1-b4dd-ce4e2b74d108", -1, {});

  // Our native replacements. The screen also takes a fourth trailing value, the
  // depth the panel is currently configured for.
  add(kNativeScreen, "4c2d90a3-0b0a-46dc-ba34-2b13a9fd1b27", -1,
      {w, h, t, std::to_string(depth)});
  add(kNativeKeyboard, "328ae86a-6f6d-4376-bd28-2ddb9f99e9eb", -1, {});

  return out;
}

std::string OcEmuHost::renderComponentList(const std::vector<ComponentSpec>& list) const {
  std::string out;
  for (const ComponentSpec& s : list) {
    out += "      {\"" + s.type + "\", ";
    // Parenthesised: += binds tighter than ?:.
    out += (s.address.empty() ? std::string("nil") : ("\"" + s.address + "\""));
    out += ", " + std::to_string(s.slot);
    for (const std::string& a : s.args) {
      out += ", ";
      // OCEmu's config.lua evaluates these with load(), so a bare nil/true/
      // false is meaningful and a number or string otherwise.
      if (a == "nil") out += "nil";
      else if (a == "true" || a == "false") out += a;
      else {
        char* end = nullptr;
        (void)std::strtol(a.c_str(), &end, 10);
        if (end != nullptr && *end == '\0') out += a;
        else out += "\"" + a + "\"";
      }
    }
    out += "},\n";
  }
  return out;
}

bool OcEmuHost::syncOcEmuConfig(const std::string& dataDir, int tier, bool internetCard,
                                 std::string* error) {
  components_ = buildComponentList(tier, internetCard);
  if (!dataDir.empty()) machineDir_ = dataDir;

  const fs::path cfgPath = fs::path(dataDir) / "ocemu.cfg";
  std::error_code ec;
  if (!fs::exists(cfgPath, ec)) {
    // No config yet: OCEmu will create one with its own defaults on first run,
    // and our list is applied on the following run. Nothing to do.
    return true;
  }

  std::ifstream in(cfgPath, std::ios::binary);
  if (!in) {
    if (error != nullptr) *error = "cannot read " + cfgPath.string();
    return false;
  }
  std::ostringstream ss;
  ss << in.rdbuf();
  const std::string original = ss.str();
  in.close();

  // Locate the components block and its closing brace (tracking depth so a
  // brace inside the block cannot end it early).
  const std::size_t key = original.find("components");
  if (key == std::string::npos) {
    if (error != nullptr) *error = "no components block in " + cfgPath.string();
    return false;
  }
  std::size_t open = original.find('{', key);
  if (open == std::string::npos) {
    if (error != nullptr) *error = "malformed components block in " + cfgPath.string();
    return false;
  }
  int depth = 0;
  std::size_t close = std::string::npos;
  for (std::size_t i = open; i < original.size(); ++i) {
    if (original[i] == '{') ++depth;
    else if (original[i] == '}') {
      if (--depth == 0) { close = i; break; }
    }
  }
  if (close == std::string::npos) {
    if (error != nullptr) *error = "unterminated components block in " + cfgPath.string();
    return false;
  }

  std::string updated = original;
  // The replacement must supply BOTH braces: `close` is inside the span being
  // replaced, so omitting it would strip the block's terminator.
  updated.replace(open, close - open + 1,
                  "{\n" + renderComponentList(components_) + "    }");

  const fs::path tmp = cfgPath.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
      if (error != nullptr) *error = "cannot write " + tmp.string();
      return false;
    }
    out << updated;
    if (!out) {
      if (error != nullptr) *error = "short write to " + tmp.string();
      return false;
    }
  }
  fs::rename(tmp, cfgPath, ec);
  if (ec) {
    std::error_code ignored;
    fs::copy_file(tmp, cfgPath, fs::copy_options::overwrite_existing, ignored);
    fs::remove(tmp, ignored);
    if (ignored) {
      if (error != nullptr) *error = "cannot replace " + cfgPath.string();
      return false;
    }
  }
  return true;
}

void OcEmuHost::pushComponentList(lua_State* L) const {
  lua_createtable(L, static_cast<int>(components_.size()), 0);
  for (std::size_t i = 0; i < components_.size(); ++i) {
    const ComponentSpec& s = components_[i];
    lua_createtable(L, 0, static_cast<int>(s.args.size()) + 3);

    lua_pushstring(L, s.type.c_str());
    lua_rawseti(L, -2, 1);

    // OCEmu's convention: a nil address means "derive one from the slot".
    if (s.address.empty()) {
      lua_pushnil(L);
    } else {
      lua_pushstring(L, s.address.c_str());
    }
    lua_rawseti(L, -2, 2);

    lua_pushinteger(L, s.slot);
    lua_rawseti(L, -2, 3);

    for (std::size_t a = 0; a < s.args.size(); ++a) {
      // The literal string "nil" means a Lua nil, which OCEmu's component
      // constructors use to mean "no directory" for the writable filesystem.
      if (s.args[a] == "nil") {
        lua_pushnil(L);
      } else if (s.args[a] == "true" || s.args[a] == "false") {
        lua_pushboolean(L, s.args[a] == "true");
      } else {
        const long v = std::strtol(s.args[a].c_str(), nullptr, 10);
        if (std::to_string(v) == s.args[a]) {
          lua_pushinteger(L, v);
        } else {
          lua_pushstring(L, s.args[a].c_str());
        }
      }
      lua_rawseti(L, -2, static_cast<lua_Integer>(a + 4));
    }
    lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
  }
}

}  // namespace ocemu