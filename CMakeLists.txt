cmake_minimum_required(VERSION 3.16)
project(RoastSim2026 VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(RS_BUILD_SERVICE "Build the host/service layer (networking, serial, C API)" ON)
option(RS_BUILD_APPS "Build roastsimd" ON)
option(RS_BUILD_NATIVE_UI "Build the native UI shell for this OS (Win32 today)" ON)
option(RS_BUILD_TESTS "Build the test-suite" ON)
option(RS_STATIC_RUNTIME "Link the C/C++ runtime statically (MinGW)" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()
if(MSVC)
  add_compile_options(/W3 /utf-8)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()
if(MINGW AND RS_STATIC_RUNTIME)
  # Self-contained .exe files: no libstdc++/libgcc/winpthread DLLs needed.
  add_link_options(-static -static-libgcc -static-libstdc++)
endif()

# ------------------------------------------------------------------ core
# Pure C++17. No OS headers, no threads, no sockets. Portable as-is.
add_library(roastsim_core STATIC
  core/src/json.cpp
  core/src/types.cpp
  core/src/bean.cpp
  core/src/roaster_def.cpp
  core/src/machine_fluidbed.cpp
  core/src/machine_drum.cpp
  core/src/machine_stirpan.cpp
  core/src/sensors.cpp
  core/src/simulation.cpp
  core/src/catalog.cpp
  core/src/scenario.cpp
)
target_include_directories(roastsim_core PUBLIC core/include)
# GCC < 9 needs stdc++fs for std::filesystem (older Haiku/Linux toolchains).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 9.0)
  target_link_libraries(roastsim_core PUBLIC stdc++fs)
endif()

# Copies data/ and web/ next to the executables on every build (they are
# runtime resources, so edits to them must reach the build tree too).
add_custom_target(rs_runtime_files ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/data ${CMAKE_BINARY_DIR}/data
  COMMAND ${CMAKE_COMMAND} -E copy_directory ${CMAKE_SOURCE_DIR}/web ${CMAKE_BINARY_DIR}/web
  VERBATIM)
function(rs_copy_runtime_files target)
  add_dependencies(${target} rs_runtime_files)
endfunction()

# Headless batch runner (core only).
add_executable(roastsim_script tools/roastsim_script.cpp)
target_link_libraries(roastsim_script PRIVATE roastsim_core)
rs_copy_runtime_files(roastsim_script)

# Preset validator + reference roasts (used by the add-bean / add-roaster agent skills).
add_executable(roastsim_check tools/roastsim_check.cpp)
target_link_libraries(roastsim_check PRIVATE roastsim_core)

# ------------------------------------------------------------------ service
if(RS_BUILD_SERVICE)
  find_package(Threads REQUIRED)
  add_library(roastsim_service STATIC
    service/src/platform_net.cpp
    service/src/platform_serial.cpp
    service/src/platform_paths.cpp
    service/src/sha1.cpp
    service/src/host.cpp
    service/src/api.cpp
    service/src/presets.cpp
    service/src/modbus_server.cpp
    service/src/web_server.cpp
    service/src/mcp_server.cpp
    service/src/tc4_serial.cpp
    service/src/capi.cpp
    service/src/audio_synth.cpp
    service/src/platform_audio.cpp
  )
  target_include_directories(roastsim_service PUBLIC service/include)
  target_compile_definitions(roastsim_service PRIVATE RS_VERSION="${PROJECT_VERSION}")
  target_link_libraries(roastsim_service PUBLIC roastsim_core Threads::Threads)
  if(WIN32)
    target_link_libraries(roastsim_service PUBLIC ws2_32 winmm)
    target_compile_definitions(roastsim_service PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0601)
  elseif(HAIKU)
    target_link_libraries(roastsim_service PUBLIC network be)
  elseif(CMAKE_SYSTEM_NAME MATCHES "SunOS")
    target_link_libraries(roastsim_service PUBLIC socket nsl)
  endif()
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(roastsim_service PUBLIC util)  # openpty fallback
  endif()
endif()

# ------------------------------------------------------------------ apps
if(RS_BUILD_SERVICE AND RS_BUILD_APPS)
  # Offline sound renderer (WAV) for auditioning and tuning the sound model.
  add_executable(roastsim_sound tools/roastsim_sound.cpp)
  target_link_libraries(roastsim_sound PRIVATE roastsim_service)

  add_executable(roastsimd apps/roastsimd/main.cpp)
  target_link_libraries(roastsimd PRIVATE roastsim_service)
  rs_copy_runtime_files(roastsimd)

  if(WIN32 AND RS_BUILD_NATIVE_UI)
    add_executable(roastsim_win32 WIN32 apps/win32/main.cpp apps/win32/roastsim.rc)
    target_link_libraries(roastsim_win32 PRIVATE roastsim_service comctl32 winmm gdi32 shell32)
    if(MINGW)
      target_link_options(roastsim_win32 PRIVATE -municode)
    endif()
    target_compile_definitions(roastsim_win32 PRIVATE UNICODE _UNICODE)
    rs_copy_runtime_files(roastsim_win32)
  endif()
  # Future native shells (see apps/gtk/README.md and apps/haiku/README.md):
  #   Linux: GTK 4 shell linking the C API.   Haiku: BeAPI shell linking the C API.
endif()

# ------------------------------------------------------------------ tests
if(RS_BUILD_TESTS)
  enable_testing()
  add_executable(roastsim_tests tests/test_main.cpp)
  target_link_libraries(roastsim_tests PRIVATE roastsim_core)
  if(RS_BUILD_SERVICE)
    target_sources(roastsim_tests PRIVATE tests/test_service.cpp)
    target_link_libraries(roastsim_tests PRIVATE roastsim_service)
    target_compile_definitions(roastsim_tests PRIVATE RS_TEST_SERVICE=1)
  endif()
  add_test(NAME roastsim_tests COMMAND roastsim_tests ${CMAKE_SOURCE_DIR}/data)
endif()
