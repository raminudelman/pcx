cmake_minimum_required(VERSION 2.6)
 
# The project() command sets variable PROJECT_SOURCE_DIR and PCX_SOURCE_DIR
project(PCX CXX C)

set(PCX_VERSION_MAJOR 0)
set(PCX_VERSION_MINOR 1)
set(PCX_VERSION_PATCH 0)
set(PCX_VERSION
    "${PCX_VERSION_MAJOR}.${PCX_VERSION_MINOR}.${PCX_VERSION_PATCH}")

################################################################################
####                      Options And Configuration                         ####
################################################################################

# Build PCX with debug prints
option(PCX_DEBUG "Build PCX with debug prints" OFF)

# Build target options
option(BUILD_TEST "Build test binary (requires gtest package)" OFF)

# Build benchmark options 
# TODO: Need to actually build the benchmark before using this option
option(BUILD_BENCHMARK "Build benchmark binary" OFF)

# Set default build type
if(NOT CMAKE_BUILD_TYPE)
  message(STATUS "Build type not set. Defaulting to 'Release'")
  set(CMAKE_BUILD_TYPE "Release")
endif()

# Add install targets by default (override from parent project)
set(PCX_INSTALL ON CACHE BOOL "Defines if to install PCX into the file system")
mark_as_advanced(PCX_INSTALL)

# Build shared or static libraries (override from parent project)
if(BUILD_SHARED_LIBS)
  set(PCX_STATIC_OR_SHARED SHARED CACHE STRING "")
else()
  set(PCX_STATIC_OR_SHARED STATIC CACHE STRING "")
endif()
mark_as_advanced(PCX_STATIC_OR_SHARED)

################################################################################
####                              Dependencies                              ####
################################################################################

set(pcx_DEPENDENCY_LIBS "")

# Local CMake modules
# Configure path to modules (for find_package)
list(APPEND CMAKE_MODULE_PATH ${PROJECT_SOURCE_DIR}/cmake/Modules)

# Find the ibverbs library
find_package(ibverbs REQUIRED)
# If ibverbs package was found, the IBVERBS_FOUND is set by CMake
if(IBVERBS_FOUND)
  include_directories(SYSTEM ${IBVERBS_INCLUDE_DIRS})
  list(APPEND pcx_DEPENDENCY_LIBS ${IBVERBS_LIBRARIES})
  message(INFO " ibverbs lib: ${IBVERBS_LIBRARIES} ibverbs include: ${IBVERBS_INCLUDE_DIRS}  ")
else()
  message(FATAL "Cannot use PCX without ibverbs library")
endif()

# Find the mlx5 library
# If mlx5 package was found, the MLX5_FOUND is set by CMake
find_package(mlx5 REQUIRED)
if(MLX5_FOUND)
  include_directories(SYSTEM ${MLX5_INCLUDE_DIRS})
  list(APPEND pcx_DEPENDENCY_LIBS ${MLX5_LIBRARIES} )
  message(INFO " mlx5 lib: ${MLX5_LIBRARIES} mlx5 include: ${MLX5_INCLUDE_DIRS}  ")
else()
  message(FATAL "Cannot use PCX without mlx5 library")
endif()

# TODO: Need to add verbs_exp.h as a dependency for PCX until this dependency is removed. 

# Make sure we can find GoogleTest if building the tests
if(BUILD_TEST)
  # If the gtest target is already defined, we assume that the project that
  # includes PCX knows what they are doing and the version of gtest is new 
  # enough so there is no need for PCX to find gtest package. On the other hand
  # if the including project of PCX did not define gtest as a target, then PCX
  # will define it itself.
  if(NOT TARGET gtest)
    find_package(GTest REQUIRED)
    if(NOT GTEST_FOUND)
      message(FATAL_ERROR "Could not find googletest; cannot compile tests")
    endif()
    add_library(gtest INTERFACE)
    target_include_directories(gtest INTERFACE ${GTEST_INCLUDE_DIRS})
    target_link_libraries(gtest INTERFACE ${GTEST_LIBRARIES})
  endif()
endif()

# GTest Depends on pthreads and PCX uses it // TODO: Check if GTest needs pthread and if PCX needs pthread
list(APPEND pcx_DEPENDENCY_LIBS pthread)

################################################################################
####                                Compiler                                ####
################################################################################

# Compiler flags
# 1. Support c++11 standard
# 2. Generate position-independent code (PIC)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fPIC")

################################################################################
####                             Source Files                               ####
################################################################################

# TODO: Consider moving this code to ${PROJECT_SOURCE_DIR}/src/CMakeLists.txt

# PCX_SRCS is the list of source files that we need to build libpcx.
set(PCX_SRCS)

# PCX_HDRS is the list of header files that we need to install.
set(PCX_HDRS)

# Compiled sources in root directory
set(SRC_PATH "${CMAKE_CURRENT_SOURCE_DIR}/src")
list(APPEND PCX_SRCS
  "${SRC_PATH}/pcx_verbs_ctx.cc"
  "${SRC_PATH}/pcx_ring_qp.cc"
  "${SRC_PATH}/pcx_qps.cc"
  "${SRC_PATH}/pcx_king_qp.cc"
  "${SRC_PATH}/pcx_allreduce_alg_common.cc"
  "${SRC_PATH}/pcx_mem.cc"
  "${SRC_PATH}/pcx_comm_graph.cc"
  "${SRC_PATH}/pcx_dv_mgr.cc"
  )

set(HEADERS_PATH "${PROJECT_SOURCE_DIR}/include/pcx")
list(APPEND PCX_HDRS
  "${HEADERS_PATH}/pcx_verbs_ctx.h"
  "${HEADERS_PATH}/pcx_ring_qp.h"
  "${HEADERS_PATH}/pcx_qps.h"
  "${HEADERS_PATH}/pcx_mem.h"
  "${HEADERS_PATH}/pcx_king_qp.h"
  "${HEADERS_PATH}/pcx_allreduce_alg_king.h"
  "${HEADERS_PATH}/pcx_allreduce_alg_common.h"
  "${HEADERS_PATH}/pcx_comm_graph.h"
  "${HEADERS_PATH}/pcx_dv_mgr.h"
  "${HEADERS_PATH}/pcx_allreduce_alg_chunked_ring.h"
  )


# Enable the following to get a list of source files
# TODO: This is only for debug the cmake build process. 
#       How to add a flag that controls this print?
if(1) 
  message(INFO " Sources: ")
  foreach(pcx_src_file ${PCX_SRCS})
    message(INFO "  " ${pcx_src_file})
  endforeach()
  message(INFO " Headers: ")
  foreach(pcx_hdr_file ${PCX_HDRS})
    message(INFO "  " ${pcx_hdr_file})
  endforeach()
endif()

# Write configuration header.
message(INFO " PCX_DEBUG=" ${PCX_DEBUG})
configure_file(include/cmake/pcx_config.h.in "${CMAKE_CURRENT_BINARY_DIR}/pcx_config.h")

# Create a shared or static library
add_library(pcx ${PCX_STATIC_OR_SHARED} ${PCX_SRCS})

# Link to the 'pcx' library that was just created all the libraries
# that are defined as dependencies
# TODO: Consider using PUBLIC instead of PRIVATE
target_link_libraries(pcx PRIVATE ${pcx_DEPENDENCY_LIBS}) 

# Add "include" directory for all the header files of the library
target_include_directories(pcx PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/pcx> # For headers when building
  $<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}> # for pcx_config.h when building
  $<INSTALL_INTERFACE:include/pcx/include/pcx>  # <prefix>/include/pcx
)

################################################################################
####                             Other Files                                ####
################################################################################

# Recurse into test project directory
if(BUILD_TEST)
  add_subdirectory(tests)
endif()

# Recurse into test project directory
if(BUILD_BENCHMARK)
  #add_subdirectory(benchmark) # TODO: Add benchmark directory
endif()

################################################################################
####                             Installation                               ####
################################################################################

set(PCX_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})
message(INFO " PCX Install information: ")
message(INFO "   PCX_INSTALL_PREFIX=" ${PCX_INSTALL_PREFIX})
message(INFO "   PCX_INSTALL=" ${PCX_INSTALL})

# Install if necessary.
# If the PCX build is included from another project's build, it may
# want to statically link with PCX and not install any artifacts, which in
# this case the PCX_INSTALL flag will be 'FALSE'.
if(PCX_INSTALL)
  message(INFO " Installing PCX into the prefix:" ${PCX_INSTALL_PREFIX})
  install(TARGETS pcx 
          EXPORT PcxTargets
          DESTINATION ${PCX_INSTALL_PREFIX}/lib/pcx)
  message(INFO " Installing " ${CMAKE_CURRENT_BINARY_DIR} "/pcx_config.h into: " ${PCX_INSTALL_PREFIX} "/include/pcx/include/pcx")
  install(FILES ${CMAKE_CURRENT_BINARY_DIR}/pcx_config.h
          DESTINATION ${PCX_INSTALL_PREFIX}/include/pcx/include/pcx)
  foreach(HEADER ${PCX_HDRS})
    string(REGEX MATCH "(.*)[/\\]" DIR ${HEADER})
    string(REGEX REPLACE "${CMAKE_CURRENT_SOURCE_DIR}" "pcx" DIR ${DIR})
    message(INFO " Installing " ${HEADER} " into: " ${PCX_INSTALL_PREFIX}/include/${DIR})
    install(FILES ${HEADER} 
            DESTINATION ${PCX_INSTALL_PREFIX}/include/${DIR})
  endforeach()
endif()

if(PCX_INSTALL)
  install(EXPORT PcxTargets 
          DESTINATION ${PCX_INSTALL_PREFIX}/share/cmake/PCX
          FILE PcxTargets.cmake
          COMPONENT dev)
endif()
