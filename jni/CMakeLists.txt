cmake_minimum_required(VERSION 3.18)
project(cleanfg LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# --- inline hooker --------------------------------------------------------
# Default: Dobby, built STATIC and linked into our module, so the final module
# is a single self-contained .so with no extra runtime dependencies.
# Alternative: ShadowHook via -DUSE_SHADOWHOOK=ON (built as shared lib).
option(USE_SHADOWHOOK "Use ShadowHook instead of Dobby" OFF)

if(USE_SHADOWHOOK)
    add_compile_definitions(USE_SHADOWHOOK)
    FetchContent_Declare(shadowhook
        GIT_REPOSITORY https://github.com/bytedance/android-inline-hook.git
        GIT_TAG v1.1.1
        SOURCE_SUBDIR shadowhook/src/main/cpp)
    FetchContent_MakeAvailable(shadowhook)
    set(HOOK_LIB shadowhook)
    set(HOOK_INCLUDE ${shadowhook_SOURCE_DIR}/shadowhook/src/main/cpp/include)
else()
    # Build Dobby as a static library so it links directly into our module.
    set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
    set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(dobby
        GIT_REPOSITORY https://github.com/jmpews/Dobby.git
        GIT_TAG master)
    FetchContent_MakeAvailable(dobby)
    set(HOOK_LIB dobby)
    set(HOOK_INCLUDE ${dobby_SOURCE_DIR}/include)
endif()

add_library(lybfghook SHARED
    main.cpp
    hook_engine.cpp
    hook_egl.cpp
    hook_vulkan.cpp
    frame_gen.cpp
    config.cpp
    display_rate.cpp
)

# Apply our compile flags only to our target (do NOT impose them on Dobby).
target_compile_options(lybfghook PRIVATE -O2 -fvisibility=hidden)
target_link_options(lybfghook PRIVATE -Wl,--gc-sections)

target_include_directories(lybfghook PRIVATE ${HOOK_INCLUDE})

# SONAME/output name like the original, so it is drop-in compatible.
set_target_properties(lybfghook PROPERTIES OUTPUT_NAME "lybfghook")

target_link_libraries(lybfghook
    ${HOOK_LIB}
    EGL
    GLESv3
    GLESv2
    log
    android
)
