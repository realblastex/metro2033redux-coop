cmake_minimum_required(VERSION 3.20)
project(metro2033redux_coop LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)

# MinHook (BSD-2-Clause, see third_party/minhook/LICENSE.txt)
add_library(minhook STATIC
    third_party/minhook/buffer.c third_party/minhook/hook.c
    third_party/minhook/trampoline.c third_party/minhook/hde/hde64.c)
target_include_directories(minhook PUBLIC third_party/minhook)

# The DLL injected into metro.exe
add_library(metro_coop SHARED src/dllmain.cpp src/engine.cpp src/logger.cpp)
target_link_libraries(metro_coop PRIVATE minhook shell32 user32)

# Launches metro.exe with the single-instance check patched in memory, and injects the DLL
add_executable(injector injector/injector.cpp)
