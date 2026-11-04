cmake_minimum_required(VERSION 3.16)
project(irrigation_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
set(IRRIGATION_CORE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../esphome/irrigation-controller/components/irrigation_core)
# The pure logic sits flat in the component directory (ESPHome only compiles
# top-level sources of an external component). The ESPHome glue is excluded.
file(GLOB LOGIC_SOURCES CONFIGURE_DEPENDS ${IRRIGATION_CORE_DIR}/*.cpp)
list(FILTER LOGIC_SOURCES EXCLUDE REGEX "/irrigation_core\.cpp$")

add_executable(host_tests ${TEST_SOURCES} ${LOGIC_SOURCES})
target_include_directories(host_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${IRRIGATION_CORE_DIR})
target_compile_options(host_tests PRIVATE -Wall -Wextra)
add_test(NAME host_tests COMMAND host_tests)
