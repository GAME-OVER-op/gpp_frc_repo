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
    #
    # NOTE: we use the BepInEx/Dobby fork instead of upstream jmpews/Dobby.
    # Upstream master is frequently broken (mid-refactor: missing core/arch/Cpu.h,
    # undeclared OSMemory, RuntimeModule.load_address, etc.). The BepInEx fork is
    # curated for stable cross-platform builds (Android/Linux/Windows/macOS) and
    # builds cleanly with modern NDK. Pinned to a branch that does not move under us.
    set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
    set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
    set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
    set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(dobby
        GIT_REPOSITORY https://github.com/BepInEx/Dobby.git
        GIT_TAG master
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(dobby)
    set(HOOK_LIB dobby)
    set(HOOK_INCLUDE ${dobby_SOURCE_DIR}/include)
endif()

add_library(lybfghook SHARED
    main.cpp
    hook_engine.cpp
    hook_egl.cpp
    hook_vulkan.cpp
    interop_bench.cpp
    extrap_bench.cpp
    gpp_engine.cpp
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

# GLES linkage: frame_gen.cpp uses GLES 3.0 VAO entry points
# (glGenVertexArrays/glBindVertexArray/glDeleteVertexArrays). Those are exported
# by the NDK's libGLESv3.so stub, NOT by libGLESv2.so (which only provides GLES 2.0
# symbols). We link both: GLESv3 for 3.x entry points, GLESv2 for the 2.0 core.
target_link_libraries(lybfghook
    ${HOOK_LIB}
    EGL
    GLESv3
    GLESv2
    vulkan
    log
    android
)
