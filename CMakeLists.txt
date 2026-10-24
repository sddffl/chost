cmake_minimum_required(VERSION 3.16)
project(chost VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(PkgConfig REQUIRED)
pkg_check_modules(CARLA REQUIRED IMPORTED_TARGET carla-standalone carla-utils)
find_package(Threads REQUIRED)

add_executable(chost src/main.cpp)
target_compile_definitions(chost PRIVATE REAL_BUILD)
target_link_libraries(chost PRIVATE PkgConfig::CARLA Threads::Threads)
target_compile_options(chost PRIVATE -Wall -Wextra)

install(TARGETS chost RUNTIME DESTINATION bin)
