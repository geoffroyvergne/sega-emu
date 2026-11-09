# Core tests link only md::core — if anything in core starts needing SDL,
# this target fails to build, which keeps the core platform-independent.

add_executable(core_tests
    test_main.cpp
    emulator_tests.cpp
    memory_tests.cpp
    cpu_tests.cpp
    vdp_tests.cpp
    vdp_dma_tests.cpp
    vdp_render_tests.cpp
    sound_tests.cpp
    input_tests.cpp
    rom_loader_tests.cpp
)
target_link_libraries(core_tests PRIVATE md::core md::warnings)

# The ZIP tests build deflate-compressed archives with zlib.
if(ZLIB_FOUND)
    target_link_libraries(core_tests PRIVATE ZLIB::ZLIB)
    target_compile_definitions(core_tests PRIVATE MD_HAVE_ZLIB=1)
endif()

add_test(NAME core_tests COMMAND core_tests)
