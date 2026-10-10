# REDGraph example client, built on its own (the SDK package): cmake -B build -A x64 && cmake --build build --config Release
# -> build/Release/REDGraphExampleClient.dll; put it in <game>/red4ext/plugins/REDGraphExampleClient/.
# RED4ext.SDK: -DRED4EXT_SDK_DIR=<checkout>, else the pinned commit is fetched from GitHub.
# In the REDGraph repository the top-level CMakeLists.txt builds the same source (RG_BUILD_EXAMPLE); this file also
# configures there (rg_host.h is found in the repository's include/).
cmake_minimum_required(VERSION 3.20)
project(rg_example_client LANGUAGES CXX)

set(RED4EXT_SDK_DIR "" CACHE PATH "RED4ext.SDK checkout (empty = fetch)")
set(RED4EXT_HEADER_ONLY ON CACHE BOOL "" FORCE)
if(RED4EXT_SDK_DIR)
  add_subdirectory("${RED4EXT_SDK_DIR}" "${CMAKE_BINARY_DIR}/RED4ext.SDK" EXCLUDE_FROM_ALL)
else()
  include(FetchContent)
  FetchContent_Declare(red4ext_sdk GIT_REPOSITORY https://github.com/WopsS/RED4ext.SDK.git
                       GIT_TAG bbc8c5c7a4a68567f015968c5301e718e3fce238)  # the commit REDGraph is built against
  FetchContent_MakeAvailable(red4ext_sdk)
endif()

# rg_host.h: the SDK package's include/ (two levels up), or the repository's (three levels up)
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../include/rg_host.h")
  set(rg_include_default "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
else()
  set(rg_include_default "${CMAKE_CURRENT_SOURCE_DIR}/../../../include")
endif()
set(RG_INCLUDE_DIR "${rg_include_default}" CACHE PATH "folder holding rg_host.h")

add_library(rg_example_client SHARED client.cpp)
target_compile_features(rg_example_client PRIVATE cxx_std_20)
target_compile_definitions(rg_example_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_include_directories(rg_example_client PRIVATE "${RG_INCLUDE_DIR}")
target_link_libraries(rg_example_client PRIVATE RED4ext::SDK)
set_target_properties(rg_example_client PROPERTIES PREFIX "" OUTPUT_NAME "REDGraphExampleClient" SUFFIX ".dll")
