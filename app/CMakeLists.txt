cmake_minimum_required(VERSION 3.18)
project(gpp_frc_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(gpp_frc_test src/main.cpp)

# Phase 2: session/gui тянем через dlopen/dlsym на устройстве.
# Линкуем только NDK-API, которые зовём напрямую:
#   log        - __android_log_print
#   dl         - dlopen/dlsym
#   mediandk   - AImageReader_* / AImage_*
#   android    - ANativeWindow_* (NDK surface API)
#   nativewindow - ANativeWindow_* (на новых NDK вынесено сюда)
target_link_libraries(gpp_frc_test log dl mediandk android nativewindow)

set_target_properties(gpp_frc_test PROPERTIES POSITION_INDEPENDENT_CODE ON)
