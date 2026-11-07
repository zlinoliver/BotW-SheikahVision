# SheikahVision for iOS (iPhone, arm64) and visionOS (Apple Vision Pro: -DCMAKE_SYSTEM_NAME=visionOS, sysroot xros). Separate project so the macOS build (../CMakeLists.txt) stays
# untouched; it compiles the same generated/, runtime/ and runtime/cemu sources plus ios/src.
#
# Use ios/scripts/build.sh (configures build-ios/<sdk>, builds, signs). By hand:
#   cmake -S ios -B build-ios/iphoneos -G Ninja -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphoneos \
#         -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=18.0
#   cmake --build build-ios/iphoneos -j6
cmake_minimum_required(VERSION 3.24)
project(SheikahVisionIOS LANGUAGES C CXX OBJC OBJCXX)

if(NOT IOS AND NOT CMAKE_SYSTEM_NAME STREQUAL "visionOS")
  message(FATAL_ERROR "configure with -DCMAKE_SYSTEM_NAME=iOS or visionOS")
endif()

get_filename_component(ROOT ${CMAKE_CURRENT_SOURCE_DIR}/.. ABSOLUTE)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

set(BOTW_BUNDLE_NAME SheikahVision)
set(BOTW_DISPLAY_NAME BotW)
if(CMAKE_SYSTEM_NAME STREQUAL "visionOS")
  # the immersive VR space (ios/src/BotWImmersive.swift) is SwiftUI / CompositorServices; swiftc needs the target
  # spelled out (CMake would hand it the host's macOS one)
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(CMAKE_Swift_COMPILER_TARGET "arm64-apple-xros${CMAKE_OSX_DEPLOYMENT_TARGET}-simulator")
  else()
    set(CMAKE_Swift_COMPILER_TARGET "arm64-apple-xros${CMAKE_OSX_DEPLOYMENT_TARGET}")
  endif()
  enable_language(Swift)
  set(BOTW_VISION ON)
  # the Vision Pro app's name (2026-11-02: the project's brand, SheikahVision); the build keeps SheikahVision.app inside
  set(BOTW_BUNDLE_NAME SheikahVision)
  set(BOTW_DISPLAY_NAME SheikahVision)
  set(BOTW_MULTIPLE_SCENES "<true/>")
  set(BOTW_DEVICE_FAMILY "<integer>7</integer>")
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(BOTW_SDK xrsimulator)
    set(BOTW_PLATFORM XRSimulator)
  else()
    set(BOTW_SDK xros)
    set(BOTW_PLATFORM XROS)
  endif()
else()
  set(BOTW_MULTIPLE_SCENES "<false/>")
  set(BOTW_DEVICE_FAMILY "<integer>1</integer><integer>2</integer>")
  if(CMAKE_OSX_SYSROOT MATCHES "[Ss]imulator")
    set(BOTW_SDK iphonesimulator)
    set(BOTW_PLATFORM iPhoneSimulator)
  else()
    set(BOTW_SDK iphoneos)
    set(BOTW_PLATFORM iPhoneOS)
  endif()
endif()
set(BOTW_BUNDLE_ID "com.example.SheikahVision" CACHE STRING "CFBundleIdentifier (build.sh passes BOTW_BUNDLE_ID)")
set(BOTW_IOS_DEPS "${ROOT}/ios-deps" CACHE PATH "ios/scripts/build_deps.sh output")
set(BOTW_PREBUILT_RUNTIME "" CACHE PATH "Verified installer runtime kit")
set(DEPS ${BOTW_IOS_DEPS}/prefix-${BOTW_SDK})
if(BOTW_PREBUILT_RUNTIME)
  set(DEPS ${BOTW_PREBUILT_RUNTIME}/prefix-${BOTW_SDK})
endif()

# deps live outside the SDK sysroot, which the iOS platform module restricts find_package to
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE BOTH)
set(SDL3_DIR "${DEPS}/lib/cmake/SDL3" CACHE PATH "" FORCE)
set(fmt_DIR "${DEPS}/lib/cmake/fmt" CACHE PATH "" FORCE)
set(zstd_DIR "${DEPS}/lib/cmake/zstd" CACHE PATH "" FORCE)
find_package(SDL3 REQUIRED CONFIG PATHS ${DEPS}/lib/cmake/SDL3 NO_DEFAULT_PATH)
find_package(fmt REQUIRED CONFIG PATHS ${DEPS}/lib/cmake/fmt NO_DEFAULT_PATH)
find_package(zstd REQUIRED CONFIG PATHS ${DEPS}/lib/cmake/zstd NO_DEFAULT_PATH)

# A14 == M1 ISA (armv8.5-a), so the guest code is compiled for the same features as on macOS.
set(BOTW_CPU -mcpu=apple-a14)

set(BOTW_GENERATED_DIR "${ROOT}/generated" CACHE PATH "Player-local generated sources")
option(BOTW_RUNTIME_ONLY "Build public runtime without any generated game code" OFF)
set(BOTW_PREBUILT_RUNTIME "" CACHE PATH "Verified installer runtime kit")
set(BOTW_GUEST_OPT "" CACHE STRING "Benchmark override only: O1 or O2")

if(NOT BOTW_RUNTIME_ONLY)
# ---- recompiled guest code (same flags as the macOS build) ----------------------------------
file(GLOB GUEST_SOURCES CONFIGURE_DEPENDS
  ${BOTW_GENERATED_DIR}/fn_*.cpp ${BOTW_GENERATED_DIR}/big_*.cpp ${BOTW_GENERATED_DIR}/huge_*.cpp
  ${BOTW_GENERATED_DIR}/table.cpp ${BOTW_GENERATED_DIR}/hle_stubs.cpp)
add_library(guest OBJECT ${GUEST_SOURCES})
target_include_directories(guest PRIVATE ${ROOT}/runtime/include ${BOTW_GENERATED_DIR})
target_compile_options(guest PRIVATE -O2 -gline-tables-only -w -fwrapv -fno-strict-aliasing ${BOTW_CPU}
  -ffp-contract=off)
file(GLOB BIG_SOURCES CONFIGURE_DEPENDS ${BOTW_GENERATED_DIR}/big_*.cpp)
file(GLOB HUGE_SOURCES CONFIGURE_DEPENDS ${BOTW_GENERATED_DIR}/huge_*.cpp)
set_source_files_properties(${BIG_SOURCES} PROPERTIES COMPILE_OPTIONS "-O1")
set_source_files_properties(${HUGE_SOURCES} PROPERTIES COMPILE_OPTIONS "-O0")

if(BOTW_GUEST_OPT)
  if(NOT BOTW_GUEST_OPT MATCHES "^O[12]$")
    message(FATAL_ERROR "BOTW_GUEST_OPT must be O1 or O2")
  endif()
  target_compile_options(guest PRIVATE -${BOTW_GUEST_OPT})
endif()
endif()

if(NOT BOTW_PREBUILT_RUNTIME)
# ---- vendored Cemu Latte GPU + shims ---------------------------------------------------------
file(GLOB_RECURSE CEMU_SOURCES CONFIGURE_DEPENDS
  ${ROOT}/runtime/cemu/Cafe/*.cpp ${ROOT}/runtime/cemu/Cafe/*.mm
  ${ROOT}/runtime/cemu/Common/*.cpp ${ROOT}/runtime/cemu/util/*.cpp
  ${ROOT}/runtime/cemu/Cemu/FileCache/*.cpp
  ${ROOT}/runtime/cemu/imgui/*.mm
  ${ROOT}/runtime/cemu_shims/*.cpp ${ROOT}/runtime/cemu_shims/*.mm)
list(FILTER CEMU_SOURCES EXCLUDE REGEX "LatteOverlay\\.cpp$|/dependencies/|SystemInfo(Win|Linux|BSD)\\.cpp$")
# AppKit view/layer creation: replaced by ios/src/MetalLayer_ios.mm
list(FILTER CEMU_SOURCES EXCLUDE REGEX "Renderer/MetalView\\.mm$|Renderer/Metal/MetalLayer\\.mm$")
list(APPEND CEMU_SOURCES
  ${ROOT}/runtime/cemu/dependencies/imgui/imgui.cpp
  ${ROOT}/runtime/cemu/dependencies/imgui/imgui_draw.cpp
  ${ROOT}/runtime/cemu/dependencies/imgui/imgui_tables.cpp
  ${ROOT}/runtime/cemu/dependencies/imgui/imgui_widgets.cpp)
add_library(cemu_gpu OBJECT ${CEMU_SOURCES})
# ios/compat first: CommonCrypto-backed <openssl/sha.h>/<openssl/evp.h>, empty <png.h>.
# boost/glm are header-only; the installer passes the detected Homebrew include paths.
set(BOTW_BOOST_INCLUDE "/opt/homebrew/opt/boost/include" CACHE PATH "Boost headers")
set(BOTW_GLM_INCLUDE "/opt/homebrew/opt/glm/include" CACHE PATH "GLM headers")
target_include_directories(cemu_gpu PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/compat
  ${ROOT}/runtime/cemu_shims ${ROOT}/runtime/cemu ${ROOT}/runtime/cemu/Cafe
  ${ROOT}/runtime/cemu/dependencies/metal-cpp ${ROOT}/runtime/cemu/dependencies/imgui
  ${BOTW_BOOST_INCLUDE} ${BOTW_GLM_INCLUDE})
target_link_libraries(cemu_gpu PRIVATE fmt::fmt zstd::libzstd_static)
target_compile_definitions(cemu_gpu PRIVATE BOOST_NOWIDE_NO_LIB ENABLE_METAL BOTWRECOMP IMGUI_IMPL_METAL_CPP
  EMULATOR_VERSION_MAJOR=2 EMULATOR_VERSION_MINOR=6 EMULATOR_VERSION_PATCH=0 EMULATOR_HASH=a53cf0e)
target_compile_options(cemu_gpu PRIVATE "-ffile-prefix-map=${ROOT}=." -w -O2 -g1
  "SHELL:-include \"${CMAKE_CURRENT_SOURCE_DIR}/compat/botw_ios_platform.h\"" "SHELL:-include Common/precompiled.h")

# ---- runtime + iOS host layer -----------------------------------------------------------------
file(GLOB_RECURSE RUNTIME_SOURCES CONFIGURE_DEPENDS ${ROOT}/runtime/src/*.cpp ${ROOT}/runtime/src/*.mm)
file(GLOB IOS_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.mm ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
# main.cpp's main() becomes botw_main(); ios/src/main_ios.mm owns main() (UIApplicationMain via SDL)
set_source_files_properties(${ROOT}/runtime/src/main.cpp PROPERTIES COMPILE_DEFINITIONS "main=botw_main")
# CEMU_SOURCES with the AppKit view excluded need a CreateMetalLayer: ios/src/MetalLayer_ios.mm
add_library(runtime_host OBJECT ${RUNTIME_SOURCES} ${IOS_SOURCES})
target_include_directories(runtime_host PRIVATE ${ROOT}/runtime/include ${ROOT}/runtime/src ${BOTW_GENERATED_DIR}
)
target_compile_options(runtime_host PRIVATE "-ffile-prefix-map=${ROOT}=."
  "$<$<NOT:$<COMPILE_LANGUAGE:Swift>>:-Wall;-Wno-unused-function;-Wno-unused-variable;-fwrapv;${BOTW_CPU}>"
  $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
target_link_libraries(runtime_host PRIVATE SDL3::SDL3-static fmt::fmt zstd::libzstd_static)
add_library(sheikah_runtime STATIC EXCLUDE_FROM_ALL $<TARGET_OBJECTS:runtime_host> $<TARGET_OBJECTS:cemu_gpu>)
endif()

# The ordinary build still links exactly the same objects; the installer can import the public archive.
add_executable(SheikahVision MACOSX_BUNDLE EXCLUDE_FROM_ALL ${ROOT}/installer/cmake/host_link.cpp)
if(BOTW_PREBUILT_RUNTIME)
  target_link_options(SheikahVision PRIVATE "LINKER:-force_load,${BOTW_PREBUILT_RUNTIME}/libsheikah_runtime.a")
  include(${ROOT}/installer/cmake/RuntimeStamp.cmake)
  botw_runtime_stamp(SheikahVision "${BOTW_PREBUILT_RUNTIME}")
elseif(NOT BOTW_RUNTIME_ONLY)
  target_sources(SheikahVision PRIVATE $<TARGET_OBJECTS:runtime_host> $<TARGET_OBJECTS:cemu_gpu>)
endif()
if(NOT BOTW_RUNTIME_ONLY)
  set_target_properties(SheikahVision PROPERTIES EXCLUDE_FROM_ALL FALSE)
  target_sources(SheikahVision PRIVATE $<TARGET_OBJECTS:guest>)
endif()
target_link_libraries(SheikahVision PRIVATE z SDL3::SDL3-static fmt::fmt zstd::libzstd_static
  "-framework Metal" "-framework QuartzCore" "-framework Foundation" "-framework UIKit"
  "-framework CoreGraphics" "-framework ImageIO" "-framework UniformTypeIdentifiers" "-framework VideoToolbox"
  "-framework CoreMedia" "-framework CoreVideo" "-framework GameController" "-framework CoreHaptics" "-framework AVFoundation"
  "-framework AudioToolbox")

if(BOTW_VISION)
  # Swift in a static library of its own: clang++ keeps linking the executable (a Swift-driven link drops the object
  # libraries' files), with the Swift runtime's search paths added; the Swift objects autolink their libraries
  if(NOT BOTW_PREBUILT_RUNTIME)
  add_library(botw_immersive STATIC ${CMAKE_CURRENT_SOURCE_DIR}/src/BotWImmersive.swift
              ${CMAKE_CURRENT_SOURCE_DIR}/src/BotWLauncher.swift ${CMAKE_CURRENT_SOURCE_DIR}/src/BotWL10n.swift
              ${CMAKE_CURRENT_SOURCE_DIR}/src/BotWGuideDiagram.swift
              ${CMAKE_CURRENT_SOURCE_DIR}/src/BotWZipImport.swift
              ${CMAKE_CURRENT_SOURCE_DIR}/src/BotWGameImport.swift)
  set_target_properties(botw_immersive PROPERTIES Swift_LANGUAGE_VERSION 5)
  target_compile_options(botw_immersive PRIVATE "SHELL:-debug-prefix-map \"${ROOT}=.\""
    "SHELL:-import-objc-header \"${CMAKE_CURRENT_SOURCE_DIR}/src/vr_bridge.h\"")
  else()
    add_library(botw_immersive STATIC IMPORTED)
    set_target_properties(botw_immersive PROPERTIES IMPORTED_LOCATION "${BOTW_PREBUILT_RUNTIME}/libbotw_immersive.a")
  endif()
  execute_process(COMMAND xcrun --sdk ${BOTW_SDK} --show-sdk-path OUTPUT_VARIABLE BOTW_SDK_PATH
    OUTPUT_STRIP_TRAILING_WHITESPACE)
  execute_process(COMMAND xcrun --find swiftc OUTPUT_VARIABLE BOTW_SWIFTC OUTPUT_STRIP_TRAILING_WHITESPACE)
  get_filename_component(BOTW_TOOLCHAIN_LIB "${BOTW_SWIFTC}/../../lib/swift/${BOTW_SDK}" ABSOLUTE)
  target_link_libraries(SheikahVision PRIVATE botw_immersive "-L${BOTW_SDK_PATH}/usr/lib/swift"
    "-L${BOTW_TOOLCHAIN_LIB}" "-framework SwiftUI" "-framework CompositorServices" "-framework ARKit")
  set_target_properties(SheikahVision PROPERTIES LINKER_LANGUAGE CXX)
endif()

if(BOTW_SDK STREQUAL "iphoneos" OR BOTW_SDK STREQUAL "xros")
  # MetalFX (upscaling) exists on devices only, not in the simulator SDK
  target_link_libraries(SheikahVision PRIVATE "-framework MetalFX")
endif()

set(BOTW_MIN_IOS ${CMAKE_OSX_DEPLOYMENT_TARGET})
# the app's version: the VERSION file at the repository root, the one version shown in the launcher's version line
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/../VERSION" BOTW_APP_VERSION LIMIT_COUNT 1)
if(NOT BOTW_APP_VERSION)
  set(BOTW_APP_VERSION 0.0.0)
endif()
# which build this is, shown in the launcher's version line (user 2026-11-12: with the same VERSION nobody could tell
# whether the headset had the new build): an always increasing build number (yyyymmddhhmm) for CFBundleVersion and the
# source commit (8 characters, "+" with local edits) as SVSourceCommit; ios/scripts/build.sh passes
# both
set(BOTW_BUILD_NUMBER 1 CACHE STRING "CFBundleVersion: an always increasing build number")
set(BOTW_SOURCE_COMMIT dev CACHE STRING "the source commit, shown in the launcher")
set_target_properties(SheikahVision PROPERTIES
  OUTPUT_NAME SheikahVision
  MACOSX_BUNDLE_INFO_PLIST ${CMAKE_CURRENT_SOURCE_DIR}/Info.plist.in
  MACOSX_BUNDLE_GUI_IDENTIFIER ${BOTW_BUNDLE_ID}
  MACOSX_BUNDLE_BUNDLE_NAME SheikahVision
  MACOSX_BUNDLE_SHORT_VERSION_STRING ${BOTW_APP_VERSION}
  MACOSX_BUNDLE_BUNDLE_VERSION ${BOTW_BUILD_NUMBER})

# Hash-only regional manifests; no game resources are bundled.
file(GLOB BOTW_GAME_MANIFESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/game-manifest/*.json")
target_sources(SheikahVision PRIVATE ${BOTW_GAME_MANIFESTS})
set_source_files_properties(${BOTW_GAME_MANIFESTS} PROPERTIES MACOSX_PACKAGE_LOCATION Resources/game-manifest)

# The optional experience save (the author's own completed save, one slot: open grassland), copied by the launcher into
# a save folder of its own when the player turns it on; never into the player's saves (user 2026-11-17)
file(GLOB_RECURSE BOTW_EXPERIENCE_SAVE CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/assets/experience-save/*.sav"
     "${CMAKE_CURRENT_SOURCE_DIR}/assets/experience-save/*.jpg")
foreach(_f IN LISTS BOTW_EXPERIENCE_SAVE)
  file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}/assets/experience-save" "${_f}")
  get_filename_component(_dir "${_rel}" DIRECTORY)
  target_sources(SheikahVision PRIVATE "${_f}")
  set_source_files_properties("${_f}" PROPERTIES MACOSX_PACKAGE_LOCATION "Resources/experience-save/${_dir}")
endforeach()

# Copied source can have older mtimes; changed contents still invalidate public host objects.
if(NOT BOTW_PREBUILT_RUNTIME)
  set(BOTW_SOURCE_VERSION "" CACHE STRING "Installer source content fingerprint")
  if(NOT BOTW_SOURCE_VERSION)
    set(_host_fingerprint "")
    foreach(_source IN LISTS RUNTIME_SOURCES IOS_SOURCES CEMU_SOURCES)
      file(SHA256 "${_source}" _hash)
      string(APPEND _host_fingerprint "${_hash}")
    endforeach()
    string(SHA256 _host_fingerprint "${_host_fingerprint}")
  else()
    set(_host_fingerprint "${BOTW_SOURCE_VERSION}")
  endif()
  file(CONFIGURE OUTPUT "${CMAKE_BINARY_DIR}/runtime-source.stamp" CONTENT "${_host_fingerprint}\n")
  set_source_files_properties(${RUNTIME_SOURCES} ${IOS_SOURCES} ${CEMU_SOURCES}
    PROPERTIES OBJECT_DEPENDS "${CMAKE_BINARY_DIR}/runtime-source.stamp")
endif()
