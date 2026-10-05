# AdvancedOCEmulator

Restore assets for the **Advanced OpenComputers Emulator** — a standalone
C++20 / SDL2 / Dear ImGui host that runs the real [OCEmu](https://github.com/Hexeption/OCEmu)
Lua core so unmodified OpenComputers software runs unmodified.

## Why this repo exists

An OpenComputers machine boots from the code in its **EEPROM**. On this emulator
that EEPROM lives at:

```
~/.local/share/ocemu/<eeprom-address>/code.lua
```

If anything overwrites it — most commonly another OS's installer, which flashes
its own network bootloader — the machine stops booting OpenOS. In our case
flashing TheanOS replaced OpenOS's 1778-byte bootloader with a 340-byte stub
that downloads and runs code from the internet on every boot, which then left the
machine unusable without an internet card.

The emulator's **Restore to OpenOS** button fetches `restore/bootloader.lua` from
this repo, writes it back to the EEPROM, shows download progress, and reboots the
machine.

## Layout

| Path | What it is |
|---|---|
| `restore/bootloader.lua` | The EEPROM payload: OpenOS 1.8.10's own `init.lua` preceded by a small compatibility shim. |
| `restore/manifest.json` | Version, the raw download URL, and SHA-256 of the bootloader and the OpenOS tree. |
| `openos/` | Reference copy of the OpenOS 1.8.10 tree (`bin/`, `boot/`, `etc/`, `home/`, `lib/`, `usr/`, `init.lua`). |

### Why the bootloader needs a shim

The payload is OpenOS's real `init.lua`, byte-for-byte. It is preceded by a shim
because OCEmu's sandbox exposes a subset of the OpenComputers API that OpenOS
expects:

- `component.list(filter)` returns a *callable map* of `address -> type`, not an
  address, so `component.gpu` and friends need a `firstAddress()` helper.
- `component.proxy()` was missing entirely.
- `component.isAvailable()` / `inAvailable()` are implemented by neither OCEmu nor
  upstream OpenOS's expectations — `boot/91_gpu.lua` needs them, and without them
  it raises, `tty.bind(gpu)` never runs, `tty.window.gpu` stays `nil`, and every
  terminal draw silently no-ops (a live shell with a blinking cursor and no
  prompt).
- A component proxy's `address` and `type` are *data*, not methods.
- `computer.getBootAddress()`, `isRobot()`, and the computer-label calls are
  missing; OpenOS's `init.lua` calls `getBootAddress()` on its second line.
- `env.component.invoke` returns its results directly, with no leading pcall
  status to strip.

## Licensing

`openos/` is OpenComputers' OpenOS, distributed under the GPLv3 — see
`openos/` for its own terms. `restore/bootloader.lua` is OpenOS's `init.lua` plus
the shim described above. The emulator itself is a separate work.
