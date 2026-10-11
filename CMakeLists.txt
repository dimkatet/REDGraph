# REDGraph - a render host for Cyberpunk 2077 (2.31): REDGraph.dll -> <game>/red4ext/plugins/REDGraph/, its CET window
# cet/REDGraph/ -> <game>/bin/x64/plugins/cyber_engine_tweaks/mods/REDGraph/. Clients are other RED4ext plugins that
# see only include/rg_host.h (target REDGraph::api).
#
#   cmake -B build -A x64 [-DRG_RED4EXT_SDK_DIR=<RED4ext.SDK checkout>] [-DRG_GAME_DIR=... -DRG_DXC_DIR=...]
#   cmake --build build --config Release
cmake_minimum_required(VERSION 3.20)
# The release version (the DLL's RED4ext version, the log, the SDK package). The API version is RG_HOST_API_VERSION in
# include/rg_host.h and changes only with the ABI.
project(REDGraph VERSION 0.3.0 LANGUAGES CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
  message(FATAL_ERROR "Cyberpunk 2077 is 64-bit only; configure with a 64-bit generator (-A x64)")
endif()

# Built as a sub-project (a client's tree: add_subdirectory(<REDGraph>)), the example is off by default and an
# existing RED4ext::SDK target is used.
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(rg_top ON)
else()
  set(rg_top OFF)
endif()
option(RG_BUILD_SELFTEST "Build REDGraphSelfTest.dll (the host's tests, a client plugin)" ON)
option(RG_BUILD_EXAMPLE "Build REDGraphExampleClient.dll (the SDK's smallest client)" ${rg_top})
option(RG_NATIVE_HOST "The host's engine hooks; OFF compiles them out (only for a bisect)" ON)
set(RG_RED4EXT_SDK_DIR "" CACHE PATH "RED4ext.SDK checkout (empty = fetch the pinned commit)")
# The self-test's game-derived blobs (docs/internals.md "Self-tests"): built from the installed game, never committed.
set(RG_GAME_DIR "$ENV{RG_GAME_DIR}" CACHE PATH "Cyberpunk 2077 install (holds bin/ and engine/), for the self-test blobs")
set(RG_DXC_DIR "$ENV{RG_DXC_DIR}" CACHE PATH "DirectX Shader Compiler release bin/x64 (dxc.exe, dxcompiler.dll, dxil.dll)")

# ---- RED4ext.SDK (header-only) ----
set(RED4EXT_HEADER_ONLY ON CACHE BOOL "" FORCE)
if(TARGET RED4ext::SDK)
  # the parent project brought it
elseif(RG_RED4EXT_SDK_DIR)
  if(NOT EXISTS "${RG_RED4EXT_SDK_DIR}/CMakeLists.txt")
    message(FATAL_ERROR "RG_RED4EXT_SDK_DIR=${RG_RED4EXT_SDK_DIR} has no RED4ext.SDK")
  endif()
  add_subdirectory("${RG_RED4EXT_SDK_DIR}" "${CMAKE_BINARY_DIR}/RED4ext.SDK" EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(red4ext_sdk GIT_REPOSITORY https://github.com/WopsS/RED4ext.SDK.git
                       GIT_TAG bbc8c5c7a4a68567f015968c5301e718e3fce238)  # keep in sync with sdk/examples/
  FetchContent_MakeAvailable(red4ext_sdk)
endif()

set(rg_common_defs WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
set(rg_msvc_opts /W4 /EHsc /utf-8 /external:anglebrackets /external:W0)

# ---- the API: what a client compiles against ----
add_library(redgraph_api INTERFACE)
target_include_directories(redgraph_api INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}/include")
add_library(REDGraph::api ALIAS redgraph_api)

# ---- the host ----
add_library(redgraph SHARED
  include/rg_host.h
  src/plugin.cpp
  src/settings.cpp
  src/settings.hpp
  src/ui.cpp
  src/ui.hpp
  src/common/dxil_hash.hpp
  src/common/ini_file.hpp
  src/core/host.cpp
  src/core/host.hpp
  src/core/clients.cpp
  src/core/clients.hpp
  src/core/prof.cpp
  src/core/prof.hpp
  src/core/internal.hpp
  src/core/engine_231.hpp
  src/core/dxbc.hpp
  src/core/pso_patch.cpp
  src/core/bindings.cpp
  src/core/rt_lib.cpp
  src/core/passes.cpp
  src/core/graph_probe.cpp
  src/core/graph_debug.cpp
  src/core/graph_debug.hpp
  src/core/gpu_timing.cpp
  src/core/gpu_timing.hpp
  src/core/hlsl.cpp
  src/core/render_flow.cpp
  src/core/render_nodes.cpp
  src/core/cb_hooks.cpp
  src/core/cb_hooks.hpp
  src/core/crash_log.cpp
  src/core/crash_log.hpp
)
target_compile_features(redgraph PRIVATE cxx_std_20)
target_compile_definitions(redgraph PRIVATE ${rg_common_defs}
  RG_HOST_BUILD  # rg_host.h: rg_host_get is exported (dllexport) for the clients
  RG_VERSION_STRING="${PROJECT_VERSION}"
  RG_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  RG_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  RG_VERSION_PATCH=${PROJECT_VERSION_PATCH}
  RG_NATIVE_HOST=$<BOOL:${RG_NATIVE_HOST}>
)
target_include_directories(redgraph PRIVATE src src/common)
target_link_libraries(redgraph PRIVATE REDGraph::api RED4ext::SDK)
if(MSVC)
  target_compile_options(redgraph PRIVATE ${rg_msvc_opts})
  # d3d12.dll must NOT be a load-time import: RED4ext loads plugins long before the game imports d3d12, and the loader
  # would then resolve it to System32's - a d3d12.dll proxy in bin/x64 (OptiScaler) would never load. Delay-loaded, it
  # resolves at the first D3D12 call, after the engine's device exists, to whatever d3d12 the game loaded.
  target_link_options(redgraph PRIVATE /DELAYLOAD:d3d12.dll)
  target_link_libraries(redgraph PRIVATE delayimp)
endif()
set_target_properties(redgraph PROPERTIES PREFIX "" OUTPUT_NAME "REDGraph" SUFFIX ".dll")

# ---- the self-tests, a client plugin (selftest/selftest.hpp): REDGraphSelfTest.dll + REDGraphSelfTest.ini ----
if(RG_BUILD_SELFTEST)
  add_library(redgraph_selftest SHARED
    selftest/plugin.cpp
    selftest/selftest.hpp
    selftest/hash_literal.hpp
    selftest/flow_probe.cpp
    selftest/rt_variant_test.cpp
    selftest/rt_swap_test.cpp
    selftest/scope_test.cpp
    selftest/lease_tex_test.cpp
    selftest/compose_test.cpp
    selftest/state_test.cpp
    selftest/wrap_use_test.cpp
    selftest/draw_test.cpp
    selftest/hlsl_test.cpp
    selftest/sky_paint_shader.hpp
    selftest/lease_tex_shader.hpp
  )
  target_compile_features(redgraph_selftest PRIVATE cxx_std_20)
  target_compile_definitions(redgraph_selftest PRIVATE ${rg_common_defs})
  target_include_directories(redgraph_selftest PRIVATE selftest src/common)
  target_link_libraries(redgraph_selftest PRIVATE REDGraph::api RED4ext::SDK)
  if(MSVC)
    target_compile_options(redgraph_selftest PRIVATE ${rg_msvc_opts})
    target_link_options(redgraph_selftest PRIVATE /DELAYLOAD:d3d12.dll)
    target_link_libraries(redgraph_selftest PRIVATE d3d12 delayimp)
  endif()
  set_target_properties(redgraph_selftest PROPERTIES PREFIX "" OUTPUT_NAME "REDGraphSelfTest" SUFFIX ".dll")

  # RtVariantTest / RtSwapTest / PatchScopeTest need game bytecode: made from the installed game at build time, else compiled out.
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_FOUND AND RG_GAME_DIR AND RG_DXC_DIR AND EXISTS "${RG_GAME_DIR}/engine/staticshader_final.cache"
     AND EXISTS "${RG_DXC_DIR}/dxcompiler.dll")
    set(rg_gen "${CMAKE_BINARY_DIR}/selftest_blobs")
    set(rg_blobs "${rg_gen}/patches/rt_variant_test_318cee44.hpp" "${rg_gen}/patches/rt_swap_test_318cee44.hpp"
                 "${rg_gen}/patches/selftest_vanilla_0c359133.hpp" "${rg_gen}/patches/selftest_vanilla_69090ccd.hpp")
    add_custom_command(
      OUTPUT ${rg_blobs}
      COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/make_selftest_blobs.py"
              --game "${RG_GAME_DIR}" --dxc "${RG_DXC_DIR}" --out "${rg_gen}"
      DEPENDS scripts/make_selftest_blobs.py sdk/tools/rg_shader_cache.py sdk/tools/rg_dxil.py
              sdk/tools/rt_raygen_variant.py
      COMMENT "Self-test blobs from the installed game"
      VERBATIM)
    target_sources(redgraph_selftest PRIVATE ${rg_blobs})
    target_include_directories(redgraph_selftest PRIVATE "${rg_gen}")
    target_compile_definitions(redgraph_selftest PRIVATE RG_SELFTEST_GAME_BLOBS=1)
    message(STATUS "REDGraph self-test: game blobs from ${RG_GAME_DIR}")
  else()
    target_compile_definitions(redgraph_selftest PRIVATE RG_SELFTEST_GAME_BLOBS=0)
    message(STATUS "REDGraph self-test: RtVariantTest / RtSwapTest / PatchScopeTest compiled out (set RG_GAME_DIR and RG_DXC_DIR)")
  endif()
endif()

# ---- the SDK's smallest client (sdk/examples/rg_client_example/, also built standalone from the SDK package) ----
if(RG_BUILD_EXAMPLE)
  add_library(rg_example_client SHARED sdk/examples/rg_client_example/client.cpp)
  target_compile_features(rg_example_client PRIVATE cxx_std_20)
  target_compile_definitions(rg_example_client PRIVATE ${rg_common_defs})
  target_link_libraries(rg_example_client PRIVATE REDGraph::api RED4ext::SDK)
  if(MSVC)
    target_compile_options(rg_example_client PRIVATE ${rg_msvc_opts})
  endif()
  set_target_properties(rg_example_client PROPERTIES PREFIX "" OUTPUT_NAME "REDGraphExampleClient" SUFFIX ".dll")
endif()
