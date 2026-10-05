# Third-party notices

## OpenComputers Emulator (OCEmu) — `third_party/OCEmu`

Vendored from <https://github.com/zenith391/OCEmu>.

**OCEmu ships no license file.** Its only licence in the tree is
`src/sdl2/LICENSE`, which covers the bundled SDL2, not OCEmu's own code. Without
an explicit licence, copyright defaults to all rights reserved, so this vendored
copy is reproduced as-is and is **not** covered by this project's GPLv3 grant.

If you are the author or hold rights to OCEmu, add a licence to
`third_party/OCEmu` and it will be covered from that point on.

`third_party/OCEmu/src/loot/openos` is OpenOS (see below), added because OCEmu
gitignores `src/loot/` and therefore ships no OS tree.

## OpenOS — `openos/`, `third_party/OCEmu/src/loot/openos`, `restore/bootloader.lua`

OpenComputers' OpenOS 1.8.10, distributed under the **GPLv3**. Because this
project redistributes OpenOS code, this project is licensed GPLv3 as a whole.

`restore/bootloader.lua` is OpenOS's own `init.lua` plus the compatibility shim
described in the README.

## Dependencies supplied by the system

| Component | Licence |
|---|---|
| Dear ImGui | MIT |
| SDL2 | zlib |
| GLEW | BSD-3-Clause / MIT |
| Lua 5.2 | MIT |
| libcurl | curl (MIT-like) |
| nlohmann/json | MIT |

OCEmu's FFI layer uses `luaffifb` (vendored under `third_party/OCEmu/luaffifb`).
It is not used on this platform — we set `elsa.SDL = false` so OCEmu skips the
FFI/audio path entirely — but it is retained for parity with upstream.
