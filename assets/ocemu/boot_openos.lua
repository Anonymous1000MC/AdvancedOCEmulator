--[[=========================================================================
  boot_openos.lua -- host bootstrap for running an OpenComputers OS.

  Runs inside OUR embedded Lua VM (not the guest sandbox). Its job is to start
  OCEmu's emulator core and then complete the OpenComputers component API before
  any guest code executes.

  WHY THE env CAPTURE
  ------------------
  OCEmu's main.lua builds the machine's sandbox environment as a *local* and
  loads the guest kernel into it:

      local machine_fn = load(machine_data, "=machine", "t", env)

  That `env` is the only place `component` lives, and main.lua keeps no
  reference we can reach from outside -- Lua 5.2's debug.getinfo does not accept
  a thread, so the usual coroutine trick is unavailable.

  Wrapping the global `load` is the reliable way in: the one and only call that
  passes an environment is that kernel load, so capturing the argument hands us
  the sandbox. It happens BEFORE the kernel coroutine is resumed, which means
  the component API is complete before OpenOS runs a single line.
=========================================================================]]

local elsa = _G.elsa
if not elsa then error("elsa is not installed") end

local compatPath = _G.__ocemuCompatPath
local function log(msg) if host and host.log then host.log(msg) end end

-- --------------------------------------------------------- capture the env --
-- OCEmu compiles the guest kernel AND the BIOS (the EEPROM's code.lua) into
-- separate sandbox environments, and each is handed to `load` in turn. The
-- BIOS env is seen FIRST and the guest env later, so keeping only the last
-- capture left the BIOS running without the compatibility shim -- which is why
-- OpenOS's own init.lua saw a nil computer.getBootAddress. Collect them all
-- and shim every one.
local capturedEnv = nil
local capturedEnvs = {}

-- Load the shim module up front: main() boots the machine (and therefore runs
-- the EEPROM's BIOS) before it returns, so the shim has to be ready to apply
-- the instant an environment shows up rather than afterwards.
local compatModule
if compatPath then
  local chunk, lerr = loadfile(compatPath)
  if chunk then
    local mod = chunk()
    if type(mod) == "function" then compatModule = mod end
  end
  if not compatModule then
    log("component-compat: cannot load " .. tostring(compatPath))
  end
end

local function rememberEnv(env)
  if type(env) ~= "table" or env.component == nil then return end
  if not capturedEnvs[env] then
    capturedEnvs[env] = true
    capturedEnvs[#capturedEnvs + 1] = env
  end
  capturedEnv = env
  -- Apply immediately. main() compiles and runs the BIOS through this very
  -- wrapper, so anything deferred until after main() returns is too late.
  if compatModule and not env.__ocemuCompatInstalled then
    env.__ocemuCompatInstalled = true
    local ok, res = pcall(compatModule, env)
    if not ok then
      env.__ocemuCompatInstalled = nil
      log("component-compat: failed on env: " .. tostring(res))
    end
  end

  -- Hand the sandbox OCEmu's GLOBAL component.cecinvoke.
  --
  -- env.component only carries list/type/slot/methods/invoke, and its invoke is
  -- cost-accounted: it returns NO values once the per-tick call budget is spent,
  -- and its direct path evaluates 1/limit where `limit` is frequently unset or
  -- the string "inf". The global cecinvoke calls emuicc[address][method]
  -- directly with neither problem -- it is the path OCEmu's own gpu.lua uses.
  -- Without it a filesystem read yields nothing and OpenOS cannot load a single
  -- module. Injected from the host state, where that global actually exists.
  if not env.__ocemuCecInvoke then
    local top = rawget(_G, "component")
    if type(top) == "table" then
      local cci = rawget(top, "cecinvoke")
      if type(cci) == "function" then
        env.__ocemuCecInvoke = cci
      end
    end
  end

  -- Publish the environment that is actually the guest's, not merely the last
  -- one seen. Several sandboxes exist (the kernel's, and the BIOS's); only the
  -- one carrying the guest runtime has dofile/require/package, and host-side
  -- probes against the wrong one report missing functions that are actually
  -- present. Prefer an env that looks like the guest runtime.
  -- Indexed normally, not rawget: the sandbox exposes dofile/require through a
  -- metatable __index, so rawget would report them missing on every env.
  if env.dofile ~= nil and env.require ~= nil then
    _G.__ocemuGuestEnv = env
  end

  -- Make the sandbox refer to itself as _G.
  --
  -- OpenOS's kernel does `_G.package = package` (lib/core/boot.lua) to install
  -- its own package library, which is what makes require("shell") and friends
  -- resolve to /lib/*.lua instead of plain Lua's ./?.lua search. Without this,
  -- `_G` resolves to the HOST table, that assignment lands outside the sandbox,
  -- require stays vanilla, /etc/profile.lua dies on its first require, sh.lua
  -- errors, and init.lua spins forever swallowing keystrokes with no prompt.
  -- Real OpenComputers sets _G to the sandbox; OCEmu does not.
  if rawget(env, "_G") ~= env then
    rawset(env, "_G", env)
  end

  -- The BIOS (the EEPROM's code.lua) is compiled from INSIDE the sandbox by
  -- machine.lua, so its `load` resolves through this environment rather than
  -- through the host global wrapped above. Wrap the environment's own load too,
  -- otherwise the BIOS environment is never captured and never shimmed -- which
  -- is why OpenOS's init.lua saw a nil computer.getBootAddress.
  if compatModule and not rawget(env, "__ocemuLoadWrapped") then
    local inner = rawget(env, "load") or load
    rawset(env, "__ocemuLoadWrapped", true)
    env.load = function(chunk, chunkname, mode, nested)
      local fn, err = inner(chunk, chunkname, mode, nested)
      if fn and type(nested) == "table" and nested.component ~= nil then
        rememberEnv(nested)
      end
      return fn, err
    end
  end
end
local realLoad = load

load = function(chunk, chunkname, mode, env)
  local fn, err = realLoad(chunk, chunkname, mode, env)
  -- Only the kernel load supplies an environment table; env.component has
  -- already been populated by then, which keeps us from grabbing an unrelated
  -- table.
  rememberEnv(env)
  return fn, err
end

local ok, err = xpcall(function()
  local main = elsa.filesystem.load("main.lua")
  if not main then
    error("could not load main.lua: " .. tostring(select(2, elsa.filesystem.load("main.lua"))), 0)
  end
  main()
end, function(m)
  return tostring(m) .. "\n" .. debug.traceback("", 2)
end)

load = realLoad

if not ok then
  error("OCEmu main.lua failed: " .. tostring(err), 0)
end

-- ------------------------------------------------- complete the component API --
-- Normally already applied by rememberEnv(); this catches any environment that
-- appeared without passing through the load wrapper.
if #capturedEnvs == 0 then
  log("component-compat: SKIPPED (no guest environment was captured)")
elseif compatModule then
  local installed = 0
  for _, env in ipairs(capturedEnvs) do
    if env.__ocemuCompatInstalled then
      installed = installed + 1
    else
      env.__ocemuCompatInstalled = true
      local ok, res = pcall(compatModule, env)
      if not ok then
        env.__ocemuCompatInstalled = nil
        log("component-compat: failed on env: " .. tostring(res))
      else
        installed = installed + 1
      end
    end
  end
  log("component-compat: installed into " .. installed .. "/" .. #capturedEnvs .. " env(s)")
end

-- NOTE: do NOT bridge env.component into _G.
--
-- apis/component.lua assigns a GLOBAL `component` table (line 19: `component =
-- {}`) which carries connect/disconnect/cecinvoke, and separately builds the
-- guest-facing `env.component`. Components are compiled WITHOUT a custom
-- environment, so gpu.lua/ocemu.lua resolve `component` against that global --
-- overwriting it with env.component would strip connect() and disconnect() and
-- break them.

-- Expose the guest environment for host-side inspection and debugging.
_G.__ocemuGuestEnv = capturedEnv

return true