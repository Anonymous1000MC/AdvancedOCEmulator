--[[=========================================================================
  ocemu boot ROM

  Exercises the whole component surface the emulator exposes:

    * the tier intersection (GPU AND screen both have to grant it)
    * resolution clamping -- we deliberately ask for more than we have
    * the custom RAM allocator's live accounting
    * the asynchronous internet card, polled chunk by chunk

  It also installs host.onTick so the host has something to call each frame.
=========================================================================]]

local gpu_tier, screen_tier, ram_kb, internet_enabled = host.tiers()

-- --------------------------------------------------------------- helpers ---

local function fmtBytes(n)
  if n < 0 then return "infinite" end
  if n >= 1048576 then return string.format("%.1f MB", n / 1048576) end
  if n >= 1024 then return string.format("%.1f KB", n / 1024) end
  return string.format("%d B", n)
end

local function row(n)
  local cols = screen.getResolution()
  screen.write(string.rep("-", cols))
end

local function field(label, value)
  screen.write(string.format(" %-9s%s", label .. " ", value))
end

-- --------------------------------------------------------- boot sequence ---

screen.clear()
screen.setColor(0x7FE0FF, 0x0A0A12)

local cols, rows, depth = screen.getResolution()
local limCols, limRows, limDepth = screen.getLimits()

screen.write("\xC9")                      -- box drawing: top-left corner
screen.write(string.rep("\xCD", cols - 2))
screen.write("\xBB\n")
screen.write("\xBA " .. " OpenComputers Emulator " .. string.rep(" ", math.max(1, cols - 28)))
screen.write("\xBA\n")
screen.write("\xC8" .. string.rep("\xCD", cols - 2) .. "\xBC\n")

field("GPU", string.format("Tier %d  (standalone card)", gpu_tier))
field("Screen", string.format("Tier %d  (panel)", screen_tier))

-- The intersection of the two tiers is the real hardware limit.
field("Granted", string.format("%d x %d cells, %d-bit colour", limCols, limRows, limDepth))
field("Buffer", string.format("%d x %d cells, %d-bit colour", cols, rows, depth))

-- Ask for more than the tiers allow and report what we were actually given.
local wantCols, wantRows = math.max(200, limCols), math.max(60, limRows)
local exact, gotCols, gotRows, gotDepth = screen.requestResolution(wantCols, wantRows, 8)
if exact then
  field("Request", string.format("%d x %d honoured exactly", gotCols, gotRows))
else
  field("Request", string.format("%d x %d -> clamped to %d x %d @ %d-bit",
                                wantCols, wantRows, gotCols, gotRows, gotDepth))
end

field("RAM", string.format("%s budget", ram_kb < 0 and "unlimited" or
                           (ram_kb .. " KB")))
field("Used", fmtBytes(component.ram.used()))
if component.ram.isInfinite() then
  field("Cap", "none (infinite memory active)")
else
  field("Cap", fmtBytes(component.ram.total()))
end

field("Net", internet_enabled and "internet card installed"
                            or "no internet card (enable it in Component Manager)")

row()

-- ------------------------------------------------- asynchronous net demo ---

local demo = {
  handle = nil,
  chunks = 0,
  bytes = 0,
  state = "idle",
  detail = "",
  done = false,
}

local function startDemo()
  if not component.internet then return end

  local handle, err = component.internet.request(
    "https://raw.githubusercontent.com/ocornut/imgui/master/LICENSE.txt")
  if not handle then
    demo.state = "start-failed"
    demo.detail = err or "unknown"
    return
  end

  demo.handle = handle
  demo.state = "connecting"
  demo.done = false
end

local function pumpDemo()
  if not demo.handle then return end

  -- Drain whatever has arrived since the last frame. poll() reports
  -- "complete" only once every chunk has been handed over, so this loop always
  -- terminates with the full body in hand.
  while true do
    local state, chunk, err, code = component.internet.poll(demo.handle)

    if chunk then
      demo.chunks = demo.chunks + 1
      demo.bytes = demo.bytes + #chunk
    end

    if state == "pending" then
      demo.state = "downloading"
      return
    end

    if state == "complete" then
      demo.state = "done"
      demo.detail = string.format("%s, %d bytes in %d chunks",
                                  fmtBytes(demo.bytes), demo.bytes, demo.chunks)
      component.internet.close(demo.handle)
      demo.handle = nil
      demo.done = true
      return
    end

    if state == "failed" then
      demo.state = "error"
      demo.detail = string.format("%s (HTTP %d)", err or "request failed", code or 0)
      component.internet.close(demo.handle)
      demo.handle = nil
      demo.done = true
      return
    end

    -- "invalid": the host dropped the handle underneath us.
    demo.state = "error"
    demo.detail = err or "invalid handle"
    demo.handle = nil
    demo.done = true
    return
  end
end

-- ------------------------------------------------------------ status bar ---

local elapsed = 0.0
local statusAccum = 0.0
local spinner = { "|", "/", "-", "\\" }
local spinIndex = 1

local function drawStatus()
  local c, r = screen.getResolution()
  local line = r  -- 1-based cursor row of the bottom line

  screen.setCursor(0, line - 1)
  screen.setColor(0xFFD479, 0x0A0A12)

  local text
  if demo.handle then
    text = string.format(" [%s] %s  %s", spinner[spinIndex], demo.state, demo.detail)
  elseif demo.done then
    text = string.format(" [ok] %s: %s", demo.state, demo.detail)
  elseif component.internet then
    text = " [..] idle"
  else
    text = " [--] internet card disabled"
  end

  screen.write(string.format("%d/%d cells  %.0fs  RAM %s%s",
                             c, r, elapsed, fmtBytes(component.ram.used()), text))

  -- Pad to the full width so leftovers from a longer previous line are cleared.
  local remaining = c - screen.getCursor()
  if remaining > 0 then screen.write(string.rep(" ", remaining)) end
end

if component.internet then startDemo() end
drawStatus()

-- ------------------------------------------------------------- host tick ---

-- Called once per frame by the host. We throttle the redraw so an idle machine
-- costs nothing.
function host.onTick(dt)
  elapsed = elapsed + (dt or 0)
  statusAccum = statusAccum + (dt or 0)
  pumpDemo()

  if statusAccum < 0.2 then return end
  statusAccum = 0.0
  spinIndex = (spinIndex % #spinner) + 1
  drawStatus()
end

host.log(string.format("boot ok: %dx%d @ %d-bit, ram %s, internet %s",
                       cols, rows, depth, ram_kb < 0 and "infinite" or (ram_kb .. "KB"),
                       tostring(internet_enabled)))