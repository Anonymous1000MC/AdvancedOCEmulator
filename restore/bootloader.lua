--[[=========================================================================
  OpenOS compatibility preamble.

  OCEmu's apis/component.lua exposes only list/type/slot/methods/invoke/doc in
  the sandbox, and its computer API omits getBootAddress. OpenOS needs the rest
  of the documented component API plus a few computer calls, so install them
  here, in the environment the bootloader itself runs in.

  Two OCEmu-specific details this respects:

  * component.invoke returns the method's results DIRECTLY -- there is no leading
    pcall status to strip. Treating result 1 as a status made every call whose
    first result was not literally `true` (a file handle, a resolution, a string)
    come back nil.
  * Most OCEmu components return `obj, nil, mai, di`, i.e. they expose no `cec`
    table, so the global component.cecinvoke is a dead end for them. The shim
    prefers it (it is the uncosted path OCEmu's own gpu.lua uses) and falls back
    to invoke for everything else.
=========================================================================]]

local _component = component
local _computer = computer
local _type = type(component) == "table" and component.type or nil

local _origInvoke = _component.invoke

local _cecinvoke
if type(_G) == "table" and type(_G.component) == "table" and _G.component.cecinvoke then
  _cecinvoke = _G.component.cecinvoke
elseif _component.cecinvoke then
  _cecinvoke = _component.cecinvoke
end

local function _invoke(address, method, ...)
  if _cecinvoke then
    local r = table.pack(_cecinvoke(address, method, ...))
    for i = 1, r.n do
      if r[i] ~= nil then return table.unpack(r, i, r.n) end
    end
  end
  local r = table.pack(_origInvoke(address, method, ...))
  return table.unpack(r, 1, r.n)
end

local function _proxy(address)
  return setmetatable({}, {
    __index = function(_, key)
      -- `address` and `type` are DATA on an OC component proxy, not methods.
      -- Returning a closure for them makes boot/91_gpu.lua's
      -- `gpu.bind(screen.address)` pass a function where a string is required.
      if key == "address" then return address end
      if key == "type" then return _type and _type(address) or nil end
      if key == "slot" then return _component.slot(address) end
      -- OCEmu's filesystem.exists returns no values through this host, and a
      -- zero-value result reaches the caller as nothing at all. OpenOS's
      -- package.lua calls fs.exists once per candidate path and reads "no
      -- value" as "missing", so every require() would fail. open() works, so
      -- answer exists() by attempting an open.
      if key == "exists" and _type and _type(address) == "filesystem" then
        return function(path)
          if type(path) ~= "string" then return false end
          local handle = _invoke(address, "open", path, "r")
          if type(handle) == "number" then
            _invoke(address, "close", handle)
            return true
          end
          if type(_invoke(address, "list", path)) == "table" then return true end
          return false
        end
      end
      return function(...) return _invoke(address, key, ...) end
    end,
  })
end

_component.proxy = _proxy

_component.get_or_nil = function(address, key)
  if type(address) ~= "string" then return nil, "address must be a string" end
  if not _type or not _type(address) then return nil, "no such component" end
  if not key then return _proxy(address) end
  return _proxy(address)[key]
end

function _component.get(address, key)
  if type(address) ~= "string" then error("address must be a string", 2) end
  if not _type or not _type(address) then
    error("no such component: " .. tostring(address), 2)
  end
  return _proxy(address)[key]
end

function _component.cecinvoke(address, method, ...)
  return _invoke(address, method, ...)
end

function _component.is(address, method)
  return type(_proxy(address)[method]) == "function"
end

function _component.wrap(address, fn)
  return function(...) return _invoke(address, fn, ...) end
end

-- OpenOS boots with `component.invoke(addr, "open", file)` captured directly,
-- and OCEmu's invoke is cost-accounted, so route it through the proxy.
_component.invoke = function(address, method, ...)
  local m = _proxy(address)[method]
  if type(m) ~= "function" then return nil, "no such method" end
  return m(...)
end

-- Availability. boot/91_gpu.lua's component_available handler starts with
-- component.isAvailable("gpu"); without it the handler raises, tty.bind(gpu)
-- never runs, tty.window.gpu stays nil, and every tty draw silently no-ops --
-- leaving a live shell with a blinking cursor and no prompt.
function _component.isAvailable(filter)
  if filter == nil then
    return next(_component.list() or {}) ~= nil
  end
  local l = _component.list(filter)
  if l == nil then return false end
  if type(l) == "function" then
    local ok, addr = pcall(l)
    return ok and addr ~= nil
  end
  if type(l) == "table" then return next(l) ~= nil end
  return false
end

function _component.inAvailable(address)
  if type(address) ~= "string" then return false end
  return _type ~= nil and _type(address) ~= nil
end

-- computer.* that OCEmu omits. OpenOS's init.lua calls getBootAddress() on its
-- second line, so without these OpenOS cannot start at all. The boot address is
-- the filesystem holding /init.lua, which is where OpenOS is installed.
function _computer.getBootAddress()
  local best
  for address in pairs(_component.list("filesystem") or {}) do
    if not best then best = address end
    local fs = _proxy(address)
    local handle = fs.open("/init.lua", "r")
    if type(handle) == "number" then
      fs.close(handle)
      return address
    end
  end
  return best
end

function _computer.isRobot() return false end
function _computer.getComputerLabel() return "ocemu" end
function _computer.setComputerLabel() return true end
function _computer.getComputerUUID() return "00000000-0000-0000-0000-000000000000" end

do
  local addr, invoke = computer.getBootAddress(), component.invoke
  local function loadfile(file)
    local handle = assert(invoke(addr, "open", file))
    local buffer = ""
    repeat
      local data = invoke(addr, "read", handle, math.maxinteger or math.huge)
      buffer = buffer .. (data or "")
    until not data
    invoke(addr, "close", handle)
    return load(buffer, "=" .. file, "bt", _G)
  end
  loadfile("/lib/core/boot.lua")(loadfile)
end

while true do
  local result, reason = xpcall(require("shell").getShell(), function(msg)
    return tostring(msg).."\n"..debug.traceback()
  end)
  if not result then
    io.stderr:write((reason ~= nil and tostring(reason) or "unknown error") .. "\n")
    io.write("Press any key to continue.\n")
    os.sleep(0.5)
    require("event").pull("key")
  end
end
