cmake_minimum_required(VERSION 3.18)
project(gpp_frc_repo LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

# Inline hook backend. Dobby is linked statically so the module ships as a
# self-contained Zygisk library.
set(DOBBY_GENERATE_SHARED OFF CACHE BOOL "" FORCE)
set(DOBBY_DEBUG OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_EXAMPLE OFF CACHE BOOL "" FORCE)
set(DOBBY_BUILD_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(dobby
    GIT_REPOSITORY https://github.com/BepInEx/Dobby.git
    GIT_TAG master
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(dobby)

add_library(gpp_frc_repo SHARED
    main.cpp
    hook_engine.cpp
    hook_egl.cpp
    hook_vulkan.cpp
    frame_gen.cpp
    backend_select.cpp
    config.cpp
    display_rate.cpp
)

target_compile_options(gpp_frc_repo PRIVATE -O2 -fvisibility=hidden)
target_link_options(gpp_frc_repo PRIVATE -Wl,--gc-sections)
target_include_directories(gpp_frc_repo PRIVATE ${dobby_SOURCE_DIR}/include)
set_target_properties(gpp_frc_repo PROPERTIES OUTPUT_NAME "gpp_frc_repo")

target_link_libraries(gpp_frc_repo
    dobby
    EGL
    GLESv3
    GLESv2
    vulkan
    log
    android
)
