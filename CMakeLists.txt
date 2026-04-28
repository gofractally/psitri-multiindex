cmake_minimum_required(VERSION 3.20)
project(psitri-multiindex VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(PSITRI_MULTIINDEX_BUILD_TESTS "Build psitri-multiindex tests" ON)

# ─── External dependencies ────────────────────────────────────────────────
#
# FetchContent declarations. These are no-ops when a parent project
# (e.g. spring) has already declared the same name first — CMake's
# "first declaration wins" rule keeps the parent's pin authoritative
# and makes the parent's external/ checkout the single source.
#
# Standalone build: declarations fire here; sources land in this
# project's external/ directory and are reused across build dirs.
#
# Local-dev override (e.g. you're hacking on psio):
#   cmake -DFETCHCONTENT_SOURCE_DIR_PSIO=$HOME/projects/psio ...
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

FetchContent_Declare(psitri
   GIT_REPOSITORY https://github.com/gofractally/arbtrie.git
   GIT_TAG        main
   SOURCE_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/external/psitri)

FetchContent_Declare(psio
   GIT_REPOSITORY https://github.com/gofractally/psio.git
   GIT_TAG        main
   SOURCE_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/external/psio)

# psitri's CMake assumes it's the root project and registers its own
# tests/benchmarks. Suppress that when we add it as a subdirectory.
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(psitri psio)

# Re-enable our own tests after psitri silenced the global flag.
if(PSITRI_MULTIINDEX_BUILD_TESTS)
   set(BUILD_TESTS ON)
endif()

# ─── psitri-multiindex library ────────────────────────────────────────────
add_library(psitri_multiindex INTERFACE)
target_include_directories(psitri_multiindex INTERFACE
   $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(psitri_multiindex INTERFACE psitri psio)

# ─── Tests ────────────────────────────────────────────────────────────────
if(PSITRI_MULTIINDEX_BUILD_TESTS)
   enable_testing()
   add_subdirectory(tests)
endif()
