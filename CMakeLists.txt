cmake_minimum_required(VERSION 3.20)

project(PulseForge
    DESCRIPTION "PulseForge - a programmable audio-reactive video synthesizer"
    VERSION 1.0.0
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Global build settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif ()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

# Static link of raylib + CrystalGUI into a single executable avoids shipping
# DLLs next to the binary; DLL builds are still possible with -DBUILD_SHARED_LIBS=ON
if (NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS OFF)
endif ()

find_program(PF_FFMPEG_EXE NAMES ffmpeg ffmpeg.exe)
find_program(PF_FFPROBE_EXE NAMES ffprobe ffprobe.exe)
if (PF_FFMPEG_EXE)
    message(STATUS "PulseForge: found ffmpeg at ${PF_FFMPEG_EXE}")
else ()
    message(WARNING "PulseForge: ffmpeg not found on PATH, export will fail until it is installed")
endif ()

# ---------------------------------------------------------------------------
# Dependencies: CrystalGUI (which itself pulls in raylib)
# ---------------------------------------------------------------------------
if (NOT DEFINED CRYSTALGUI_DIR)
    if (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crystalgui/CMakeLists.txt")
        set(CRYSTALGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/crystalgui")
    else ()
        # Falls back to the sibling checkout used on this machine.
        set(CRYSTALGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../crystalgui")
    endif ()
    set(CRYSTALGUI_DIR "${CRYSTALGUI_DIR}" CACHE PATH "Path to the CrystalGUI source tree")
endif ()

if (NOT EXISTS "${CRYSTALGUI_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "CrystalGUI sources not found at ${CRYSTALGUI_DIR}")
endif ()

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_DOCUMENTATION OFF CACHE BOOL "" FORCE)
set(BUILD_PACKAGE OFF CACHE BOOL "" FORCE)

add_subdirectory("${CRYSTALGUI_DIR}" crystalgui_build)

# ---------------------------------------------------------------------------
# PulseForge core library
# ---------------------------------------------------------------------------
add_library(pulseforge_core STATIC
    src/core/Port.cpp
    src/core/Node.cpp
    src/core/Graph.cpp
    src/core/Project.cpp
    src/core/Json.cpp
    src/core/TextEdit.cpp
    src/core/Registry.cpp
    src/dsp/AudioClip.cpp
    src/dsp/Fft.cpp
    src/dsp/Analysis.cpp
    src/dsp/Dynamics.cpp
    src/render/ShaderLibrary.cpp
    src/render/Renderer.cpp
    src/render/Geometry.cpp
    src/render/FrameReadback.cpp
    src/render/Palette.cpp
    src/export/FFmpeg.cpp
    src/export/Exporter.cpp
)

target_include_directories(pulseforge_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(pulseforge_core PUBLIC CrystalGUI raylib)

# Raw OpenGL access (glad ships with raylib and is compiled into libraylib).
target_include_directories(pulseforge_core PRIVATE
    "${CRYSTALGUI_DIR}/deps/raylib/src/external")

target_compile_definitions(pulseforge_core PUBLIC
    PF_FFMPEG_DEFAULT="${PF_FFMPEG_EXE}"
    PF_FFPROBE_DEFAULT="${PF_FFPROBE_EXE}"
)

if (MSVC)
    target_compile_options(pulseforge_core PRIVATE /W3 /permissive- /MP)
    target_compile_definitions(pulseforge_core PRIVATE _CRT_SECURE_NO_WARNINGS NOMINMAX)
else ()
    target_compile_options(pulseforge_core PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_options(pulseforge_core PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-march=native>)
endif ()

find_package(Threads REQUIRED)
target_link_libraries(pulseforge_core PUBLIC Threads::Threads)

if (WIN32)
    target_link_libraries(pulseforge_core PUBLIC winmm)
endif ()

# ---------------------------------------------------------------------------
# Application
# ---------------------------------------------------------------------------
option(PF_BUILD_APP "Build the PulseForge editor" ON)
if (PF_BUILD_APP)
    add_executable(PulseForge
        src/main.cpp
        src/ui/App.cpp
        src/ui/GraphCanvasWidget.cpp
        src/ui/NodeVisuals.cpp
        src/ui/TimelineWidget.cpp
        src/ui/InspectorWidget.cpp
        src/ui/PreviewWidget.cpp
        src/ui/BrowserDialog.cpp
        src/ui/Widgets.cpp
        src/ui/Theme.cpp
    )

    target_link_libraries(PulseForge PRIVATE pulseforge_core)

    if (MSVC)
        set_property(TARGET PulseForge PROPERTY VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}")
        target_link_options(PulseForge PRIVATE /ENTRY:mainCRTStartup)
    endif ()

    # Copy shader presets / assets next to the binary for convenience.
    add_custom_command(TARGET PulseForge POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_CURRENT_SOURCE_DIR}/assets"
                "$<TARGET_FILE_DIR:PulseForge>/assets"
        # CrystalGUI loads its shader and fonts from "resource/" relative to the
        # working directory, so keep a copy next to the executable.
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:PulseForge>/resource/shaders/glsl330"
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_FILE_DIR:PulseForge>/resource/fonts/Inter/static"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CRYSTALGUI_DIR}/resource/shaders/glsl330/box.fs"
                "$<TARGET_FILE_DIR:PulseForge>/resource/shaders/glsl330/"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CRYSTALGUI_DIR}/resource/fonts/Inter/static/Inter_18pt-Regular.ttf"
                "${CRYSTALGUI_DIR}/resource/fonts/Inter/static/Inter_18pt-Italic.ttf"
                "${CRYSTALGUI_DIR}/resource/fonts/Inter/static/Inter_18pt-Light.ttf"
                "${CRYSTALGUI_DIR}/resource/fonts/Inter/static/Inter_18pt-LightItalic.ttf"
                "${CRYSTALGUI_DIR}/resource/fonts/Inter/static/Inter_24pt-SemiBold.ttf"
                "${CRYSTALGUI_DIR}/resource/fonts/Inter/static/Inter_24pt-SemiBoldItalic.ttf"
                "$<TARGET_FILE_DIR:PulseForge>/resource/fonts/Inter/static/"
        COMMENT "Copying PulseForge assets and CrystalGUI resources"
    )
endif ()

option(PF_BUILD_TOOLS "Build PulseForge diagnostic tools" ON)
if (PF_BUILD_TOOLS)
    add_executable(pf_probe tools/probe.cpp)
    target_link_libraries(pf_probe PRIVATE pulseforge_core)
    # The probe talks raw OpenGL, which needs raylib's bundled glad headers.
    target_include_directories(pf_probe PRIVATE "${CRYSTALGUI_DIR}/deps/raylib/src/external")

    add_executable(pf_selftest tools/selftest.cpp)
    target_link_libraries(pf_selftest PRIVATE pulseforge_core)
    target_include_directories(pf_selftest PRIVATE "${CRYSTALGUI_DIR}/deps/raylib/src/external")

    add_executable(pf_stage tools/stage_dump.cpp)
    target_link_libraries(pf_stage PRIVATE pulseforge_core)
    target_include_directories(pf_stage PRIVATE "${CRYSTALGUI_DIR}/deps/raylib/src/external")

    add_executable(pf_migrate tools/migrate.cpp)
    target_link_libraries(pf_migrate PRIVATE pulseforge_core)
endif ()
