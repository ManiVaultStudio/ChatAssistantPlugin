cmake_minimum_required(VERSION 3.22)
project(ChatAssistantPlugin VERSION 1.0.0 LANGUAGES CXX)

include(FetchContent)
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()
set(CMAKE_INCLUDE_CURRENT_DIR ON)

option(ASSISTANT_BUILD_LLAMA "Build bundled offline llama.cpp runtime" ON)
set(_default_autoinstall OFF)
if(TARGET ManiVault::Core)
  set(_default_autoinstall ON)
endif()
option(ASSISTANT_AUTOINSTALL "Install plugin and assets to the ManiVault development bundle after build" ${_default_autoinstall})
option(ASSISTANT_PACKAGE_REQUIRED "Fail configuration if a model package is not supplied" OFF)
set(_default_metal OFF)
if(APPLE)
  set(_default_metal ON)
endif()
option(ASSISTANT_ENABLE_METAL "Use Metal acceleration for the bundled inference runtime" ${_default_metal})
option(ASSISTANT_ENABLE_AVX2 "Use AVX2/FMA CPU acceleration on x86 (disable for older CPUs)" ON)
set(ASSISTANT_GPU_BACKEND "AUTO" CACHE STRING "Inference backend: AUTO, CPU, METAL or VULKAN")
set_property(CACHE ASSISTANT_GPU_BACKEND PROPERTY STRINGS AUTO CPU METAL VULKAN)
set(ASSISTANT_PACKAGE_DIR "" CACHE PATH "Local trainer release folder containing assistant.gguf and knowledge.db")
set(_default_package_url "https://sec.lumc.nl/mtg-viewer/MV_data/CytosploreViewer/TutorialModels/manivault-trained-assistant-small.zip")
set(_default_package_sha256 "786A6A50B3585205C88F896803834937D2FCC53723F359DA3FC83CD5AE0E9369")
#[[
    USE Either
        set(_default_package_url "https://sec.lumc.nl/mtg-viewer/MV_data/CytosploreViewer/TutorialModels/manivault-trained-assistant-small.zip")
        set(_default_package_sha256 "786A6A50B3585205C88F896803834937D2FCC53723F359DA3FC83CD5AE0E9369")
    OR
        set(_default_package_url "https://sec.lumc.nl/mtg-viewer/MV_data/CytosploreViewer/TutorialModels/manivault-trained-assistant-large.zip")
        set(_default_package_sha256 "FF26D6ADB246EC627A913825364C9EB523B0B11DCDA33347858F9DDD34E4430D")
]]
if(ASSISTANT_PACKAGE_DIR)
  set(_default_package_url "")
  set(_default_package_sha256 "")
endif()
set(ASSISTANT_PACKAGE_URL "${_default_package_url}" CACHE STRING "HTTPS URL for a ZIP of the trainer release")
set(ASSISTANT_PACKAGE_SHA256 "${_default_package_sha256}" CACHE STRING "SHA256 digest of the release ZIP (required when using URL)")
set(ASSISTANT_LLAMA_SOURCE_DIR "" CACHE PATH "Optional local checkout of llama.cpp to skip network fetch")
set(ASSISTANT_LLAMA_COMMIT "2cbfc3cf34f94c8ada67eb8766efe7374a3d2db6" CACHE STRING "Pinned llama.cpp revision from the successful trainer export")
set(ASSISTANT_LLAMA_SOURCE_SHA256 "0781a7b42f63de1613b296b22998099ba4b01641dc7743d422ea9a9791698144" CACHE STRING "SHA256 of the pinned llama.cpp source archive")

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Widgets Sql Network)
if(NOT TARGET ManiVault::Core)
  find_package(ManiVault COMPONENTS Core CONFIG QUIET)
endif()
if(NOT TARGET ManiVault::Core)
  message(FATAL_ERROR "ManiVault::Core is unavailable. Add this plugin after add_subdirectory(core) in a development bundle, or set ManiVault_DIR to the installed SDK's cmake/mv directory for a standalone build.")
endif()
mv_project_defaults()

if(NOT TARGET QtTooltipKit::Tooltips)
  set(ASSISTANT_TOOLTIP_SOURCE_DIR "" CACHE PATH "Optional local Qt Tooltip Kit checkout")
  if(ASSISTANT_TOOLTIP_SOURCE_DIR)
    add_subdirectory("${ASSISTANT_TOOLTIP_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/tooltip-kit" EXCLUDE_FROM_ALL)
  elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../qt-tooltip-kit/CMakeLists.txt")
    add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../qt-tooltip-kit" "${CMAKE_CURRENT_BINARY_DIR}/tooltip-kit" EXCLUDE_FROM_ALL)
  else()
    FetchContent_Declare(assistant_tooltips
      URL "https://codeload.github.com/sbvis/qt-tooltip-kit/tar.gz/4940bfd0a17358a2b7e17e12d41688c22b9a9615"
      URL_HASH "SHA256=3437036ec6804c28bb21a087ba160cdf0eebf97fdc3c33632ce526188da8f438")
    FetchContent_MakeAvailable(assistant_tooltips)
  endif()
endif()

# Configure-time acquisition: runtime never downloads from the internet.
get_target_property(_assistant_tooltip_source QtTooltipKit::Tooltips SOURCE_DIR)
if(EXISTS "${_assistant_tooltip_source}/LICENSE")
  install(FILES "${_assistant_tooltip_source}/LICENSE"
    DESTINATION Plugins/ChatAssistantPlugin/licenses RENAME QtTooltipKit-LICENSE COMPONENT PLUGINS)
endif()
install(FILES LICENSE NOTICE DESTINATION Plugins/ChatAssistantPlugin/licenses COMPONENT PLUGINS)
install(FILES resources/nlohmann-json-LICENSE DESTINATION Plugins/ChatAssistantPlugin/licenses COMPONENT PLUGINS)
install(FILES docs/USER_GUIDE.md DESTINATION Plugins/ChatAssistantPlugin/docs COMPONENT PLUGINS)
set(ASSISTANT_PACKAGE_OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/assistant-package")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/AcquirePackage.cmake")

if(ASSISTANT_BUILD_LLAMA)
  # Keep nested shader-tool compiler checks below Windows' legacy path limit.
  # This directory property is inherited only by this plugin's dependencies.
  include(ExternalProject)
  set_property(DIRECTORY PROPERTY EP_BASE "${CMAKE_CURRENT_BINARY_DIR}/ep")
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/SelectGpuBackend.cmake")
  string(TOUPPER "${ASSISTANT_GPU_BACKEND}" _assistant_requested)
  set(_assistant_vulkan_available OFF)
  if(DEFINED ENV{VULKAN_SDK})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{VULKAN_SDK}")
  endif()
  if(_assistant_requested STREQUAL "AUTO" AND NOT APPLE)
    find_package(Vulkan COMPONENTS glslc QUIET)
    find_package(SPIRV-Headers CONFIG QUIET)
    if(Vulkan_FOUND AND TARGET SPIRV-Headers::SPIRV-Headers)
      set(_assistant_vulkan_available ON)
    endif()
  endif()
  if(_assistant_requested STREQUAL "AUTO" AND APPLE AND NOT ASSISTANT_ENABLE_METAL)
    set(_assistant_requested CPU)
  endif()
  assistant_select_gpu_backend("${_assistant_requested}" "${CMAKE_SYSTEM_NAME}"
    "${_assistant_vulkan_available}" _assistant_backend)
  set(GGML_VULKAN OFF CACHE BOOL "Assistant Vulkan backend" FORCE)
  set(GGML_METAL OFF CACHE BOOL "Assistant Metal backend" FORCE)
  if(_assistant_backend STREQUAL "VULKAN")
    set(GGML_VULKAN ON CACHE BOOL "Assistant Vulkan backend" FORCE)
  elseif(_assistant_backend STREQUAL "METAL")
    set(GGML_METAL ON CACHE BOOL "Assistant Metal backend" FORCE)
  endif()
  message(STATUS "Knowledge Assistant inference backend: ${_assistant_backend}")
  # Keep the inference executable and its dependencies local; no HTTP API/server.
  set(BUILD_SHARED_LIBS OFF)
  set(LLAMA_TOOLS_INSTALL OFF CACHE BOOL "Install only the assistant runtime" FORCE)
  set(LLAMA_BUILD_COMMON OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_MTMD OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_SERVER OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(LLAMA_CURL OFF CACHE BOOL "" FORCE)
  set(GGML_METAL_EMBED_LIBRARY ON CACHE BOOL "" FORCE)
  # Distributed runtimes must not inherit AVX512 or other build-machine-only flags.
  set(GGML_NATIVE OFF CACHE BOOL "Optimize only for the build machine CPU")
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|x64|i.86)$")
    # Explicit baseline also repairs caches created while native mode disabled
    # all SIMD defaults. Do not require build-machine-specific AVX512.
    foreach(_feature SSE42 AVX AVX2 FMA F16C)
      set(GGML_${_feature} ${ASSISTANT_ENABLE_AVX2} CACHE BOOL "Assistant x86 SIMD baseline" FORCE)
    endforeach()
  endif()
  if(ASSISTANT_LLAMA_SOURCE_DIR)
    if(NOT EXISTS "${ASSISTANT_LLAMA_SOURCE_DIR}/CMakeLists.txt")
      message(FATAL_ERROR "Invalid ASSISTANT_LLAMA_SOURCE_DIR")
    endif()
    add_subdirectory("${ASSISTANT_LLAMA_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/llama-build" EXCLUDE_FROM_ALL)
    set(_assistant_llama_source "${ASSISTANT_LLAMA_SOURCE_DIR}")
  else()
    FetchContent_Declare(assistant_llama
      URL "https://codeload.github.com/ggml-org/llama.cpp/tar.gz/${ASSISTANT_LLAMA_COMMIT}"
      URL_HASH "SHA256=${ASSISTANT_LLAMA_SOURCE_SHA256}"
      PATCH_COMMAND "${CMAKE_COMMAND}" "-DLLAMA_SOURCE_DIR=<SOURCE_DIR>"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/IncrementalVulkanShaders.cmake")
    FetchContent_MakeAvailable(assistant_llama)
    set(_assistant_llama_source "${assistant_llama_SOURCE_DIR}")
  endif()
  install(FILES "${_assistant_llama_source}/LICENSE"
    DESTINATION Plugins/ChatAssistantPlugin/licenses RENAME llama.cpp-LICENSE COMPONENT PLUGINS)
  add_executable(assistant-runtime src/AssistantRuntime.cpp)
  target_include_directories(assistant-runtime PRIVATE "${_assistant_llama_source}/vendor/nlohmann")
  target_compile_features(assistant-runtime PRIVATE cxx_std_17)
  target_link_libraries(assistant-runtime PRIVATE llama)
  if(NOT _assistant_requested STREQUAL "AUTO" AND NOT _assistant_backend STREQUAL "CPU")
    target_compile_definitions(assistant-runtime PRIVATE ASSISTANT_REQUIRE_GPU=1)
  endif()
  # Solution folders affect only this dependency subtree, never sibling plugins.
  set_property(GLOBAL PROPERTY USE_FOLDERS ON)
  function(assistant_group_dependency_targets directory)
    get_property(_targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_target IN LISTS _targets)
      set_property(TARGET "${_target}" PROPERTY FOLDER "ChatAssistantPlugin/Dependencies")
    endforeach()
    get_property(_children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(_child IN LISTS _children)
      assistant_group_dependency_targets("${_child}")
    endforeach()
  endfunction()
  assistant_group_dependency_targets("${_assistant_llama_source}")
  set_target_properties(assistant-runtime PROPERTIES FOLDER "ChatAssistantPlugin/Runtime")
endif()

set(PLUGIN_SOURCES
  src/ChatAssistantPlugin.h
  src/ChatAssistantPlugin.cpp
  src/ChatPanel.h
  src/ChatPanel.cpp
  src/KnowledgeIndex.h
  src/KnowledgeIndex.cpp
  src/OfflineInference.h
  src/OfflineInference.cpp
  src/RuntimePaths.h
  src/RuntimePaths.cpp
  src/ChatStore.h
  src/ChatStore.cpp
  src/ChatUi.h
  src/ChatUi.cpp
  src/ManiVaultControls.h
  src/ManiVaultControls.cpp
  resources/assistant.qrc
  PluginInfo.json
)
add_library(${PROJECT_NAME} SHARED ${PLUGIN_SOURCES})
target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_20)
set_target_properties(${PROJECT_NAME} PROPERTIES AUTOMOC ON AUTORCC ON UNITY_BUILD OFF)
set_property(TARGET ${PROJECT_NAME} PROPERTY FOLDER "ChatAssistantPlugin")
target_include_directories(${PROJECT_NAME} PRIVATE "${ManiVault_INCLUDE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_link_libraries(${PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets Qt6::Sql Qt6::Network ManiVault::Core QtTooltipKit::Tooltips)
target_compile_definitions(${PROJECT_NAME} PRIVATE ASSISTANT_MANIVAULT_ACTIONS=1)
target_compile_definitions(${PROJECT_NAME} PRIVATE
  ASSISTANT_DEV_PACKAGE_DIR="${ASSISTANT_PACKAGE_OUTPUT}"
  ASSISTANT_DEV_LLAMA_BIN="${CMAKE_CURRENT_BINARY_DIR}/bin")

# Use ManiVault naming/metadata conventions instead of custom output filenames.
mv_handle_plugin_config(${PROJECT_NAME} 0)
install(TARGETS ${PROJECT_NAME}
  RUNTIME DESTINATION Plugins COMPONENT PLUGINS
  LIBRARY DESTINATION Plugins COMPONENT PLUGINS)
if(ASSISTANT_BUILD_LLAMA)
  target_compile_definitions(${PROJECT_NAME} PRIVATE ASSISTANT_DEV_LLAMA_FILE=\"$<TARGET_FILE:assistant-runtime>\")
  add_dependencies(${PROJECT_NAME} assistant-runtime)
  install(TARGETS assistant-runtime RUNTIME DESTINATION Plugins/ChatAssistantPlugin/runtime COMPONENT PLUGINS)
endif()
if(EXISTS "${ASSISTANT_PACKAGE_OUTPUT}/knowledge.db")
  install(DIRECTORY "${ASSISTANT_PACKAGE_OUTPUT}/"
    DESTINATION Plugins/ChatAssistantPlugin/data COMPONENT PLUGINS
    PATTERN "model-package.zip" EXCLUDE
    PATTERN "*.partial" EXCLUDE
    PATTERN ".package-sha256" EXCLUDE
    PATTERN ".archive-extract" EXCLUDE)
endif()

if(ASSISTANT_AUTOINSTALL)
  if(NOT ManiVault_INSTALL_DIR)
    message(FATAL_ERROR "ASSISTANT_AUTOINSTALL requires ManiVault_INSTALL_DIR")
  endif()
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" --install "${CMAKE_CURRENT_BINARY_DIR}"
      --config $<CONFIG> --component PLUGINS --prefix "${ManiVault_INSTALL_DIR}/$<CONFIG>"
    VERBATIM)
endif()

if(MSVC)
  set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_WORKING_DIRECTORY
    "${ManiVault_INSTALL_DIR}/$<CONFIG>")
  set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_COMMAND
    "${ManiVault_INSTALL_DIR}/$<CONFIG>/ManiVault Studio.exe")
  set_property(TARGET ${PROJECT_NAME} PROPERTY VS_DEBUGGER_ENVIRONMENT
    "PATH=$<TARGET_FILE_DIR:Qt6::Core>;%PATH%")
endif()
