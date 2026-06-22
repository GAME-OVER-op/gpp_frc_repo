cmake_minimum_required(VERSION 3.18)
project(gpp_frc_test CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(gpp_frc_test src/main.cpp)

# Phase 1 использует только NDK-либы (всё остальное через dlopen/dlsym на устройстве)
target_link_libraries(gpp_frc_test log dl)

# PIE-исполняемый (NDK делает это по умолчанию, но фиксируем явно)
set_target_properties(gpp_frc_test PROPERTIES POSITION_INDEPENDENT_CODE ON)
