cmake_minimum_required(VERSION 3.22)
project(BEMatica LANGUAGES CXX ASM)

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

set(BLOCK_MAP "${CMAKE_CURRENT_SOURCE_DIR}/data/java_to_bedrock.tsv.gz")
configure_file(src/block_map_data.S.in "${CMAKE_BINARY_DIR}/block_map_data.S" @ONLY)
set_property(SOURCE "${CMAKE_BINARY_DIR}/block_map_data.S" APPEND PROPERTY OBJECT_DEPENDS "${BLOCK_MAP}")

add_library(BEMatica SHARED src/main.cpp src/nbt.cpp src/mcstructure.cpp src/library.cpp src/materials_panel.cpp
            src/mesh.cpp src/hologram.cpp src/easyplace.cpp src/gzip.cpp src/blockmap.cpp src/litematic.cpp
            "${CMAKE_BINARY_DIR}/block_map_data.S")
target_include_directories(BEMatica PRIVATE src)
target_link_libraries(BEMatica PRIVATE preloader log z)
target_compile_options(BEMatica PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-O2 -fvisibility=hidden -fvisibility-inlines-hidden -Wall>)
# 16 KB pages are required on newer Android devices.
target_link_options(BEMatica PRIVATE -Wl,--gc-sections -Wl,-z,max-page-size=16384 -s)
