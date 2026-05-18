cmake_minimum_required(VERSION 3.16)
project(doppler
    VERSION 0.1.0
    DESCRIPTION "A tiny Pexip Pulse + Dear ImGui video-call demo"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Always export compile_commands.json (handy for editor/IDE integration).
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
#
# We need three things to build the demo:
#   1. The Pexip Pulse library + headers (installed from the .deb packages
#      shipped in this repository - see README.md).
#   2. GLFW + OpenGL, used to host the Dear ImGui control panel.
#   3. Dear ImGui itself, which we fetch as source via CMake's FetchContent.
#
# Keeping the dependency story this short is intentional: the goal of the
# demo is to show how few moving parts the Pulse API actually needs.
# ---------------------------------------------------------------------------

find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)
find_package(Threads REQUIRED)

# --- Locate the Pexip Pulse library --------------------------------------
#
# The .deb packages install Pulse to /opt/pexip (not the usual /usr prefix),
# so we look there first.  We don't rely on pkg-config since the .deb does
# not ship a .pc file.
set(PEXIP_PREFIX "/opt/pexip" CACHE PATH "Install prefix of the Pexip Pulse package")

find_path(PEXPULSE_INCLUDE_DIR
    NAMES pexpulse/pulse.h
    HINTS "${PEXIP_PREFIX}/include"
    REQUIRED)

find_library(PEXPULSE_LIBRARY
    NAMES pexpulse
    HINTS "${PEXIP_PREFIX}/lib"
    REQUIRED)

message(STATUS "Found pexpulse headers: ${PEXPULSE_INCLUDE_DIR}")
message(STATUS "Found pexpulse library: ${PEXPULSE_LIBRARY}")

# Expose Pulse as a clean imported target so the application target stays
# tidy.  We also bake the Pulse lib directory into the RPATH so the
# resulting binary can find libpexpulse.so and its private siblings
# (libpexlgpl.so, libimf.so, libonnxruntime.so.1, ...) without the user
# needing to set LD_LIBRARY_PATH.
add_library(pexip::pulse SHARED IMPORTED)
set_target_properties(pexip::pulse PROPERTIES
    IMPORTED_LOCATION "${PEXPULSE_LIBRARY}"
    INTERFACE_INCLUDE_DIRECTORIES "${PEXPULSE_INCLUDE_DIR}")

# --- Dear ImGui (vendored via FetchContent) ------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5-docking
    GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

# Dear ImGui doesn't ship its own CMakeLists - we just compile the core
# sources plus the two backends we want (GLFW + OpenGL3) into a static
# library that the demo links against.
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw OpenGL::GL)

# ---------------------------------------------------------------------------
# The demo application
# ---------------------------------------------------------------------------
add_executable(doppler src/main.cpp)
target_link_libraries(doppler PRIVATE
    imgui
    pexip::pulse
    Threads::Threads)

# Make sure the binary can find libpexpulse.so at run time without the
# user having to set LD_LIBRARY_PATH for the *direct* dependency.
#
# Note: libpexpulse.so itself has its own DT_RUNPATH baked in (pointing
# to the Pulse build tree), and because DT_RUNPATH is not inherited from
# the executable to its transitive dependencies, you still have to put
# /opt/pexip/lib on LD_LIBRARY_PATH (or in /etc/ld.so.conf.d/) to resolve
# Pulse's private siblings (libpexlgpl.so, libimf.so, libonnxruntime.so.1,
# libsvml.so).  The launcher script generated below takes care of that
# so end users can just run ./build/doppler.
set_target_properties(doppler PROPERTIES
    BUILD_RPATH   "${PEXIP_PREFIX}/lib"
    INSTALL_RPATH "${PEXIP_PREFIX}/lib")

# Convenience launcher that sets LD_LIBRARY_PATH so the user can just run
# `./build/run-doppler.sh` without dealing with Pulse's transitive lib paths.
# ---------------------------------------------------------------------------
# Optional: the much larger pexninja reference client, lifted from another
# repo and ported to build inside this tree. Disabled by default because it
# pulls in extra system deps (X11, GStreamer, glib-2.0); flip BUILD_PEXNINJA
# on (e.g. `cmake -S . -B build -DBUILD_PEXNINJA=ON`) to build it.
# ---------------------------------------------------------------------------
option(BUILD_PEXNINJA "Build the larger pexninja reference client" OFF)
if(BUILD_PEXNINJA)
    add_subdirectory(pexninja)
endif()

# ---------------------------------------------------------------------------
# Optional: the doppler-infinity demo. Uses Pulse as a pure media engine
# (pulse_new_external_rest + pulse_setup_stage_1/2_from_structure) and
# drives the Pexip Infinity Client REST API from libcurl. Disabled by
# default so the lean `doppler` build keeps working without libcurl. Flip
# BUILD_DOPPLER_INFINITY on to build it.
# ---------------------------------------------------------------------------
option(BUILD_DOPPLER_INFINITY "Build the doppler-infinity (libcurl + Infinity REST) demo" OFF)
if(BUILD_DOPPLER_INFINITY)
    add_subdirectory(infinity-demo)
endif()

file(GENERATE
    OUTPUT  "${CMAKE_BINARY_DIR}/run-doppler.sh"
    CONTENT "#!/bin/sh
# Auto-generated by CMake - sets LD_LIBRARY_PATH so that libpexpulse.so's
# private siblings (libpexlgpl.so, libimf.so, libonnxruntime.so.1, ...)
# can be resolved by the dynamic linker, then execs the demo binary.
export LD_LIBRARY_PATH=\"${PEXIP_PREFIX}/lib:\${LD_LIBRARY_PATH}\"
exec \"$<TARGET_FILE:doppler>\" \"$@\"
"
    FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                     GROUP_READ GROUP_EXECUTE
                     WORLD_READ WORLD_EXECUTE)
