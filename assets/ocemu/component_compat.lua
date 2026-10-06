--[[=========================================================================
  component_compat.lua -- completes the OpenComputers `component` API.

  WHY THIS EXISTS
  ---------------
  OCEmu's apis/component.lua exposes only:

      component.list / type / slot / methods / invoke / doc

  Real OpenComputers additionally provides:

      component.get / get_or_nil / proxy / wrap / is / set
      component.cecinvoke
      and the per-type convenience fields (component.screen, component.ram,
      component.keyboard, component.eeprom, component.internet, ...)

  OpenOS uses component.is (7x), component.proxy (4x) and component.get (2x),
  so without this the guest simply cannot boot. This module adds exactly those,
  as a thin layer over the invoke/list pair OCEmu does export -- no OCEmu file
  is modified.

  It is installed by the host after main.lua has run, and is handed the GUEST's
  environment table (the same one apis/component.lua populated), because
  `component` only exists inside the sandboxed machine environment.
=========================================================================]]

return function(env)
  local component = env and env.component
  if not component then return "no component table" end
  if component.get then return "already complete" end

  local invoke = component.invoke
  local list = component.list

  -- OCEmu's component.invoke ends with:
  --
  --     local results = table.pack(pcall(proxylist[address][method], ...))
  --     return table.unpack(results, 1, results.n)
  --
  -- so it returns the pcall STATUS as its first value. That is deliberate
  -- INSIDE OCEmu: machine.lua's processResult() and bios.lua both strip it with
  -- `table.unpack(result, 2, result.n)`, so the emulator is self-consistent. It
  -- is only wrong from a GUEST's point of view, because real OpenComputers
  -- returns just the method's own values.
  --
  -- Therefore the two audiences need different behaviour:
  --   * component.invoke  -- leave exactly as OCEmu provides it, or machine.lua
  --                         stops working (it strips index 1 itself).
  --   * what the GUEST sees -- strip the status, so OCOS gets real OpenComputers
  --     (get/proxy/is/wrap/  semantics.
  --     cecinvoke)
  --
  -- OCEmu also keeps cecinvoke private to apis/component.lua, but gpu.lua calls
  -- component.cecinvoke when it blits to a bound screen.
  local function stripStatus(...)
    local r = table.pack(...)
    if r.n >= 1 then
      if r[1] == true then
        return table.unpack(r, 2, r.n)
      elseif r[1] == false then
        return nil, r[2]
      end
    end
    return table.unpack(r, 1, r.n)
  end

  -- Builds the proxy object OC's component.proxy is documented to return: a
  -- table you index to get bound methods. Each entry is the same closure
  -- component.get(address, key) would hand back, so both entry points behave
  -- identically.
  local function newProxy(address)
    return setmetatable({}, {
      __index = function(_, key)
        -- `address` and `type` are DATA on an OC component proxy, not methods.
        -- Returning a closure for them makes `gpu.bind(screen.address)` pass a
        -- function where an address string is required -- which is what
        -- boot/91_gpu.lua does when it binds the tty to the GPU.
        if key == "address" then return address end
        if key == "type" then return component.type(address) end
        return component.get(address, key)
      end,
    })
  end

  if not component.cecinvoke then
    component.cecinvoke = function(address, method, ...)
      if not invoke then return nil, "component API unavailable" end
      return stripStatus(invoke(address, method, ...))
    end
  end

  -------------------------------------------------------------- computer -----
  --
  -- OCEmu's apis/computer.lua omits these, but OpenOS's init.lua calls
  -- getBootAddress() on its very first line, so without them OpenOS cannot even
  -- start. getBootAddress answers the address of the filesystem the machine
  -- booted from, which is what OC returns: the first filesystem component.

  if env.computer then
    if not env.computer.getBootAddress then
      env.computer.getBootAddress = function()
        -- OC returns the filesystem the computer actually booted from. For
        -- OpenOS that is the WRITABLE one -- it is where /init.lua and the
        -- installed library live. Returning whichever entry pairs() happens to
        -- visit first picks the read-only bundled copy instead, and the guest
        -- then cannot find its own modules.
        local list = component.list("filesystem")
        local fallback
        if type(list) == "table" then
          for address in pairs(list) do
            if fallback == nil then fallback = address end
            local ok, fs = pcall(component.proxy, address)
            if ok and fs then
              -- The filesystem the machine booted from is the one OpenOS is
              -- installed on, which is exactly the one holding /init.lua. Probe
              -- for that rather than trusting isReadOnly, which does not report
              -- through this host.
              -- Probe with open(), which is known to report a handle here,
              -- rather than exists().
              local probed, handle = pcall(fs.open, "/init.lua", "r")
              if probed and handle then
                pcall(fs.close, handle)
                return address
              end
            end
          end
          return fallback
        elseif type(list) == "function" then
          local only = list()
          if type(only) == "string" then return only end
        end
        return nil
      end
    end
    if not env.computer.isRobot then
      env.computer.isRobot = function() return false end
    end
    if not env.computer.getComputerLabel then
      env.computer.getComputerLabel = function() return "ocemu" end
    end
    if not env.computer.setComputerLabel then
      env.computer.setComputerLabel = function() return true end
    end
    if not env.computer.getComputerUUID then
      env.computer.getComputerUUID = function()
        return "00000000-0000-0000-0000-000000000000"
      end
    end
  end

  ------------------------------------------------------------------- get -----

  function component.get_or_nil(address, key)
    if type(address) ~= "string" then return nil, "address must be a string" end
    local t = component.type(address)
    if not t then return nil end
    if not key then return newProxy(address) end
    if key == "type" then return t end
    if key == "slot" then return component.slot(address) end

    -- OCEmu's filesystem.exists/isReadOnly return NO values through this host
    -- (the component's own method yields nothing), and stripStatus turns a
    -- zero-value result into no result at all. OpenOS's package.lua calls
    -- fs.exists once per candidate path and treats "no value" as "missing", so
    -- every require fails and OpenOS cannot boot. open() does work, so answer
    -- exists() by attempting an open and closing it again.
    -- OCEmu's filesystem.lua does `size = size or math.huge`, so a volume with no
    -- explicit capacity reports spaceTotal() == math.huge. No real drive is
    -- infinite, and installers depend on this number: they choose an install
    -- target with `if proxy.spaceTotal() >= 2 * 1024 * 1024 then ... break end`,
    -- which will happily pick a volume that cannot be written to and then fail
    -- every open(..., "wb") with "File opening failed".
    --
    -- Only substitute when the component really does answer with infinity, so a
    -- properly sized volume keeps its own number.
    if t == "filesystem" and key == "spaceTotal" then
      local kUnknownCapacity = 64 * 1024 * 1024
      return function()
        local total = stripStatus(invoke(address, "spaceTotal"))
        if type(total) ~= "number" or total == math.huge or total <= 0 then
          total = kUnknownCapacity
        end
        -- Deliberately NOT keyed on isReadOnly: OCEmu's settings.lua has a
        -- one-time config migration that copies the *label* into the readonly
        -- field (v[6] = v[5]), so isReadOnly reports true for volumes that are
        -- perfectly writable, and false for ones that are not. Keying off it
        -- would clamp every volume. The root cause is fixed where it belongs,
        -- in component/ocemu.lua; this is only a floor for unknown sizes.
        return total
      end
    end

    if t == "filesystem" and (key == "exists" or key == "isReadOnly") then
      local function fsOpen(path, mode)
        local handle = invoke(address, "open", path, mode or "r")
        return handle
      end
      if key == "exists" then
        return function(path)
          if type(path) ~= "string" then return nil, "bad argument #1 (string expected)" end
          local handle = fsOpen(path, "r")
          if handle then
            invoke(address, "close", handle)
            return true
          end
          -- Directories cannot be opened for reading, so fall back to list().
          local entries = invoke(address, "list", path)
          if type(entries) == "table" then return true end
          return false
        end
      end
      -- Must NOT go through stripStatus: it treats a lone `true` as a status and
      -- drops it, which is exactly why this always used to answer nothing and we
      -- hardcoded false. Report whatever the component actually says.
      return function()
        local ok, ro = pcall(invoke, address, "isReadOnly")
        if not ok or ro == nil then return false end
        return ro and true or false
      end
    end
    local fn = function(...) return stripStatus(invoke(address, key, ...)) end
    return fn
  end

  function component.get(address, key)
    local v, err = component.get_or_nil(address, key)
    if v == nil then
      error(err or ("no such component: " .. tostring(address)), 2)
    end
    return v
  end

  function component.proxy(address)
    return newProxy(address)
  end

  -- component.wrap(fn) returns a function that invokes fn through the component
  -- at the given address; used by Lua-side drivers.
  function component.wrap(address, fn)
    if type(fn) == "string" then fn = fn end
    return function(...) return stripStatus(invoke(address, fn, ...)) end
  end

  ------------------------------------------------------------------- is -----

  -- component.is("screen")      -> true when at least one such component exists
  -- component.is("screen", addr) -> whether that address is of that type
  function component.is(filter, address)
    if address then
      local t = component.type(address)
      if not t then return false end
      if filter == nil then return true end
      return t == filter
    end
    for _, addr in pairs(list(filter)) do
      if addr then return true end
    end
    return false
  end

  ------------------------------------------------------------- setProxy -----

  local proxyOverrides = {}
  function component.setProxy(address, proxy)
    proxyOverrides[address] = proxy
  end
  function component.unproxy(address)
    proxyOverrides[address] = nil
  end

  ------------------------------------------- per-type convenience accessors --

  -- component.screen, component.ram, ... resolve to the first component of that
  -- type. They are dynamic: a component can appear or disappear at runtime, so
  -- these are resolved on access rather than captured once.
  local types = {
    "screen", "gpu", "cpu", "ram", "rom", "nvram", "keyboard", "eeprom",
    "internet", "rednet", "gps", "robot", "modem", "tunnel", "analog", "rs",
    "printer", "filesystem", "drive", "disk", "button", "led", "speaker",
    "piezo", "servo", "radio", "wlan", "serial", "parallel", "craft", "minecraft",
    "generator", "pony", "chest", "crt", "dbus", "energy", "mfm", "firwm",
    "hologram", "multinode", "nbt", "network", "podpony", "switch", "trident",
  }

  -- Resolves a type filter to a single component address.
  --
  -- OCEmu's component.list(filter) does NOT return an address: it returns a
  -- CALLABLE TABLE that yields one address when called, and which is also a map
  -- of address -> type. Treating its result as an address produced a proxy built
  -- around a table, so component.gpu and friends were silently broken -- which is
  -- why OpenOS's tty never received a GPU and tty.stream:write returned on its
  -- first line without drawing anything.
  local function firstAddress(filter)
    local ok, l = pcall(list, filter)
    if not ok or l == nil then return nil end
    if type(l) == "function" then
      local okCall, addr = pcall(l)
      if okCall and type(addr) == "string" then return addr end
      return nil
    end
    if type(l) == "table" then
      for addr in pairs(l) do
        if type(addr) == "string" then return addr end
      end
    end
    return nil
  end

  setmetatable(component, {
    __index = function(_, key)
      -- `list`, `get`, `type`, ... are real fields and never reach here.
      for _, t in ipairs(types) do
        if t == key then
          local addr = firstAddress(key)
          if addr then return newProxy(addr) end
          return nil
        end
      end
      return nil
    end,
  })

  -- Guest tracebacks.
  --
  -- machine.lua drives the guest through coroutine.resume, which reports only
  -- the error message. Appending a traceback here means a guest crash tells us
  -- WHERE it happened, not just what it said.
  if env.coroutine and type(env.coroutine.resume) == "function" then
    local rawResume = env.coroutine.resume
    env.coroutine.resume = function(co, ...)
      local r = table.pack(rawResume(co, ...))
      if r[1] == false and type(r[2]) == "string" and not r[2]:find("stack traceback") then
        local okTb, tb = pcall(debug.traceback, co, "", 2)
        if okTb and tb then r[2] = r[2] .. "\n" .. tb end
      end
      return table.unpack(r, 1, r.n)
    end
  end

  return "installed"
end