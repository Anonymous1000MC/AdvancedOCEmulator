// ---------------------------------------------------------------------------
//  ocemu -- a lightweight, standalone OpenComputers emulator.
//
//  Usage:
//    ocemu [--config <path/to/config.json>] [--rom <path/to/boot.lua>]
//          [--help] [--version]
//
//  Both paths default to the workspace copies so a freshly cloned tree runs
//  with no arguments at all.
// ---------------------------------------------------------------------------
#include <clocale>
#include <cstdio>
#include <cstring>
#include <string>

#include "ocemu/App.hpp"

namespace {

void printUsage() {
  std::printf(
      "ocemu -- OpenComputers emulator\n"
      "\n"
      "usage: ocemu [options]\n"
      "\n"
      "  --config <path>   use this config.json instead of the workspace copy\n"
      "  --rom <path>      use this Lua ROM instead of assets/lua/boot.lua\n"
      "  --help            show this message\n"
      "  --version         print the version and exit\n"
      "\n"
      "Environment:\n"
      "  OCEMU_FONT        path to a monospace TTF to use for the screen glyphs\n");
}

}  // namespace

int main(int argc, char** argv) {
  // Keep number formatting inside the panel predictable regardless of the
  // user's locale.
  std::setlocale(LC_ALL, "C");

  std::string configPath;
  std::string romPath;

  for (int i = 1; i < argc; ++i) {
    const char* arg = argv[i];
    const bool hasNext = (i + 1) < argc;

    if (std::strcmp(arg, "--help") == 0 || std::strcmp(arg, "-h") == 0) {
      printUsage();
      return 0;
    }
    if (std::strcmp(arg, "--version") == 0 || std::strcmp(arg, "-V") == 0) {
      std::printf("ocemu 1.0.0\n");
      return 0;
    }
    if (std::strcmp(arg, "--config") == 0 && hasNext) {
      configPath = argv[++i];
      continue;
    }
    if (std::strcmp(arg, "--rom") == 0 && hasNext) {
      romPath = argv[++i];
      continue;
    }

    std::fprintf(stderr, "ocemu: unrecognised argument '%s' (try --help)\n", arg);
    return 2;
  }

  try {
    ocemu::App app;
    if (!configPath.empty()) app.setConfigPath(configPath);
    if (!romPath.empty()) app.setRomPath(romPath);
    return app.run();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "ocemu: fatal: %s\n", e.what());
    return 1;
  } catch (...) {
    std::fprintf(stderr, "ocemu: fatal: unknown error\n");
    return 1;
  }
}