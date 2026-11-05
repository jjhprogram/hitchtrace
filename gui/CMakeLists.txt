# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# hitchexplorer: a desktop viewer for hitchtrace recordings (Qt 6 Widgets).
#
#   cmake -S gui -B build/gui -G Ninja && cmake --build build/gui
#   build/gui/hitchexplorer cases/out/*/hitch.jsonl
#   ctest --test-dir build/gui        # analysis checks and a GUI smoke test on the case data
cmake_minimum_required(VERSION 3.20)
project(hitchexplorer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Core Gui Widgets)

# Records and the analysis on them: no widgets, so tests can use it alone.
add_library(htcore STATIC
  src/data/record.cpp
  src/analysis/analysis.cpp)
target_include_directories(htcore PUBLIC src)
target_link_libraries(htcore PUBLIC Qt6::Core)
target_compile_options(htcore PRIVATE -Wall -Wextra)

# The views and the window, shared by the app and the smoke test.
add_library(htui STATIC
  src/ui/theme.cpp
  src/ui/session.cpp
  src/ui/session_strip.cpp
  src/ui/ribbon.cpp
  src/ui/lanes.cpp
  src/ui/blame_view.cpp
  src/ui/blame_panel.cpp
  src/ui/main_window.cpp
  src/ui/record_dialog.cpp
  src/ui/stack_panel.cpp)
target_link_libraries(htui PUBLIC htcore Qt6::Widgets)
target_compile_options(htui PRIVATE -Wall -Wextra)

add_executable(hitchexplorer src/main.cpp)
target_link_libraries(hitchexplorer PRIVATE htui)
target_compile_options(hitchexplorer PRIVATE -Wall -Wextra)

enable_testing()
add_executable(test_analysis tests/test_analysis.cpp)
target_link_libraries(test_analysis PRIVATE htcore)
target_compile_definitions(test_analysis PRIVATE
  CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../cases/out")
add_test(NAME analysis COMMAND test_analysis)

# Drives every view offscreen through the interactions on the case data and
# saves a screenshot of each state into gui-smoke/ in the build directory.
add_executable(gui_smoke tests/gui_smoke.cpp)
target_link_libraries(gui_smoke PRIVATE htui)
target_compile_definitions(gui_smoke PRIVATE
  CASES_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../cases/out")
add_test(NAME gui_smoke COMMAND gui_smoke gui-smoke)
set_tests_properties(gui_smoke PROPERTIES ENVIRONMENT QT_QPA_PLATFORM=offscreen)
