# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.18)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(sparta LANGUAGES CXX)
endif()

include(GNUInstallDirs)
find_package(Threads REQUIRED)

add_library(SPARTA INTERFACE)
add_library(SPARTA::SPARTA ALIAS SPARTA)
add_library(sparta::sparta ALIAS SPARTA)
add_library(sparta ALIAS SPARTA)
set_target_properties(SPARTA PROPERTIES EXPORT_NAME sparta)

target_compile_features(SPARTA INTERFACE cxx_std_17)
target_link_libraries(SPARTA INTERFACE Threads::Threads)
target_include_directories(SPARTA INTERFACE
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/Install.cmake")
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  sparta_install("${CMAKE_CURRENT_SOURCE_DIR}")
endif()
