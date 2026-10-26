cmake_minimum_required(VERSION 3.22)
project(BEMatica LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT ANDROID)
  message(FATAL_ERROR "Build with the Android NDK toolchain (arm64-v8a). See CONTRIBUTING.md.")
endif()

set(PL_SDK "${CMAKE_CURRENT_SOURCE_DIR}/vendor/preloader-android")
if(NOT EXISTS "${PL_SDK}/CMakeLists.txt")
  message(FATAL_ERROR "Preloader SDK missing: run `git submodule update --init`.")
endif()
add_subdirectory("${PL_SDK}" "${CMAKE_BINARY_DIR}/_preloader")

add_library(BEMatica SHARED src/main.cpp)
target_include_directories(BEMatica PRIVATE src)
target_link_libraries(BEMatica PRIVATE preloader log)
target_compile_options(BEMatica PRIVATE -O2 -fvisibility=hidden -fvisibility-inlines-hidden -Wall)
# 16 KB pages are required on newer Android devices.
target_link_options(BEMatica PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384 -s)
