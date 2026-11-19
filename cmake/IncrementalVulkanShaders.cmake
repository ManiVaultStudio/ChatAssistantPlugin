# Track inputs to the fetched Vulkan host tool so incremental builds can skip it.
set(_file "${LLAMA_SOURCE_DIR}/ggml/src/ggml-vulkan/CMakeLists.txt")
file(READ "${_file}" _original)
if(_original MATCHES "assistant_shader_inputs")
  return()
endif()
set(_before [=[    ExternalProject_Add(
        vulkan-shaders-gen]=])
set(_after [=[    file(GLOB _assistant_shader_inputs CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan-shaders/*.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/vulkan-shaders/*.h")
    set(_assistant_shader_manifest "${CMAKE_CURRENT_BINARY_DIR}/vulkan-shaders-gen-inputs.txt")
    file(CONFIGURE OUTPUT "${_assistant_shader_manifest}" CONTENT "@_assistant_shader_inputs@")
    set(_assistant_shader_install_args)
    if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.26)
        list(APPEND _assistant_shader_install_args INSTALL_BYPRODUCTS
            "${CMAKE_BINARY_DIR}/$<CONFIG>/vulkan-shaders-gen$<IF:$<STREQUAL:${CMAKE_HOST_SYSTEM_NAME},Windows>,.exe,>")
    endif()
    ExternalProject_Add(
        vulkan-shaders-gen]=])
string(REPLACE "${_before}" "${_after}" _patched "${_original}")
string(REPLACE "        BUILD_ALWAYS  TRUE" "        \${_assistant_shader_install_args}" _patched "${_patched}")
set(_before [=[    set (_ggml_vk_host_suffix]=])
set(_after [=[    ExternalProject_Add_Step(vulkan-shaders-gen generator-inputs
        DEPENDEES configure
        DEPENDERS build
        DEPENDS ${_assistant_shader_inputs} "${_assistant_shader_manifest}"
                "${CMAKE_CURRENT_SOURCE_DIR}/vulkan-shaders/CMakeLists.txt"
                ${HOST_CMAKE_TOOLCHAIN_FILE})

    set (_ggml_vk_host_suffix]=])
string(REPLACE "${_before}" "${_after}" _patched "${_patched}")
if(NOT _original MATCHES "BUILD_ALWAYS  TRUE"
   OR NOT _patched MATCHES "ExternalProject_Add_Step\\(vulkan-shaders-gen generator-inputs"
   OR NOT _patched MATCHES "file\\(GLOB _assistant_shader_inputs")
  message(FATAL_ERROR "The llama.cpp Vulkan host-tool rules do not match the incremental build patch.")
endif()
file(WRITE "${_file}" "${_patched}")
