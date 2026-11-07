cmake_minimum_required(VERSION 3.24)
project(SheikahVision LANGUAGES C CXX OBJC OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES arm64)
set(CMAKE_OSX_DEPLOYMENT_TARGET 13.3)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(BOTW_GENERATED_DIR "${CMAKE_SOURCE_DIR}/generated" CACHE PATH "Player-local generated sources")

# Recompiled guest code (tools/recompile.py writes generated/).
file(GLOB GUEST_SOURCES CONFIGURE_DEPENDS
  ${BOTW_GENERATED_DIR}/fn_*.cpp
  ${BOTW_GENERATED_DIR}/big_*.cpp
  ${BOTW_GENERATED_DIR}/huge_*.cpp
  ${BOTW_GENERATED_DIR}/table.cpp
  ${BOTW_GENERATED_DIR}/hle_stubs.cpp)
add_library(guest OBJECT ${GUEST_SOURCES})
target_include_directories(guest PRIVATE runtime/include ${BOTW_GENERATED_DIR})
target_compile_options(guest PRIVATE -O2 -gline-tables-only -w -fwrapv -fno-strict-aliasing -mcpu=apple-m1
  -ffp-contract=off)
file(GLOB BIG_SOURCES CONFIGURE_DEPENDS ${BOTW_GENERATED_DIR}/big_*.cpp)
file(GLOB HUGE_SOURCES CONFIGURE_DEPENDS ${BOTW_GENERATED_DIR}/huge_*.cpp)
set_source_files_properties(${BIG_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1")
set_source_files_properties(${HUGE_SOURCES} PROPERTIES COMPILE_OPTIONS "-O0")

# Vendored Cemu Latte GPU layer (runtime/cemu, MPL-2.0) + shims, built in Cemu's own environment.
file(GLOB_RECURSE CEMU_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu/Cafe/*.mm
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Common/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu/util/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/Cemu/FileCache/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/imgui/*.mm
  ${CMAKE_SOURCE_DIR}/runtime/cemu_shims/*.cpp ${CMAKE_SOURCE_DIR}/runtime/cemu_shims/*.mm)
list(FILTER CEMU_SOURCES EXCLUDE REGEX "LatteOverlay\\.cpp$|/dependencies/|SystemInfo(Win|Linux|BSD)\\.cpp$")
list(APPEND CEMU_SOURCES
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_draw.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_tables.cpp
  ${CMAKE_SOURCE_DIR}/runtime/cemu/dependencies/imgui/imgui_widgets.cpp)
add_library(cemu_gpu OBJECT ${CEMU_SOURCES})
target_include_directories(cemu_gpu PRIVATE runtime/cemu_shims runtime/cemu runtime/cemu/Cafe
  runtime/cemu/dependencies/metal-cpp runtime/cemu/dependencies/imgui /opt/homebrew/include)
target_compile_definitions(cemu_gpu PRIVATE BOOST_NOWIDE_NO_LIB ENABLE_METAL BOTWRECOMP IMGUI_IMPL_METAL_CPP
  EMULATOR_VERSION_MAJOR=2 EMULATOR_VERSION_MINOR=6 EMULATOR_VERSION_PATCH=0 EMULATOR_HASH=a53cf0e)
target_compile_options(cemu_gpu PRIVATE -w -O2 -g1 "SHELL:-include Common/precompiled.h")

file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS
  ${CMAKE_SOURCE_DIR}/runtime/src/*.cpp
  ${CMAKE_SOURCE_DIR}/runtime/src/*.mm)
add_library(runtime_host OBJECT ${RUNTIME_SOURCES})
add_library(sheikah_runtime STATIC EXCLUDE_FROM_ALL $<TARGET_OBJECTS:runtime_host> $<TARGET_OBJECTS:cemu_gpu>)
add_executable(SheikahVision $<TARGET_OBJECTS:runtime_host> $<TARGET_OBJECTS:guest> $<TARGET_OBJECTS:cemu_gpu>)
target_include_directories(runtime_host PRIVATE runtime/include runtime/src ${BOTW_GENERATED_DIR})
target_compile_options(runtime_host PRIVATE -Wall -Wno-unused-function -Wno-unused-variable -fwrapv -mcpu=apple-m1
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_include_directories(SheikahVision PRIVATE runtime/include runtime/src ${BOTW_GENERATED_DIR})
target_compile_options(SheikahVision PRIVATE -Wall -Wno-unused-function -Wno-unused-variable -fwrapv -mcpu=apple-m1
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
find_package(SDL3 REQUIRED CONFIG)
find_package(fmt REQUIRED)
target_link_libraries(runtime_host PRIVATE SDL3::SDL3 fmt::fmt)
target_link_libraries(SheikahVision PRIVATE z SDL3::SDL3 fmt::fmt
  /opt/homebrew/opt/openssl@3/lib/libcrypto.dylib /opt/homebrew/opt/zstd/lib/libzstd.dylib
  "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework AppKit" "-framework MetalFX"
  "-framework CoreGraphics" "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework VideoToolbox" "-framework CoreMedia" "-framework CoreVideo")

# Native Metal GPU path (docs/native-renderer.md): option BOTW_NATIVE_GPU, default OFF (gpu_native.cmake).
include(${CMAKE_SOURCE_DIR}/runtime/src/gpu_native/gpu_native.cmake OPTIONAL)

# Optional native-renderer definitions also apply to the extracted host objects.
target_compile_definitions(runtime_host PRIVATE $<TARGET_PROPERTY:SheikahVision,COMPILE_DEFINITIONS>)
