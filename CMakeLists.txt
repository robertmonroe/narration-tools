cmake_minimum_required(VERSION 3.22)
project(NarrationTools VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the plugins and apps run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

include(FetchContent)
FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(JUCE)

add_subdirectory(BreathTamer)
add_subdirectory(GlitchTamer)
add_subdirectory(MouthTamer)
add_subdirectory(PopTamer)
add_subdirectory(RoomTamer)
