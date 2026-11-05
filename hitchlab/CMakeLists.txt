# SPDX-License-Identifier: (LGPL-2.1 OR BSD-2-Clause)
#
# hitchlab: the case-study game as an app of its own. Pick a case, press Start,
# and the game runs with that case's cause of hitches around it, ready to be
# recorded like any game (Hitchtrace Explorer › Record).
#
#   cmake -S hitchlab -B build/hitchlab -G Ninja && cmake --build build/hitchlab
#   build/hitchlab/hitchlab
#
# One executable: the window, and (hitchlab --run NAME ...) the game and the
# programs that make up its surroundings, built from the case-study sources in
# cases/. The timer-slack shim is the one separate file: it is preloaded into
# the game, so it has to be a shared library.
cmake_minimum_required(VERSION 3.20)
project(hitchlab LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS Widgets)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(GAMELIBS REQUIRED IMPORTED_TARGET vulkan xcb)

set(CASES ${CMAKE_CURRENT_SOURCE_DIR}/../cases)

# The programs, each one's main() renamed so they can share one binary.
# Frame pointers so that hitchtrace -U can walk the game's stacks.
set(PROGRAMS
  game ${CASES}/minigame/minigame.c
  bgload ${CASES}/bgload.c
  bgio ${CASES}/bgio.c
  ipcserver ${CASES}/ipcserver.c
  squeeze ${CASES}/squeeze.c
  frag ${CASES}/frag.c)
set(PROGRAM_SOURCES)
while(PROGRAMS)
  list(POP_FRONT PROGRAMS name src)
  set_source_files_properties(${src} PROPERTIES COMPILE_DEFINITIONS main=${name}_main)
  list(APPEND PROGRAM_SOURCES ${src})
endwhile()
add_library(labprograms STATIC ${PROGRAM_SOURCES})
target_compile_options(labprograms PRIVATE -fno-omit-frame-pointer -Wall -Wno-unused-parameter)
target_link_libraries(labprograms PUBLIC PkgConfig::GAMELIBS Threads::Threads m)

add_executable(hitchlab
  src/main.cpp
  src/lab_cases.cpp
  src/lab_window.cpp)
target_include_directories(hitchlab PRIVATE src)
target_link_libraries(hitchlab PRIVATE labprograms Qt6::Widgets)
target_compile_options(hitchlab PRIVATE -Wall -Wextra -fno-omit-frame-pointer)

# libhitchlab_slack.so next to the executable
add_library(hitchlab_slack MODULE ${CASES}/slack.c)
target_link_libraries(hitchlab_slack PRIVATE ${CMAKE_DL_LIBS})
target_compile_options(hitchlab_slack PRIVATE -Wall)
