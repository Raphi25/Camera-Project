cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.0")
# Vendor the camera stack from the working prototypes; no registry needed.
set(ENV{IDF_COMPONENT_MANAGER} "0")
set(CMAKE_C_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)
set(CMAKE_CXX_STANDARD_LIBRARIES "" CACHE STRING "" FORCE)
set(COMPONENTS main cmake_utilities esp_ipa)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(blur_test)
