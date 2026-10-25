cmake_minimum_required(VERSION 3.16)
project(chost VERSION 0.5.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CHOST_BUILD_GUI "Build the Qt GUI" ON)

# libcarla: from pkg-config on Linux, or from a Carla install/build dir given with CARLA_ROOT
set(CARLA_ROOT "" CACHE PATH "Directory holding Carla headers (include/carla) and libraries (lib/carla)")
add_library(carla INTERFACE)
if(CARLA_ROOT)
    target_include_directories(carla INTERFACE
        "${CARLA_ROOT}/include/carla" "${CARLA_ROOT}/include/carla/includes" "${CARLA_ROOT}/include/carla/utils")
    # MinGW links straight against the DLLs shipped in Carla's Windows release
    foreach(name standalone2 utils)
        string(TOUPPER "CARLA_${name}_LIB" var)
        find_file(${var} NAMES libcarla_${name}.dll libcarla_${name}.so libcarla_${name}.dylib
                  PATHS "${CARLA_ROOT}/lib/carla" "${CARLA_ROOT}/lib" REQUIRED NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
    endforeach()
    target_link_libraries(carla INTERFACE "${CARLA_STANDALONE2_LIB}" "${CARLA_UTILS_LIB}")
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(CARLA REQUIRED IMPORTED_TARGET carla-standalone carla-utils)
    target_link_libraries(carla INTERFACE PkgConfig::CARLA)
endif()

find_package(Threads REQUIRED)

add_library(chost_core STATIC src/core/HostEngine.cpp src/core/AudioConvert.cpp src/core/MidiFile.cpp src/core/Session.cpp)
target_include_directories(chost_core PUBLIC src)
target_include_directories(chost_core PRIVATE third_party/dr_libs)
target_compile_definitions(chost_core PUBLIC REAL_BUILD)
target_link_libraries(chost_core PUBLIC carla Threads::Threads)
if(NOT MSVC)
    target_compile_options(chost_core PRIVATE -Wall -Wextra)
endif()

add_executable(chost-cli src/cli/main.cpp)
target_link_libraries(chost-cli PRIVATE chost_core)
install(TARGETS chost-cli RUNTIME DESTINATION bin)

if(CHOST_BUILD_GUI)
    find_package(Qt6 REQUIRED COMPONENTS Widgets)
    set(CMAKE_AUTOMOC ON)
    add_executable(chost WIN32
        src/gui/main.cpp
        src/gui/MainWindow.cpp src/gui/MainWindow.h
        src/gui/PluginWindows.cpp src/gui/PluginWindows.h
        src/gui/StemDialog.cpp src/gui/StemDialog.h
        src/gui/Resources.cpp src/gui/Resources.h
        src/gui/TranscribeDialog.cpp src/gui/TranscribeDialog.h
        src/gui/Theme.cpp src/gui/Theme.h
        src/gui/Widgets.cpp src/gui/Widgets.h)
    target_compile_definitions(chost PRIVATE CHOST_VERSION="${PROJECT_VERSION}" CHOST_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(chost PRIVATE chost_core Qt6::Widgets)
    install(TARGETS chost RUNTIME DESTINATION bin)
endif()
