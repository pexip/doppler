# ---------------------------------------------------------------------------
# PulseDemo.cmake — shared helpers for the Pexip Pulse showcase demos.
#
# Every demo under demos/ is a small, self-contained CMake project that links
# against the Pexip Pulse runtime. To keep each demo's CMakeLists.txt short and
# uniform, the boilerplate (locating Pulse, baking an RPATH, generating a
# run-*.sh launcher) lives here and is shared by all of them.
#
# The root CMakeLists.txt includes this module once and calls:
#
#   pulse_find_runtime()      # locate Pulse, define the pexip::pulse target
#   pulse_declare_imgui()     # fetch Dear ImGui + build the shared `imgui` lib
#
# Each demo's CMakeLists.txt then calls:
#
#   pulse_demo_rpath(<target>)              # find libpexpulse.so at run time
#   pulse_demo_launcher(<target> <script>)  # emit build/<script> wrapper
# ---------------------------------------------------------------------------

# pulse_find_runtime()
#
# Locate the Pexip Pulse library + headers and expose them as the imported
# target `pexip::pulse`. Implemented as a macro so the cache variables and the
# imported target it defines land in the caller's (root) scope and stay visible
# to every add_subdirectory(demos/...).
#
# Search order:
#   * headers : ${PEXIP_PREFIX}/include, then the in-repo SDK copy under
#               sdk/linux/opt/pexip/include.
#   * library : ${PEXIP_PREFIX}/lib (Linux .deb install), then the in-repo
#               macOS dylibs under sdk/macos.
#
# Sets in the caller scope:
#   PEXPULSE_INCLUDE_DIR, PEXPULSE_LIBRARY, PEXPULSE_LIBDIR
macro(pulse_find_runtime)
    set(PEXIP_PREFIX "/opt/pexip" CACHE PATH
        "Install prefix of the Pexip Pulse package")

    find_path(PEXPULSE_INCLUDE_DIR
        NAMES pexpulse/pulse.h
        HINTS "${PEXIP_PREFIX}/include"
              "${CMAKE_SOURCE_DIR}/sdk/linux/opt/pexip/include"
        REQUIRED)

    find_library(PEXPULSE_LIBRARY
        NAMES pexpulse
        HINTS "${PEXIP_PREFIX}/lib"
              "${CMAKE_SOURCE_DIR}/sdk/macos"
        REQUIRED)

    message(STATUS "Found pexpulse headers: ${PEXPULSE_INCLUDE_DIR}")
    message(STATUS "Found pexpulse library: ${PEXPULSE_LIBRARY}")

    # The directory that actually holds libpexpulse + its private siblings
    # (libpexlgpl, libimf, libonnxruntime.so.1, ...). Used to bake an RPATH and
    # to point the launcher scripts at the right place — works whether Pulse came
    # from /opt/pexip/lib (Linux) or the repo's sdk/macos folder (macOS).
    get_filename_component(PEXPULSE_LIBDIR "${PEXPULSE_LIBRARY}" DIRECTORY)

    # The Pulse install prefix (the parent of lib/, e.g. /opt/pexip). The runtime
    # needs this exported as PEX_BASE_PATH to locate its models, gstreamer plugins
    # and friends; without it Pulse aborts at startup. The launcher scripts below
    # set it for the user.
    get_filename_component(PEXPULSE_PREFIX "${PEXPULSE_LIBDIR}" DIRECTORY)

    # On macOS the shipped dylibs carry absolute install names from Pexip's build
    # machine; rewrite them to be relocatable so the demos can actually launch.
    # This may repoint PEXPULSE_LIBRARY / PEXPULSE_LIBDIR at the patched copies
    # (PEXPULSE_PREFIX is intentionally left at the original install above so
    # PEX_BASE_PATH keeps resolving the runtime's models/plugins).
    if(APPLE)
        pulse_relocate_macos_dylibs()
    endif()

    if(NOT TARGET pexip::pulse)
        add_library(pexip::pulse SHARED IMPORTED)
        set_target_properties(pexip::pulse PROPERTIES
            IMPORTED_LOCATION "${PEXPULSE_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${PEXPULSE_INCLUDE_DIR}")
    endif()
endmacro()

# pulse_relocate_macos_dylibs()
#
# Make the macOS Pulse dylibs relocatable.
#
# The dylibs shipped under sdk/macos/ were linked and signed on Pexip's build
# machine, so their Mach-O install names (LC_ID_DYLIB) — and libpexpulse's
# reference to libpexlgpl — are *absolute* paths under
# /Users/Shared/actions-runner/... that exist on no other machine. Because those
# load paths are absolute rather than @rpath-relative, neither the RPATH every
# demo bakes in nor the launcher's DYLD_LIBRARY_PATH can redirect them: dyld
# looks only at the hard-coded path, fails to find Pulse, and the exec() of the
# (perfectly good) demo binary fails with ENOENT — which the shell surfaces as
# the baffling "cannot execute: No such file or directory".
#
# Fix it by copying the dylibs into the build tree, rewriting their install
# names to @rpath/<leaf> (and the inter-library reference to match), re-signing
# them ad-hoc (editing a Mach-O invalidates its signature, which Apple Silicon
# then refuses to load), and pointing the rest of the build at those copies.
# Every demo already bakes PEXPULSE_LIBDIR into its RPATH, so @rpath resolves.
#
# Implemented as a macro so the PEXPULSE_LIBRARY / PEXPULSE_LIBDIR it updates
# land back in pulse_find_runtime's (root) scope. No-op (leaves everything
# untouched) if the expected dylibs or the Xcode command-line tools are missing.
macro(pulse_relocate_macos_dylibs)
    set(_pulse_src "${PEXPULSE_LIBRARY}")
    set(_lgpl_src  "${PEXPULSE_LIBDIR}/libpexlgpl.dylib")

    find_program(INSTALL_NAME_TOOL install_name_tool)
    find_program(CODESIGN codesign)
    find_program(OTOOL otool)

    if(EXISTS "${_pulse_src}" AND EXISTS "${_lgpl_src}"
       AND INSTALL_NAME_TOOL AND CODESIGN AND OTOOL)
        set(_reloc_dir "${CMAKE_BINARY_DIR}/pulse-macos-runtime")
        set(_pulse_dst "${_reloc_dir}/libpexpulse.dylib")
        set(_lgpl_dst  "${_reloc_dir}/libpexlgpl.dylib")

        # Start from pristine copies — configure_file re-copies whenever the
        # source dylib changes, so re-running CMake always re-patches cleanly.
        configure_file("${_pulse_src}" "${_pulse_dst}" COPYONLY)
        configure_file("${_lgpl_src}"  "${_lgpl_dst}"  COPYONLY)

        # Helper: run a relocation command and abort with a clear diagnostic if
        # it fails, rather than letting demos mysteriously fail to launch later.
        macro(_pulse_run_or_die)
            execute_process(COMMAND ${ARGN} RESULT_VARIABLE _rc)
            if(NOT _rc EQUAL 0)
                message(FATAL_ERROR
                    "macOS Pulse dylib relocation step failed (exit ${_rc}): ${ARGN}")
            endif()
        endmacro()

        # Discover libpexpulse's current (absolute) reference to libpexlgpl so we
        # can rewrite exactly that load command.
        execute_process(
            COMMAND "${OTOOL}" -L "${_pulse_dst}"
            OUTPUT_VARIABLE _pulse_deps
            RESULT_VARIABLE _otool_rc)
        if(NOT _otool_rc EQUAL 0)
            message(FATAL_ERROR "otool -L on ${_pulse_dst} failed (exit ${_otool_rc})")
        endif()
        string(REGEX MATCH "[^ \t\r\n]*libpexlgpl\\.dylib" _lgpl_ref "${_pulse_deps}")
        if(NOT _lgpl_ref)
            message(WARNING "Could not find libpexlgpl reference in ${_pulse_dst}; "
                            "leaving libpexpulse's inter-library reference unchanged.")
        endif()

        # libpexlgpl: id -> @rpath/libpexlgpl.dylib, then re-sign.
        _pulse_run_or_die("${INSTALL_NAME_TOOL}" -id "@rpath/libpexlgpl.dylib" "${_lgpl_dst}")
        _pulse_run_or_die("${CODESIGN}" --force --sign - "${_lgpl_dst}")

        # libpexpulse: id -> @rpath/libpexpulse.dylib, fix its libpexlgpl
        # reference to @rpath/libpexlgpl.dylib, then re-sign.
        _pulse_run_or_die("${INSTALL_NAME_TOOL}" -id "@rpath/libpexpulse.dylib" "${_pulse_dst}")
        if(_lgpl_ref)
            _pulse_run_or_die("${INSTALL_NAME_TOOL}"
                -change "${_lgpl_ref}" "@rpath/libpexlgpl.dylib" "${_pulse_dst}")
        endif()
        _pulse_run_or_die("${CODESIGN}" --force --sign - "${_pulse_dst}")

        set(PEXPULSE_LIBRARY "${_pulse_dst}")
        set(PEXPULSE_LIBDIR  "${_reloc_dir}")
        message(STATUS "Relocated macOS Pulse dylibs into ${_reloc_dir}")
    endif()
endmacro()

# pulse_declare_imgui()
#
# Fetch Dear ImGui (the -docking branch, a superset that every demo builds
# happily against) and compile the core + GLFW/OpenGL3 backends into a shared
# static `imgui` target. The lighter demos (doppler, gateway) link this target
# directly; the heavier ones (sip, pexninja) build their own ImGui variant but
# reuse the already-populated imgui_SOURCE_DIR. Macro so imgui_SOURCE_DIR and
# the `imgui` target are visible to the demo subdirectories.
macro(pulse_declare_imgui)
    include(FetchContent)
    set(FETCHCONTENT_QUIET OFF)
    FetchContent_Declare(
        imgui
        GIT_REPOSITORY https://github.com/ocornut/imgui.git
        GIT_TAG        v1.91.5-docking
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(imgui)

    if(NOT TARGET imgui)
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
        # macOS deprecates every OpenGL symbol; silence the backend's warnings so
        # the shared imgui lib builds cleanly there too.
        if(APPLE)
            target_compile_definitions(imgui PUBLIC GL_SILENCE_DEPRECATION)
        endif()
    endif()
endmacro()

# pulse_demo_rpath(<target>)
#
# Bake the Pulse library directory into the target's RPATH so the direct
# dependency libpexpulse.so resolves at run time without LD_LIBRARY_PATH.
function(pulse_demo_rpath target)
    set_target_properties(${target} PROPERTIES
        BUILD_RPATH   "${PEXPULSE_LIBDIR}"
        INSTALL_RPATH "${PEXPULSE_LIBDIR}")
endfunction()

# pulse_demo_launcher(<target> <script-name>)
#
# Generate ${CMAKE_BINARY_DIR}/<script-name>, a tiny wrapper that puts the Pulse
# lib directory on the dynamic-linker search path (so Pulse's *transitive*
# private siblings — libpexlgpl, libimf, libonnxruntime.so.1, ... — are found),
# exports PEX_BASE_PATH (the Pulse runtime aborts at startup without it), and
# then execs the demo binary. Lets users just run ./build/<script-name>.
function(pulse_demo_launcher target script)
    file(GENERATE
        OUTPUT  "${CMAKE_BINARY_DIR}/${script}"
        CONTENT "#!/bin/sh
# Auto-generated by CMake — puts Pulse's private siblings (libpexlgpl,
# libimf, libonnxruntime.so.1, ...) on the linker search path, points
# PEX_BASE_PATH at the Pulse install prefix (required by the runtime), then
# execs the demo binary.
export LD_LIBRARY_PATH=\"${PEXPULSE_LIBDIR}:\${LD_LIBRARY_PATH}\"
export DYLD_LIBRARY_PATH=\"${PEXPULSE_LIBDIR}:\${DYLD_LIBRARY_PATH}\"
export PEX_BASE_PATH=\"\${PEX_BASE_PATH:-${PEXPULSE_PREFIX}}\"
exec \"$<TARGET_FILE:${target}>\" \"$@\"
"
        FILE_PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE
                         GROUP_READ GROUP_EXECUTE
                         WORLD_READ WORLD_EXECUTE)
endfunction()
