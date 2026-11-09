# megadrive — SDL3 frontend executable. The only place SDL is allowed.

include(FetchContent)

# FIND_PACKAGE_ARGS makes FetchContent try find_package(SDL3 CONFIG) first and
# only download/build SDL from source if no installed package is found.
# Set -DMD_FETCH_SDL=ON to always build from source (e.g. for reproducible CI).
if(MD_FETCH_SDL)
    set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
endif()

set(SDL_SHARED       OFF CACHE BOOL "" FORCE)  # only used when building from source
set(SDL_STATIC       ON  CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES     OFF CACHE BOOL "" FORCE)

FetchContent_Declare(SDL3
    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
    GIT_TAG        release-3.2.10
    GIT_SHALLOW    TRUE
    FIND_PACKAGE_ARGS 3.2 CONFIG
)
FetchContent_MakeAvailable(SDL3)

add_executable(megadrive
    src/main.cpp
)

target_link_libraries(megadrive
    PRIVATE
        md::core
        md::warnings
        SDL3::SDL3   # provided both by the installed config package and the source build
)
