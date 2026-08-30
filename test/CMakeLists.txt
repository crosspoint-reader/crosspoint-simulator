cmake_minimum_required(VERSION 3.16)
project(crosspoint_simulator_tests C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
)
FetchContent_Declare(
  espressif_tlsf_src
  GIT_REPOSITORY https://github.com/espressif/tlsf.git
  GIT_TAG 2867f6883a12920b1969ff9624c0ab0e4185c2ce
)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest espressif_tlsf_src)

add_library(espressif_tlsf STATIC ${espressif_tlsf_src_SOURCE_DIR}/tlsf.c)
target_include_directories(espressif_tlsf PUBLIC
  ${espressif_tlsf_src_SOURCE_DIR}
  ${espressif_tlsf_src_SOURCE_DIR}/include
)

set(SIMULATOR_HEAP_TEST_SOURCES
  SimulatorHeapTest.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/SimulatorHeap.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/SimulatorHeapViz.cpp
)
if(NOT APPLE)
  list(APPEND SIMULATOR_HEAP_TEST_SOURCES SimulatorHeapTestShim.cpp)
endif()

add_executable(simulator_heap_test ${SIMULATOR_HEAP_TEST_SOURCES})
target_compile_definitions(simulator_heap_test PRIVATE
  SIMULATOR
  CROSSPOINT_SIM_HEAP_ARENA
  SIMULATOR_HEAP_TESTING
  SIMULATOR_HEAP_TESTING_NO_GLOBAL_OVERRIDES
)
target_include_directories(simulator_heap_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_options(simulator_heap_test PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(simulator_heap_test PRIVATE GTest::gtest_main espressif_tlsf)

enable_testing()
include(GoogleTest)
gtest_discover_tests(simulator_heap_test)
