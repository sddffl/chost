cmake_minimum_required(VERSION 3.16)
project(chost VERSION 0.21.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CHOST_BUILD_GUI "Build the Qt GUI" ON)

# libcarla: from pkg-config on Linux, or from a Carla install/build dir given with CARLA_ROOT
# carla: the standalone host for chost and chost-cli; carla_render: carla's engine as a plugin, for chost-render
set(CARLA_ROOT "" CACHE PATH "Directory holding Carla headers (include/carla) and libraries (lib/carla)")
add_library(carla_headers INTERFACE)
add_library(carla INTERFACE)
add_library(carla_render INTERFACE)
if(CARLA_ROOT)
    target_include_directories(carla_headers INTERFACE
        "${CARLA_ROOT}/include/carla" "${CARLA_ROOT}/include/carla/includes" "${CARLA_ROOT}/include/carla/utils")
    # MinGW links straight against the DLLs shipped in Carla's Windows release
    foreach(name standalone2 utils host-plugin)
        string(TOUPPER "CARLA_${name}_LIB" var)
        string(REPLACE "-" "_" var "${var}")
        find_file(${var} NAMES libcarla_${name}.dll libcarla_${name}.so libcarla_${name}.dylib
                  PATHS "${CARLA_ROOT}/lib/carla" "${CARLA_ROOT}/lib" REQUIRED NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    endforeach()
    target_link_libraries(carla INTERFACE carla_headers "${CARLA_STANDALONE2_LIB}" "${CARLA_UTILS_LIB}")
    # the host-plugin library holds the engine as a plugin and the host API for it, one copy of carla
    target_link_libraries(carla_render INTERFACE carla_headers "${CARLA_HOST_PLUGIN_LIB}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CARLA REQUIRED IMPORTED_TARGET carla-standalone carla-utils)
    pkg_check_modules(CARLA_PLUGIN REQUIRED IMPORTED_TARGET carla-host-plugin)
    target_link_libraries(carla INTERFACE PkgConfig::CARLA)
    target_link_libraries(carla_render INTERFACE PkgConfig::CARLA_PLUGIN)
endif()

find_package(Threads REQUIRED)

# the core is compiled once and linked against either flavour of carla
add_library(chost_core_objects OBJECT src/core/HostEngine.cpp src/core/AudioConvert.cpp src/core/MidiFile.cpp src/core/Session.cpp src/core/Chords.cpp src/core/Scales.cpp src/core/Score.cpp src/core/ClipEdit.cpp src/core/Click.cpp src/core/Loudness.cpp src/core/MixBalance.cpp)
target_include_directories(chost_core_objects PUBLIC src)
target_include_directories(chost_core_objects PRIVATE third_party/dr_libs)
target_compile_definitions(chost_core_objects PUBLIC REAL_BUILD)
if(CARLA_ROOT)
    target_link_libraries(chost_core_objects PUBLIC carla_headers)
else()
    target_include_directories(chost_core_objects PUBLIC ${CARLA_INCLUDE_DIRS})
endif()
if(NOT MSVC)
    target_compile_options(chost_core_objects PRIVATE -Wall -Wextra)
endif()

add_library(chost_core STATIC $<TARGET_OBJECTS:chost_core_objects>)
target_include_directories(chost_core PUBLIC src)
target_compile_definitions(chost_core PUBLIC REAL_BUILD)
target_link_libraries(chost_core PUBLIC carla Threads::Threads)

# offline mixdown: renders a session faster than real time, in its own process
add_executable(chost-render src/render/main.cpp src/core/OfflineEngine.cpp $<TARGET_OBJECTS:chost_core_objects>)
target_include_directories(chost-render PRIVATE src third_party/dr_libs)
target_compile_definitions(chost-render PRIVATE REAL_BUILD)
target_link_libraries(chost-render PRIVATE carla_render Threads::Threads)
install(TARGETS chost-render RUNTIME DESTINATION bin)

add_executable(chost-cli src/cli/main.cpp)
target_link_libraries(chost-cli PRIVATE chost_core)
install(TARGETS chost-cli RUNTIME DESTINATION bin)

if(CHOST_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets Network)
    set(CMAKE_AUTOMOC ON)
    set(CMAKE_AUTORCC ON)
    # the app icon: drawn in resources/icons/chost.svg (chost-small.svg for 16 and 24 px)
    set(CHOST_ICON resources/icons/chost.qrc)
    if(WIN32)
        enable_language(RC)
        list(APPEND CHOST_ICON resources/icons/chost.rc)
    endif()
    add_executable(chost WIN32 ${CHOST_ICON} resources/fonts/fonts.qrc
        src/gui/main.cpp
        src/gui/MainWindow.cpp src/gui/MainWindow.h
        src/gui/PluginWindows.cpp src/gui/PluginWindows.h
        src/gui/StemDialog.cpp src/gui/StemDialog.h
        src/gui/Resources.cpp src/gui/Resources.h
        src/gui/TranscribeDialog.cpp src/gui/TranscribeDialog.h
        src/gui/PluginDiscovery.cpp src/gui/PluginDiscovery.h
        src/gui/Instruments.cpp src/gui/Instruments.h
        src/gui/InstrumentDialog.cpp src/gui/InstrumentDialog.h
        src/gui/PianoRoll.cpp src/gui/PianoRoll.h
        src/gui/Theme.cpp src/gui/Theme.h
        src/gui/Widgets.cpp src/gui/Widgets.h
        src/gui/RenderJob.cpp src/gui/RenderJob.h
        src/gui/ScriptJob.cpp src/gui/ScriptJob.h
        src/gui/ExportDialog.cpp src/gui/ExportDialog.h
        src/gui/BalanceDialog.cpp src/gui/BalanceDialog.h
        src/gui/MasterDialog.cpp src/gui/MasterDialog.h
        src/gui/ArrangeDialog.cpp src/gui/ArrangeDialog.h
        src/gui/TuneDialog.cpp src/gui/TuneDialog.h
        src/gui/History.cpp src/gui/History.h
        src/gui/Autosave.cpp src/gui/Autosave.h
        src/gui/Control.cpp src/gui/Control.h
        src/gui/ClaudeConnect.cpp src/gui/ClaudeConnect.h
        src/gui/ChordPad.cpp src/gui/ChordPad.h
        src/gui/ScoreView.cpp src/gui/ScoreView.h
        src/gui/ScoreDialog.cpp src/gui/ScoreDialog.h
        src/control/Tools.cpp src/control/Tools.h)
    target_compile_definitions(chost PRIVATE CHOST_VERSION="${PROJECT_VERSION}" CHOST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(chost PRIVATE chost_core Qt6::Widgets Qt6::Network)
    install(TARGETS chost RUNTIME DESTINATION bin)

    # chost's MCP server for AI assistants (Claude Desktop, Claude Code, ...): a console program they start,
    # passing their tool calls on to the chost that runs (and starting one when none does)
    add_executable(chost-mcp src/mcp/main.cpp src/control/Tools.cpp src/control/Tools.h)
    target_include_directories(chost-mcp PRIVATE src)
    target_compile_definitions(chost-mcp PRIVATE CHOST_VERSION="${PROJECT_VERSION}")
    target_link_libraries(chost-mcp PRIVATE Qt6::Core Qt6::Network)
    install(TARGETS chost-mcp RUNTIME DESTINATION bin)
endif()
