# ---------------------------------------------------------------------------
# pexninja — the much larger reference Pulse client.
#
# The .cpp in this directory was lifted from another repository and is being
# ported to build inside the doppler tree. It exercises a much wider Pulse
# surface than src/main.cpp (the rest of doppler), so it pulls in a fatter
# dependency tree: ImPlot, gl3w and ImGui-Addons (file browser).
#
# It used to also depend on the system GLib + GStreamer, but those clashed at
# runtime with the copies statically linked into libpexlgpl, so they have been
# replaced by native C++/POSIX code directly in pexninja.cpp (logging now just
# goes to stdout/stderr). No GLib/GStreamer packages are required any more.
#
# We keep this build self-contained — the parent CMakeLists.txt just calls
# add_subdirectory(demos/pexninja). Run `cmake --build build --target pexninja` to
# build just this one binary. It builds on Linux and macOS.
# ---------------------------------------------------------------------------

# ---- System dependencies --------------------------------------------------
# OpenGL / GLFW / Threads are needed for the window + GL context. X11 is only
# used by the Linux window-enumeration helpers; on macOS the equivalent code
# uses the CoreGraphics / CoreFoundation frameworks instead.
find_package(OpenGL REQUIRED)
find_package(glfw3 REQUIRED)
find_package(Threads REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter) # gl3w generator

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    find_package(X11 REQUIRED)
endif()

# ---- Fetched third-party dependencies -------------------------------------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# Dear ImGui — also fetched by the parent CMakeLists.txt; FetchContent will
# happily reuse the already-populated source tree when both declarations
# point at the same tag.
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG        v1.91.5-docking
    GIT_SHALLOW    TRUE)

# ImPlot — plotting widgets used by pexninja's media-stats panels.
FetchContent_Declare(
    implot
    GIT_REPOSITORY https://github.com/epezent/implot.git
    GIT_TAG        v0.16
    GIT_SHALLOW    TRUE)

# gl3w — explicit OpenGL function loader (pexninja calls gl3wInit()).
# The repo's own CMakeLists.txt runs a Python generator at configure time.
# Upstream has no release tags, so we pin to a known-good commit SHA.
FetchContent_Declare(
    gl3w
    GIT_REPOSITORY https://github.com/skaslev/gl3w.git
    GIT_TAG        d5ba9340cdeb9154323817f5c87e5a5c377fdef7)

# ImGui-Addons — provides imgui_addons::ImGuiFileBrowser used by the
# media-file picker. No CMakeLists, so we compile its single .cpp by hand.
# Upstream has no release tags, so we pin to a known-good commit SHA.
FetchContent_Declare(
    imgui_addons
    GIT_REPOSITORY https://github.com/gallickgunner/ImGui-Addons.git
    GIT_TAG        0b25588ba842b93537f82ae84f27a75f604b04ce)

FetchContent_MakeAvailable(imgui implot imgui_addons)

# gl3w needs network access at configure time to download glcorearb.h /
# khrplatform.h from registry.khronos.org. Sandboxes (and some CI) don't
# have that, but the headers are shipped by the system's libgl-dev /
# libegl-dev packages — pre-stage them so the generator's existence check
# reuses them instead of trying to fetch.
FetchContent_GetProperties(gl3w)
if(NOT gl3w_POPULATED)
    FetchContent_Populate(gl3w)
    foreach(_pair
            "/usr/include/GL/glcorearb.h;GL"
            "/usr/include/KHR/khrplatform.h;KHR")
        list(GET _pair 0 _src)
        list(GET _pair 1 _subdir)
        if(EXISTS "${_src}")
            file(MAKE_DIRECTORY "${gl3w_BINARY_DIR}/include/${_subdir}")
            file(COPY "${_src}"
                 DESTINATION "${gl3w_BINARY_DIR}/include/${_subdir}")
        endif()
    endforeach()
    add_subdirectory(${gl3w_SOURCE_DIR} ${gl3w_BINARY_DIR})
endif()

# ---- Dear ImGui static lib (with the extras pexninja needs) ---------------
# The parent project builds its own "imgui" target; we build a separate one
# named "pexninja_imgui" so we can layer on imgui_stdlib (used here, not in
# src/main.cpp) without touching the doppler target.
add_library(pexninja_imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
    ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp)
target_include_directories(pexninja_imgui PUBLIC
    ${imgui_SOURCE_DIR}
    ${imgui_SOURCE_DIR}/backends
    ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(pexninja_imgui PUBLIC glfw OpenGL::GL)

# ---- ImPlot static lib (depends on ImGui) ---------------------------------
add_library(pexninja_implot STATIC
    ${implot_SOURCE_DIR}/implot.cpp
    ${implot_SOURCE_DIR}/implot_items.cpp
    ${implot_SOURCE_DIR}/implot_demo.cpp)
target_include_directories(pexninja_implot PUBLIC ${implot_SOURCE_DIR})
target_link_libraries(pexninja_implot PUBLIC pexninja_imgui)

# ---- ImGui-Addons file browser --------------------------------------------
add_library(pexninja_imgui_addons STATIC
    ${imgui_addons_SOURCE_DIR}/FileBrowser/ImGuiFileBrowser.cpp)
target_include_directories(pexninja_imgui_addons PUBLIC
    ${imgui_addons_SOURCE_DIR}/FileBrowser)
target_link_libraries(pexninja_imgui_addons PUBLIC pexninja_imgui)
# The file browser's .cpp uses some sprintf/strcpy-style helpers that newer
# glibc/gcc flag as deprecated; silence those so pexninja's -Werror-free
# build doesn't become a treasure hunt across upstream code.
target_compile_options(pexninja_imgui_addons PRIVATE
    -Wno-deprecated-declarations -Wno-format-truncation)

# ---- The pexninja executable ---------------------------------------------
add_executable(pexninja pexninja.cpp)

# Enable the platform-specific code paths that pexninja.cpp guards with
# HAVE_X11 (Linux window enumeration etc.). On non-Linux hosts the upstream
# file expects HOST_WINDOWS or HOST_DARWIN to be defined instead.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(pexninja PRIVATE HAVE_X11)
elseif(WIN32)
    target_compile_definitions(pexninja PRIVATE HOST_WINDOWS)
elseif(APPLE)
    target_compile_definitions(pexninja PRIVATE HOST_DARWIN)
endif()

target_include_directories(pexninja PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR})  # for ImGuiUtils.h shim

# The upstream pexninja.cpp expects a build-time VERSION string macro (used
# in the "PexNinja/<ver>" user-agent). Provide one based on the parent
# project's version so it's always defined.
target_compile_definitions(pexninja PRIVATE
    "VERSION=\"${PROJECT_VERSION}\"")

target_link_libraries(pexninja PRIVATE
    pexninja_imgui
    pexninja_implot
    pexninja_imgui_addons
    pexip::pulse
    gl3w
    Threads::Threads)

# Platform-specific window/display enumeration backends.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(pexninja PRIVATE ${X11_LIBRARIES})
elseif(APPLE)
    # The HOST_DARWIN code path enumerates windows/displays via CoreGraphics
    # and CoreFoundation, and AppKit is pulled in by GLFW's Cocoa backend.
    find_library(COREGRAPHICS_LIBRARY CoreGraphics REQUIRED)
    find_library(COREFOUNDATION_LIBRARY CoreFoundation REQUIRED)
    find_library(APPKIT_LIBRARY AppKit REQUIRED)
    target_link_libraries(pexninja PRIVATE
        ${COREGRAPHICS_LIBRARY}
        ${COREFOUNDATION_LIBRARY}
        ${APPKIT_LIBRARY})
endif()

# pexninja itself isn't -Werror, but the upstream code triggers a handful of
# benign warnings on modern compilers — silence the most common ones so the
# build log stays useful.
target_compile_options(pexninja PRIVATE
    -Wno-deprecated-declarations
    -Wno-unused-parameter
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-unused-function
    -Wno-sign-compare
    -Wno-missing-field-initializers)

# Bake the Pulse lib directory into the RPATH (so libpexpulse resolves without
# LD_LIBRARY_PATH) and emit build/run-pexninja.sh — using the same shared
# helpers as every other demo so pexninja also gets PEX_BASE_PATH exported and
# benefits from the macOS dylib relocation done in pulse_find_runtime().
pulse_demo_rpath(pexninja)
pulse_demo_launcher(pexninja run-pexninja.sh)
