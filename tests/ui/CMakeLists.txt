cmake_minimum_required(VERSION 3.22)
project(AssistantUiTest LANGUAGES CXX)
find_package(Qt6 REQUIRED COMPONENTS Core Widgets Sql Network Test)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
add_executable(assistant-ui-test main.cpp ../../src/ChatPanel.cpp ../../src/ChatUi.cpp ../../src/ChatStore.cpp ../../src/ManiVaultControls.cpp ../../src/KnowledgeIndex.cpp ../../src/OfflineInference.cpp ../../src/RuntimePaths.cpp ../../resources/assistant.qrc)
target_compile_features(assistant-ui-test PRIVATE cxx_std_20)
target_include_directories(assistant-ui-test PRIVATE ../../src "${TOOLTIP_SOURCE}/include")
# Layout/theme tests use the adapter's plain-widget implementation because native
# WidgetAction labels require the running host's theme and project managers.
# The production plugin always compiles with ASSISTANT_MANIVAULT_ACTIONS enabled.
target_link_libraries(assistant-ui-test PRIVATE Qt6::Widgets Qt6::Sql Qt6::Network Qt6::Test)
if(WIN32)
  target_link_libraries(assistant-ui-test PRIVATE dbghelp)
endif()
