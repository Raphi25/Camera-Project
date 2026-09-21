cmake_minimum_required(VERSION 3.16)

# Keep builds reproducible when the exported project is used outside Git.
set(PROJECT_VER "1.0.0")

# Guard against host toolchain contamination on Windows where CMake can inject
# default desktop libs (kernel32.lib, user32.lib, etc.) into cross-link steps.
set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(breadboard_rtc_sd)
