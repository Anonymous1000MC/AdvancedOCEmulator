cmake_minimum_required(VERSION 3.20)

project(ocemu
  VERSION      1.0.0
  DESCRIPTION  "Lightweight standalone OpenComputers emulator (SDL2 + Dear ImGui)"
  LANGUAGES    CXX)

# ---------------------------------------------------------------------------
#  Toolchain / optimisation profile
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  message(STATUS "ocemu: CMAKE_BUILD_TYPE not set, defaulting to Release")
endif()

# -march=native is the whole point of shipping this for CachyOS, so probe for
# it instead of assuming: a cross build or an unusual toolchain simply loses the
# flag rather than failing the configure step.
include(CheckCXXCompilerFlag)
set(OCEMU_NATIVE_FLAGS "")
foreach(_flag -march=native -mtune=native)
  string(MAKE_C_IDENTIFIER "HAVE${_flag}" _flag_var)
  check_cxx_compiler_flag(${_flag} ${_flag_var})
  if(${_flag_var})
    list(APPEND OCEMU_NATIVE_FLAGS ${_flag})
  endif()
endforeach()

# Dependency targets must exist before ocemu_flags can reference them.
include(cmake/OcEmuDependencies.cmake)

# Applied to *our* targets only; FetchContent'd third-party code stays on the
# portable defaults so it remains buildable on other machines.
add_library(ocemu_flags INTERFACE)
target_compile_features(ocemu_flags INTERFACE cxx_std_20)
target_compile_options(ocemu_flags INTERFACE
  $<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-O3;-fno-math-errno;-Wall;-Wextra;-Wno-unused-parameter>
  ${OCEMU_NATIVE_FLAGS}
)
target_link_libraries(ocemu_flags INTERFACE
  ocemu_sdl2
  ocemu_lua
  ocemu_curl
  ${OCEMU_JSON_TARGET}
)

# ---------------------------------------------------------------------------
#  ocemu_vm -- the emulated machine.
#
#  Deliberately free of windowing dependencies: config, screen buffer, custom
#  allocator, internet card and the Lua VM. Keeping it separate lets the tests
#  drive the whole emulator core headless, with no display attached.
# ---------------------------------------------------------------------------
add_library(ocemu_vm STATIC
  src/Config.cpp
  src/OcEmuHost.cpp
  src/OcScreen.cpp
  src/OcSound.cpp
  src/ScreenBuffer.cpp
  src/MemoryAllocator.cpp
  src/InternetCard.cpp
  src/OcFont.cpp
  src/OpenOsRestore.cpp
  src/LuaMachine.cpp
)

target_include_directories(ocemu_vm PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Runtime asset locations, needed by both the app and the tests.
target_compile_definitions(ocemu_vm PUBLIC
  OCEMU_ASSET_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets"
  OCEMU_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)

target_link_libraries(ocemu_vm PUBLIC ocemu_flags)
set_target_properties(ocemu_vm PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
#  ocemu_core -- presentation on top of the VM.
# ---------------------------------------------------------------------------
add_library(ocemu_core STATIC
  src/GlyphAtlas.cpp
  src/App.cpp
  src/ui/ComponentManagerPanel.cpp
)

target_link_libraries(ocemu_core
  PUBLIC
    ocemu_vm
    ocemu::imgui
    ocemu_glew
)

set_target_properties(ocemu_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

# ---------------------------------------------------------------------------
#  Executable
# ---------------------------------------------------------------------------
add_executable(ocemu src/main.cpp)
target_link_libraries(ocemu PRIVATE ocemu_core)
set_target_properties(ocemu PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---------------------------------------------------------------------------
#  Install
# ---------------------------------------------------------------------------
install(TARGETS ocemu RUNTIME DESTINATION bin)
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/assets/ DESTINATION share/ocemu/assets)

# ---------------------------------------------------------------------------
#  Tests
#
#  These link only ocemu_vm, so they run with no display attached.
# ---------------------------------------------------------------------------
include(CTest)
if(BUILD_TESTING AND TARGET ocemu_core)
  add_executable(ocemu_tests tests/core_test.cpp)
  # GlyphAtlas.hpp needs imgui.h for its declarations, but the geometry helpers
  # under test are header-only, so include dirs alone are enough -- no ImGui
  # library needs to be linked.
  target_link_libraries(ocemu_tests PRIVATE ocemu_vm)
  target_include_directories(ocemu_tests PRIVATE
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
  )
  set_target_properties(ocemu_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
  )
  add_test(NAME core COMMAND ocemu_tests)
endif()

message(STATUS "")
message(STATUS "  ocemu ${PROJECT_VERSION} configured")
message(STATUS "    build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "    native opt : ${OCEMU_NATIVE_FLAGS}")
message(STATUS "    assets     : ${CMAKE_CURRENT_SOURCE_DIR}/assets")
message(STATUS "    tests      : ${BUILD_TESTING}")
message(STATUS "")