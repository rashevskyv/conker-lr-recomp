cmake_minimum_required(VERSION 3.20)

# ============================================================================
# Conker: Live & Reloaded (Xbox, Rare 2005) - Static Recompilation
#
# Hosts: Windows (MSVC, D3D11), Linux (GCC, SDL2 + OpenGL), Nintendo
# Switch homebrew (devkitA64 + libnx, built with switch/build.sh) and
# Android arm64 (NDK, SDL2 + Vulkan, built with android/build.sh).
# ============================================================================

project(clx_recomp C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

set(XBOXRECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/xboxrecomp" CACHE PATH
    "Path to the xboxrecomp toolkit root directory")

# The lifted code is ~100 MB of C generated from the retail XBE. It is never
# committed; point this at wherever tools.recomp --gen-dir wrote it.
set(NFSU2_GEN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp/gen" CACHE PATH
    "Directory holding the tools.recomp output (recomp_*.c, recomp_funcs.h)")

if(NOT EXISTS "${NFSU2_GEN_DIR}/recomp_funcs.h")
    message(FATAL_ERROR "No generated code in ${NFSU2_GEN_DIR}. Run tools.recomp "
                        "with --gen-dir, or pass -DNFSU2_GEN_DIR=<dir>.")
endif()

# The Vulkan renderer (xboxrecomp nv2a_vk) instead of OpenGL. On the Switch it
# links mesa-switch's static NVK (NVK_SDK) and builds clx-vulkan.nro.
option(NFSU2_VULKAN "Render with Vulkan instead of OpenGL" OFF)
set(XBOXRECOMP_VULKAN ${NFSU2_VULKAN} CACHE BOOL "" FORCE)

# Windows: MinGW builds with the OpenGL pushbuffer renderer (see xboxrecomp/CMakeLists.txt);
# MSVC keeps the toolkit's D3D11 layer, which does not run this title.
if(WIN32 AND NOT MSVC)
    set(XBOXRECOMP_WIN_GL ON CACHE BOOL "" FORCE)
endif()

add_subdirectory(${XBOXRECOMP_DIR} ${CMAKE_BINARY_DIR}/xboxrecomp)

file(GLOB RECOMP_GEN_SOURCES CONFIGURE_DEPENDS "${NFSU2_GEN_DIR}/*.c")

set(GAME_SOURCES
    src/main.c
    src/recomp_manual.c
    src/switch_nx.c
    src/switch_saltynx.c
    src/clx_options.c
    src/clx_menu.c
    src/clx_rumble.c
    src/clx_buttons.c
    src/clx_mouse.c
    src/clx_gyro.c
)

if(ANDROID)
    # SDLActivity loads libmain.so and calls its SDL_main (src/android.c).
    list(APPEND GAME_SOURCES src/android.c)
    add_library(${PROJECT_NAME} SHARED ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
    set_target_properties(${PROJECT_NAME} PROPERTIES OUTPUT_NAME main)
    target_link_libraries(${PROJECT_NAME} PRIVATE log android dl)
    # No undefined symbols left for the loader to find at run time; segments
    # aligned for phones with 16 KB pages.
    target_link_options(${PROJECT_NAME} PRIVATE "LINKER:--no-undefined"
                        "LINKER:-z,max-page-size=16384")
elseif(MSVC)
    add_executable(${PROJECT_NAME} WIN32 ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
else()
    add_executable(${PROJECT_NAME} ${GAME_SOURCES} ${RECOMP_GEN_SOURCES})
endif()

target_link_libraries(${PROJECT_NAME} PRIVATE xboxrecomp)

# Movies: VP6 decoded by FFmpeg instead of the lifted On2 decoder
# (src/movie_vp6.c). NFSU2_FFMPEG_DIR is an install of the minimal LGPL
# build from tools/build_ffmpeg_vp6.sh (libavcodec with only the vp6
# decoder); without it the lifted decoder plays the movies.
set(NFSU2_FFMPEG_DIR "" CACHE PATH "FFmpeg (vp6 decoder) install prefix")
if(NFSU2_FFMPEG_DIR)
    if(NOT EXISTS "${NFSU2_FFMPEG_DIR}/lib/libavcodec.a")
        message(FATAL_ERROR "no libavcodec.a in ${NFSU2_FFMPEG_DIR}/lib (tools/build_ffmpeg_vp6.sh)")
    endif()
    target_sources(${PROJECT_NAME} PRIVATE src/movie_vp6.c)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NFSU2_NATIVE_VP6=1)
    # BEFORE: the Switch portlibs include dir has devkitPro's (GPL) FFmpeg headers.
    target_include_directories(${PROJECT_NAME} BEFORE PRIVATE "${NFSU2_FFMPEG_DIR}/include")
    target_link_libraries(${PROJECT_NAME} PRIVATE
        "${NFSU2_FFMPEG_DIR}/lib/libavcodec.a" "${NFSU2_FFMPEG_DIR}/lib/libavutil.a")
endif()

if(NFSU2_VULKAN)
    target_compile_definitions(${PROJECT_NAME} PRIVATE NFSU2_VULKAN=1)
endif()

# The tiled aperture (0xF0000000) is folded on every guest access on the
# Switch (recomp_types.h default). FlatOut 2 turned that off
# (RECOMP_FOLD_TILED=0) after checking on Linux with RECOMP_NO_TILED=1 that
# only its device-aware libraries touch the aperture. TODO: check the same
# for Conker before turning the fold off here.

target_include_directories(${PROJECT_NAME} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${NFSU2_GEN_DIR}
    ${XBOXRECOMP_DIR}/include
)

if(WIN32)
    if(XBOXRECOMP_WIN_GL)
        target_compile_definitions(${PROJECT_NAME} PRIVATE CLX_PC_GL=1)
        target_link_libraries(${PROJECT_NAME} PRIVATE winmm dbghelp ws2_32 bcrypt)
    else()
        target_link_libraries(${PROJECT_NAME} PRIVATE
            d3d11 dxgi dxguid xinput winmm dbghelp)
    endif()
    if(MSVC)
        target_link_options(${PROJECT_NAME} PRIVATE /LARGEADDRESSAWARE)
        target_compile_options(${PROJECT_NAME} PRIVATE /bigobj)
    endif()
endif()
if(NOT MSVC)
    # The generated chunks are huge straight-line functions; -O1 keeps GCC's
    # memory and time per chunk sane while still allocating registers. The
    # Switch's and a phone's CPU is the bottleneck there, so they get -O2.
    if(NINTENDO_SWITCH OR ANDROID)
        set(NFSU2_GEN_OPT "-O2" CACHE STRING "Optimisation for the lifted code")
    else()
        set(NFSU2_GEN_OPT "-O1" CACHE STRING "Optimisation for the lifted code")
    endif()
    # NFSU2_LTO: link-time optimisation of the lifted code and its manual
    # overrides, so lifted callers inline each other and the native leaves
    # (_ftol2, ...). The runtime and renderers are compiled as usual.
    # NFSU2_LTO_JOBS parallel LTRANS jobs (memory: ~1-2 GB each).
    option(NFSU2_LTO "Link-time optimisation of the lifted code" OFF)
    set(NFSU2_LTO_JOBS "4" CACHE STRING "Parallel LTO link jobs")
    set(NFSU2_GEN_FLAGS "${NFSU2_GEN_OPT};-w;-fno-strict-aliasing;-fwrapv")
    if(NFSU2_LTO)
        list(APPEND NFSU2_GEN_FLAGS "-flto=auto")
        set_property(SOURCE src/recomp_manual.c APPEND PROPERTY
            COMPILE_OPTIONS "-flto=auto;-fno-strict-aliasing;-fwrapv")
        target_link_options(${PROJECT_NAME} PRIVATE
            "-flto=${NFSU2_LTO_JOBS}" "-flto-partition=balanced" ${NFSU2_GEN_OPT})
    endif()
    set_source_files_properties(${RECOMP_GEN_SOURCES} PROPERTIES
        COMPILE_OPTIONS "${NFSU2_GEN_FLAGS}")
    target_link_libraries(${PROJECT_NAME} PRIVATE m)
endif()

# ── Nintendo Switch homebrew (devkitA64 + libnx) ────────────────
#
# Build with switch/build.sh. The NRO reads the extracted disc from
# sdmc:/switch/clx/game/ (never an ISO); logs and clx_env.txt live in
# sdmc:/switch/clx/.
if(NINTENDO_SWITCH)
    set(NFSU2_SWITCH_APP_NAME "Conker: Live & Reloaded (Xbox recomp)" CACHE STRING
        "Title shown in the homebrew menu")
    set(NFSU2_SWITCH_AUTHOR "conker-lr-xbox" CACHE STRING "Author shown in the homebrew menu")
    # The release lives here, not in the cache: a cached default stays at
    # whatever the build directory was first configured with.
    # -DNFSU2_SWITCH_VERSION_OVERRIDE=x sets another one.
    # From git, so the homebrew menu tells builds apart: 0.<commit count>.<sha7>
    # ("0.192.82a7bca"); the Vulkan NRO 0.<count>.<sha6>-vk ("0.192.82a7bc-vk"):
    # nacptool keeps 15 bytes of the version. Configured on every switch/build.sh run.
    execute_process(COMMAND git -c safe.directory=* rev-list --count HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE CLX_GIT_COUNT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND git -c safe.directory=* rev-parse --short=7 HEAD
                    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
                    OUTPUT_VARIABLE CLX_GIT_SHA OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(NOT CLX_GIT_COUNT OR NOT CLX_GIT_SHA)
        set(CLX_GIT_COUNT 0)
        set(CLX_GIT_SHA nogit)
    endif()
    if(NFSU2_VULKAN)
        string(SUBSTRING "${CLX_GIT_SHA}" 0 6 CLX_GIT_SHA)
        set(NFSU2_SWITCH_VERSION "0.${CLX_GIT_COUNT}.${CLX_GIT_SHA}-vk")
    else()
        set(NFSU2_SWITCH_VERSION "0.${CLX_GIT_COUNT}.${CLX_GIT_SHA}")
    endif()
    if(NFSU2_VULKAN)
        set(NFSU2_SWITCH_APP_NAME "${NFSU2_SWITCH_APP_NAME}, Vulkan")
    endif()
    if(NFSU2_SWITCH_VERSION_OVERRIDE)
        set(NFSU2_SWITCH_VERSION "${NFSU2_SWITCH_VERSION_OVERRIDE}")
    endif()
    set(NFSU2_SWITCH_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/icon.jpg" CACHE FILEPATH
        "256x256 JPEG icon (libnx default when empty)")
    # Icon and logo come from the user's XBE (switch/build.sh runs make_logo.py).
    if(NOT NFSU2_VULKAN AND NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/logo.h")
        message(FATAL_ERROR "src/logo.h missing: python3 tools/make_logo.py <game>/default.xbe")
    endif()
    if(NOT EXISTS "${NFSU2_SWITCH_ICON}")
        message(WARNING "${NFSU2_SWITCH_ICON} missing (tools/make_logo.py makes it): libnx's default icon")
        set(NFSU2_SWITCH_ICON "")
    endif()

    if(NFSU2_VULKAN)
        # No GL: SDL is only the audio output; NVK comes with nv2a_vk.
        target_link_libraries(${PROJECT_NAME} PRIVATE nx m)
    else()
        # switch-sdl2's GL path is EGL on switch-mesa.
        target_link_libraries(${PROJECT_NAME} PRIVATE EGL glapi drm_nouveau nx m)
    endif()
    # SDL's audio thread gets a real-time priority and audren gaps are
    # counted (switch_nx.c, "SDL's audio thread").
    target_link_options(${PROJECT_NAME} PRIVATE
        "LINKER:--wrap=SDL_SYS_SetThreadPriority"
        "LINKER:--wrap=audrvVoiceAddWaveBuf"
        "LINKER:--wrap=__syscall_thread_create")   # every pthread, paused at exit (switch_nx.c)
    set_property(SOURCE src/switch_nx.c APPEND PROPERTY
        INCLUDE_DIRECTORIES "${XBOXRECOMP_DIR}/src/apu")
    # switch_nx.c's loading screen asks EGL for the on-screen size.
    target_include_directories(${PROJECT_NAME} PRIVATE "$ENV{DEVKITPRO}/portlibs/switch/include")

    # Only switch_nx.c: on the target it would rebuild the lifted code every commit.
    set_property(SOURCE src/switch_nx.c APPEND PROPERTY
        COMPILE_DEFINITIONS "CLX_VERSION=\"${NFSU2_SWITCH_VERSION}\";CLX_BUILD_NO=\"${CLX_GIT_COUNT}\"")
    message(STATUS "clx version ${NFSU2_SWITCH_VERSION}")
    set(NFSU2_NACP "${CMAKE_CURRENT_BINARY_DIR}/clx.nacp")
    nx_generate_nacp(OUTPUT "${NFSU2_NACP}"
        NAME "${NFSU2_SWITCH_APP_NAME}"
        AUTHOR "${NFSU2_SWITCH_AUTHOR}"
        VERSION "${NFSU2_SWITCH_VERSION}")
    if(NFSU2_SWITCH_ICON)
        nx_create_nro(${PROJECT_NAME} NACP "${NFSU2_NACP}" ICON "${NFSU2_SWITCH_ICON}")
    else()
        nx_create_nro(${PROJECT_NAME} NACP "${NFSU2_NACP}")
    endif()
endif()
