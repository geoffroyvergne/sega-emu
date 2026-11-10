# md::core — the emulation core.
#
# RULE: this target must never depend on SDL (or any other platform library).
# It exposes plain C++ types (framebuffer, controller state, audio samples)
# that a frontend consumes. tests/ links against it without SDL to enforce this.

include(FetchContent)

# ---------------------------------------------------------------------------
# Moira — Motorola 68000 core (MIT, https://github.com/dirkwhoffmann/Moira)
# ---------------------------------------------------------------------------
# Moira reads its compile-time options from MoiraConfig.h *next to its own
# sources* (quoted include), so an include-path override is not possible.
# Instead we copy our config over the upstream one at configure time (below).
set(MD_MOIRA_CONFIG ${CMAKE_CURRENT_SOURCE_DIR}/third_party/moira/MoiraConfig.h)

FetchContent_Declare(moira
    GIT_REPOSITORY https://github.com/dirkwhoffmann/Moira.git
    GIT_TAG        ce28239b0507ebfe1bf2ad2b9c9252dc2f2cc031  # master, 2026-09-23
    SOURCE_SUBDIR  Moira          # only the library, not the test runner / Cputester
    UPDATE_DISCONNECTED TRUE      # pinned commit: never re-fetch on configure
    SYSTEM                        # consumers see its headers as system headers (no warnings)
)
FetchContent_MakeAvailable(moira)

# Upstream's CMakeLists only lists sources; add what a consumer needs.
target_include_directories(moira SYSTEM PUBLIC ${moira_SOURCE_DIR}/Moira)
target_compile_features(moira PUBLIC cxx_std_20)
set_target_properties(moira PROPERTIES POSITION_INDEPENDENT_CODE ON)
# Upstream marks its -Wno-* flags PUBLIC; keep them on Moira only so they
# don't weaken warnings in our own code.
get_target_property(_moira_opts moira INTERFACE_COMPILE_OPTIONS)
if(_moira_opts)
    set_target_properties(moira PROPERTIES INTERFACE_COMPILE_OPTIONS "")
    target_compile_options(moira PRIVATE ${_moira_opts})
endif()

# Install our config into Moira's source dir. configure_file also makes CMake
# re-run (and Moira rebuild) whenever our copy changes.
configure_file(${MD_MOIRA_CONFIG} ${moira_SOURCE_DIR}/Moira/MoiraConfig.h COPYONLY)

# ---------------------------------------------------------------------------
# ymfm — Yamaha FM synthesis (BSD-3-Clause, https://github.com/aaronsgiles/ymfm)
# Used for the YM2612. No CMakeLists upstream, so we list the sources.
# ---------------------------------------------------------------------------
FetchContent_Declare(ymfm
    GIT_REPOSITORY https://github.com/aaronsgiles/ymfm.git
    GIT_TAG        81aec25ccbb98f4873a255f7551ac4dadac59b4a  # master, 2026-07-27
    UPDATE_DISCONNECTED TRUE
)
FetchContent_MakeAvailable(ymfm)  # populate only (no CMakeLists.txt upstream)

add_library(ymfm STATIC
    ${ymfm_SOURCE_DIR}/src/ymfm_opn.cpp    # YM2612 lives here
    ${ymfm_SOURCE_DIR}/src/ymfm_ssg.cpp    # needed by other OPN chips in the same TU
    ${ymfm_SOURCE_DIR}/src/ymfm_adpcm.cpp  # idem
)
target_include_directories(ymfm SYSTEM PUBLIC ${ymfm_SOURCE_DIR}/src)
target_compile_features(ymfm PUBLIC cxx_std_14)

# ---------------------------------------------------------------------------
# Z80 — Zilog Z80 core in C (MIT, https://github.com/superzazu/z80)
# ---------------------------------------------------------------------------
FetchContent_Declare(z80
    GIT_REPOSITORY https://github.com/superzazu/z80.git
    GIT_TAG        d64fe10a2274e5e40019b1086bf7d8990cbc5f23  # master, 2021-11-10
    UPDATE_DISCONNECTED TRUE
)
FetchContent_MakeAvailable(z80)   # populate only

add_library(z80_core STATIC ${z80_SOURCE_DIR}/z80.c)
target_include_directories(z80_core SYSTEM PUBLIC ${z80_SOURCE_DIR})
set_target_properties(z80_core PROPERTIES C_STANDARD 99)

# ---------------------------------------------------------------------------
# md_core
# ---------------------------------------------------------------------------
add_library(md_core STATIC
    src/emulator.cpp
    src/memory.cpp
    src/cpu/m68k.cpp
    src/cpu/sh2.cpp
    src/video/vdp.cpp
    src/cpu/z80.cpp
    src/audio/psg.cpp
    src/audio/ym2612.cpp
    src/audio/sound.cpp
    src/io/gamepad.cpp
    src/rom_loader.cpp
    src/zip_archive.cpp
    src/megacd/registers.cpp
    src/megacd/megacd.cpp
    src/megacd/cdc.cpp
    src/megacd/cd_drive.cpp
    src/megacd/pcm.cpp
    src/megacd/disc.cpp
    src/megacd/graphics_asic.cpp
    src/sega32x/system_registers.cpp
    src/sega32x/sega32x.cpp
    src/sega32x/vdp32x.cpp
    src/sega32x/pwm.cpp
)
add_library(md::core ALIAS md_core)

target_include_directories(md_core
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_compile_features(md_core PUBLIC cxx_std_20)
# Third-party cores are PRIVATE: none of their headers appear in md_core's API.
target_link_libraries(md_core PRIVATE md::warnings moira ymfm z80_core)

# zlib (system library) for deflate-compressed ZIP ROMs. Optional: without it,
# only uncompressed ("stored") ZIP entries can be loaded.
find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(md_core PRIVATE ZLIB::ZLIB)
    target_compile_definitions(md_core PRIVATE MD_HAVE_ZLIB=1)
else()
    message(WARNING "zlib not found: compressed ZIP ROMs will not be supported")
    target_compile_definitions(md_core PRIVATE MD_HAVE_ZLIB=0)
endif()
