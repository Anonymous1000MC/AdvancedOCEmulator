# Advanced OpenComputers Emulator

A standalone OpenComputers emulator: C++20 + SDL2 + Dear ImGui, with **ImGui as
the entire UI**. No window system, no SDL renderer, no separate ImGui context.

It runs the real [OCEmu](https://github.com/zenith391/OCEmu) Lua core, so
OpenComputers software — OpenOS and anything built for it — runs **unmodified**.

```
╔══════════════════════════════════════╗
║  OpenOS 1.8.10 (2048k RAM)           ║
║  Most programs can be interrupted... ║
╚══════════════════════════════════════╝
Note: Your home directory is readonly. Run `install` and reboot.
/home # dir
```

## Quick start

```bash
git clone --recurse-submodules https://github.com/Anonymous1000MC/AdvancedOCEmulator
cd AdvancedOCEmulator
cmake -S . -B build
cmake --build build -j"$(nproc)"
./build/bin/ocemu
```

OCEmu is vendored, so `--recurse-submodules` is only needed the first time and
only to pull OCEmu's own nested dependency. If you already have an OCEmu
checkout, skip it and point the emulator at it instead:

```bash
OCEMU_OC_SRC=/path/to/OCEmu/src ./build/bin/ocemu
```

The emulator looks for OCEmu in this order: `OCEMU_OC_SRC`,
`third_party/OCEmu/src`, `../OCEmu/src`, then `~/OCEmu/src`. Machine data is
found via `OCEMU_OC_DATA`, defaulting to `~/.local/share/ocemu`.

Requirements: CMake ≥ 3.16, a C++20 compiler, and dev packages for
`lua5.2`, `sdl2`, `glew` and `libcurl`. Everything else (Dear ImGui,
nlohmann/json) is fetched by CMake.

On Arch:

```bash
sudo pacman -S base-devel cmake lua sdl2 glew curl
```

## Using it

Boot, and OpenOS comes up on its own. Type at it.

| Key | Action |
|---|---|
| any key / text | goes to the guest |
| `Ctrl+C` / `Ctrl+Shift+C` | copy the mouse selection |
| `Ctrl+V` / `Ctrl+Shift+V`, middle-click | paste through as real keystrokes |
| drag | select text |

**Component Manager** (the floating panel) configures the machine and applies it
on reboot:

| Control | Meaning |
|---|---|
| GPU tier / Screen tier | 1 = 50×16 mono, 2 = 80×25 @ 4-bit, 3 = 160×50 @ 8-bit. The effective limit is the **lower** of the two |
| RAM | slider, or unlimited |
| Internet card | adds an `internet` component so `wget`/`curl` work |
| Reboot / Apply | restarts the machine with the new hardware |

**Diagnostics** shows the live GL renderer, grid, RAM and machine state, plus
**Restore to OpenOS** (below).

### Restore to OpenOS

An OpenComputers machine boots from the code in its **EEPROM**. Anything that
overwrites it — most often another OS's installer, which flashes its own
bootloader — leaves the machine unable to start OpenOS. That happened here when
TheanOS's installer replaced OpenOS's bootloader with a network stub that
downloaded code from GitHub on every boot.

**Restore to OpenOS** fetches `restore/bootloader.lua` from this repo, shows a
progress bar, writes it back to the EEPROM atomically, and reboots the machine.
It checks the payload looks like a bootloader before overwriting anything, so a
failed download can't brick the machine.

Override the source with `OCEMU_OPENOS_URL`.

### Debugging

Set `debug = true` under `emulator { }` in `~/.local/share/ocemu/ocemu.cfg` for
OCEmu's own trace — every component call, every coroutine switch, and
`Missing environment access env.X` whenever guest code touches a global OCEmu
doesn't provide. That last one is how the API gaps below were found.

The emulator also prints a status line to stdout every 10s, and the Diagnostics
panel shows machine state and the last error.

## How it works

```
┌─────────────────────────────────────────────┐
│ ImGui (the whole UI)                        │
│  ├─ OC screen        ─┐                     │
│  ├─ Component Manager ├─ native C++         │
│  └─ Diagnostics      ─┘                     │
└──────────────┬──────────────────────────────┘
               │ elsa.*  (the host seam OCEmu expects)
┌──────────────▼──────────────────────────────┐
│ OCEmu Lua core  (vendored, unmodified)      │
│  main.lua · machine.lua · apis/ · component/ │
└──────────────┬──────────────────────────────┘
               │ component API
┌──────────────▼──────────────────────────────┐
│ emulated machine                            │
│  screen_native + keyboard_native  ← ours    │
│  gpu, eeprom, filesystem, computer ← OCEmu  │
└─────────────────────────────────────────────┘
```

Only the window boundary is ours. OCEmu supplies the kernel, process scheduler,
component API, filesystem and EEPROM emulation, signal delivery and call
budgeting; we replace its two SDL-backed components with native C++ ones and
provide the `elsa` host shim (filesystem, timer, system, handlers) it expects.

Machine data — `ocemu.cfg` and the virtual disks — lives in
`~/.local/share/ocemu`, not in the source tree. The EEPROM that OpenOS boots
from is `<eeprom-address>/code.lua` in there.

### The compatibility shim

`restore/bootloader.lua` is OpenOS's own `init.lua`, verbatim, preceded by a
shim. OCEmu's sandbox exposes a subset of the OpenComputers API, so the shim
fills in the rest. Each of these was a real bug, not a stylistic choice:

- **`component.isAvailable()` / `inAvailable()`** — implemented by neither OCEmu
  nor expected elsewhere. `boot/91_gpu.lua` starts its `component_available`
  handler with `component.isAvailable("gpu")`; without it the handler raises,
  `tty.bind(gpu)` never runs, `tty.window.gpu` stays `nil`, and every terminal
  draw then silently no-ops. Symptom: a live shell with a blinking cursor, a
  blank screen, and keystrokes that get eaten.
- **`component.proxy()`** — referenced by OCEmu's own code paths but never
  actually defined.
- **`component.list(filter)`** returns a *callable map* of `address -> type`, not
  an address, so `component.gpu` and friends need a `firstAddress()` helper.
- **A proxy's `address` and `type` are data, not methods** — returning closures
  makes `gpu.bind(screen.address)` pass a function where a string is required.
- **`computer.getBootAddress()`** and friends — missing entirely; OpenOS's
  `init.lua` calls `getBootAddress()` on its second line.
- **`env.component.invoke` returns results directly**, with no leading pcall
  status. Stripping one made every call whose first result wasn't literally
  `true` (a file handle, a resolution, a string) come back `nil`.
- **The per-tick call budget.** `main.lua` grants `maxCallBudget = 1.5` per tick
  and every cost-accounted method does `if not machine.consumeCallBudget(cost)
  then return end` — which returns *no values*, so the guest sees `nil` instead
  of an error and OpenOS's `package.lua` decides every module is missing.

### Performance note

The screen once kept a second, palette-resolved copy of the cell array and
rebuilt **all** of it after every single cell write, making a write O(cells): a
full 160×50 repaint took **254 ms**, so any GUI program crawled. Writes are O(1)
now and colours are resolved once per drawn frame, as upstream `screen_sdl2`
does — a full repaint is **0.014 ms**, about 18,000× faster. There's a
regression guard in the test suite.

## Tests

```bash
./build/bin/ocemu_tests
```

Covers tiers and resolution limits, colour quantisation, screen buffer and
geometry, allocator accounting, config persistence, the Lua VM (including OOM
recovery), the `elsa` shim, the OC font decoder, double-width glyph layout,
write throughput, and an end-to-end boot of real OpenOS that asserts the banner,
the shell prompt, and that typed characters (including uppercase) are echoed.

## Repository layout

| Path | What |
|---|---|
| `src/`, `include/`, `tests/` | the emulator |
| `restore/bootloader.lua` | the EEPROM payload the restore button writes |
| `restore/manifest.json` | version, URL, SHA-256 of the payload and OS tree |
| `openos/` | OpenOS 1.8.10, the reference copy |
| `third_party/OCEmu/` | vendored OCEmu |

## Components

Provided by OCEmu's own Lua components: `gpu`, `eeprom`, three `filesystem`
volumes, `modem`, `internet` (optional), `computer`, `ocemu`.

Provided natively:

| Component | Notes |
| --- | --- |
| `screen` | Replaces `screen_sdl2.lua`. O(1) writes, OC's own bitmap font, palette, depth, keycodes, mouse and touch. |
| `keyboard` | Replaces `keyboard_sdl2.lua`. Real SDL key events, printable text, modifiers. |
| `sound` | Replaces `sound_card.lua`. See below. |
| `data` | In-memory key/value store (OCEmu's `data.lua`). |
| `drive` | Floppy slot (OCEmu's `drive.lua`), currently mounted empty. |

### The sound card

OCEmu's `sound_card.lua` is a stub: every method is `--STUB`, and the file calls
`elsa.SDL.openAudioDevice` at load time, so it cannot even load here
(`elsa.SDL` is `false`). `sound_native` therefore implements the card directly:

- `getSampleRate()` reports 44100, and the audio device refuses to resample, so
  the rate the guest is told is the rate it gets.
- `openSpeaker(speaker, dt, freq, volume, delay, mode)` returns a handle.
  `pushSample(handle, sample)` then takes one amplitude per `dt` period and the
  **card** synthesises the waveform, as real hardware does, rather than making
  the guest do it.
- Sine, square, triangle and sawtooth; `setFrequency`, `setVolume`, `setPitch`,
  `setSpeed`, `closeSpeaker`, `getError`.
- Speakers mix additively. The ring buffer is mutex-guarded because the guest
  runs on the Lua thread while SDL drains it on the audio thread, and it is
  capped at two seconds: a guest that stops pushing drops the oldest audio
  instead of growing memory without bound.

## Known gaps

- **Modem radio.** The `modem` component is present, but there is only one
  machine, so there is nothing to transmit to and wireless is inert.
- **Drive has no disk.** The `drive` component mounts empty; there is no UI to
  insert one.
- **Rendering is not accelerated.** The screen is composited on the CPU into one
  texture, which is fast enough at tier 3 but is not what a real GPU path does.
- **Debugger.** OCEmu's debugger is not wired to our host, so there is no
  stepping or breakpoint UI.

## Licence

GPLv3 — see [LICENSE](LICENSE). This is forced by redistributing OpenOS.

**[LICENSE-NOTICE.md](LICENSE-NOTICE.md) is important**: the vendored OCEmu ships
no licence of its own, so it is reproduced as-is and is *not* covered by this
project's GPLv3 grant.
