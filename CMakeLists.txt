cmake_minimum_required(VERSION 3.25)  # FetchContent SYSTEM

project(sega_emu
    VERSION 0.1.0
    DESCRIPTION "SEGA Mega Drive / Genesis emulator"
    LANGUAGES C CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(MD_BUILD_FRONTEND "Build the SDL3 frontend executable"            ON)
option(MD_BUILD_TESTS    "Build core unit tests (no SDL dependency)"      ON)
option(MD_FETCH_SDL      "Always build SDL3 from source instead of using an installed package" OFF)
option(MD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"          OFF)

# ---------------------------------------------------------------------------
# Global settings
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Shared warning settings, applied to our own targets only (never to SDL).
add_library(md_warnings INTERFACE)
add_library(md::warnings ALIAS md_warnings)
if(MSVC)
    target_compile_options(md_warnings INTERFACE /W4 /permissive-
        $<$<BOOL:${MD_WARNINGS_AS_ERRORS}>:/WX>)
else()
    target_compile_options(md_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        $<$<BOOL:${MD_WARNINGS_AS_ERRORS}>:-Werror>)
endif()

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
add_subdirectory(core)          # md::core  – pure C++, no SDL

if(MD_BUILD_FRONTEND)
    add_subdirectory(frontend)  # megadrive – SDL3 executable
endif()

if(MD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
