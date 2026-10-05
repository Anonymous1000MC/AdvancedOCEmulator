# ---------------------------------------------------------------------------
#  OcEmu third-party dependency resolution
#
#  Strategy: prefer the distro-provided packages (CachyOS host deps listed in
#  the task brief: lua52, curl, nlohmann-json, sdl2, glew) and only reach for
#  the network when a package is genuinely absent. This keeps the build fast
#  and offline-friendly once the host packages are installed.
# ---------------------------------------------------------------------------

include_guard(GLOBAL)
include(FetchContent)
include(CMakeDependentOption)

set(OCEMU_IMGUI_VERSION "v1.91.9b" CACHE STRING "Dear ImGui git tag to build against")
# Pinned to the commit behind the v3.11.3 tag. A git clone is used rather than a
# release tarball so content integrity is verified by git's own object hashing.
set(OCEMU_NLOHMANN_COMMIT "9cca280a4d0ccf0c08f47a99aa71d1b0e52f8d03"
    CACHE STRING "nlohmann-json commit to build against")

# --- SDL2 -------------------------------------------------------------------
find_package(SDL2 QUIET)
if(NOT SDL2_FOUND)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
  add_library(ocemu_sdl2 ALIAS PkgConfig::SDL2)
  message(STATUS "ocemu: SDL2 resolved through pkg-config (${SDL2_VERSION})")
else()
  add_library(ocemu_sdl2 ALIAS SDL2::SDL2)
  message(STATUS "ocemu: SDL2 resolved through CMake (${SDL2_VERSION})")
endif()

# --- OpenGL loader (GLEW) ---------------------------------------------------
find_package(GLEW QUIET)
if(GLEW_FOUND)
  add_library(ocemu_glew ALIAS GLEW::GLEW)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GLEW REQUIRED IMPORTED_TARGET glew)
  add_library(ocemu_glew ALIAS PkgConfig::GLEW)
endif()
message(STATUS "ocemu: GLEW ready")

# --- OpenGL / GLU -----------------------------------------------------------
set(OpenGL_GL_PREFERENCE GLVND)
find_package(OpenGL REQUIRED)

# --- Lua 5.2 (the OpenComputers reference VM) -------------------------------
find_package(PkgConfig REQUIRED)
pkg_check_modules(LUA52 REQUIRED IMPORTED_TARGET lua5.2)
add_library(ocemu_lua ALIAS PkgConfig::LUA52)
message(STATUS "ocemu: Lua ${LUA52_VERSION} ready")

# --- libcurl ---------------------------------------------------------------
# NOTE: on Arch/CachyOS the pkg-config module is named "libcurl", not "curl".
pkg_check_modules(CURL QUIET IMPORTED_TARGET libcurl)
if(NOT CURL_FOUND)
  pkg_check_modules(CURL REQUIRED IMPORTED_TARGET curl)
endif()
add_library(ocemu_curl ALIAS PkgConfig::CURL)
message(STATUS "ocemu: libcurl ${CURL_VERSION} ready")

# --- nlohmann/json ---------------------------------------------------------
# Prefer the host package; fall back to fetching the single-header release so
# the workspace always builds even on a machine without `nlohmann-json`
# installed.
find_package(nlohmann_json 3.10 QUIET)
if(nlohmann_json_FOUND)
  set(OCEMU_JSON_TARGET nlohmann_json::nlohmann_json)
  message(STATUS "ocemu: nlohmann-json ${nlohmann_json_VERSION} resolved from system")
else()
  message(STATUS "ocemu: nlohmann-json not installed - fetching ${OCEMU_NLOHMANN_COMMIT}")
  FetchContent_Declare(nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        ${OCEMU_NLOHMANN_COMMIT}
    GIT_SHALLOW    FALSE
    GIT_PROGRESS   TRUE
  )
  FetchContent_MakeAvailable(nlohmann_json)
  # nlohmann_json::nlohmann_json is itself an ALIAS, so it cannot be re-aliased;
  # export the name instead of wrapping it.
  set(OCEMU_JSON_TARGET nlohmann_json::nlohmann_json)
endif()

# --- Dear ImGui ------------------------------------------------------------
# ImGui ships no CMakeLists.txt, so we point SOURCE_SUBDIR at a path that does
# not exist. That makes FetchContent_MakeAvailable() download the sources
# without trying to add_subdirectory() them; we then compile the sources
# ourselves into a dedicated target below.
set(OCEMU_IMGUI_SOURCE_SUBDIR "imgui-has-no-cmake-files")
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        ${OCEMU_IMGUI_VERSION}
  GIT_SHALLOW    TRUE
  SOURCE_SUBDIR  ${OCEMU_IMGUI_SOURCE_SUBDIR}
)
FetchContent_MakeAvailable(imgui)

FetchContent_GetProperties(imgui)
if(NOT imgui_POPULATED)
  message(FATAL_ERROR "ocemu: failed to fetch Dear ImGui ${OCEMU_IMGUI_VERSION}")
endif()

set(OCEMU_IMGUI_DIR "${imgui_SOURCE_DIR}")

set(OCEMU_IMGUI_SOURCES
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)

if(NOT EXISTS "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp")
  message(FATAL_ERROR "ocemu: ImGui SDL2/OpenGL3 backends missing from ${imgui_SOURCE_DIR}")
endif()

add_library(ocemu_imgui STATIC ${OCEMU_IMGUI_SOURCES})
add_library(ocemu::imgui ALIAS ocemu_imgui)

target_include_directories(ocemu_imgui SYSTEM PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)

# ImGui is third-party code: keep our aggressive optimisation flags off it.
target_compile_features(ocemu_imgui PUBLIC cxx_std_17)
set_target_properties(ocemu_imgui PROPERTIES
  POSITION_INDEPENDENT_CODE ON
  CXX_EXTENSIONS OFF
)

# The OpenGL3 backend bundles its own loader (imgl3w) by default on Linux,
# which we deliberately keep: it resolves the GL 3.x core entry points itself
# and therefore never conflicts with GLEW, which we link separately for the
# host application's own GL queries.

target_link_libraries(ocemu_imgui PUBLIC ocemu_sdl2 OpenGL::GL)

message(STATUS "ocemu: Dear ImGui ${OCEMU_IMGUI_VERSION} ready (${OCEMU_IMGUI_DIR})")