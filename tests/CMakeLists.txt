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
    megacd_tests.cpp
    megacd_scheduler_tests.cpp
    megacd_cd_tests.cpp
    megacd_graphics_tests.cpp
    megacd_sound_tests.cpp
    sega32x_tests.cpp
    sega32x_video_tests.cpp
    sh2_tests.cpp
)
target_link_libraries(core_tests PRIVATE md::core md::warnings)

# The ZIP tests build deflate-compressed archives with zlib. (find_package
# results are directory-scoped, so look it up here too.)
find_package(ZLIB)
if(ZLIB_FOUND)
    target_link_libraries(core_tests PRIVATE ZLIB::ZLIB)
    target_compile_definitions(core_tests PRIVATE MD_HAVE_ZLIB=1)
endif()

add_test(NAME core_tests COMMAND core_tests)

# Frontend input tests (need SDL3, so only when the frontend is built).
# Uses SDL's virtual gamepads: no physical device needed.
if(MD_BUILD_FRONTEND)
    add_executable(frontend_tests
        test_main.cpp
        frontend_input_tests.cpp
    )
    target_link_libraries(frontend_tests PRIVATE md_frontend_input md::warnings)
    add_test(NAME frontend_tests COMMAND frontend_tests)
endif()
