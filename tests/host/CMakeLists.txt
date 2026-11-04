cmake_minimum_required(VERSION 3.16)
project(irrigation_host_tests CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
enable_testing()

file(GLOB TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
file(GLOB LOGIC_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/../../logic/*.cpp)

add_executable(host_tests ${TEST_SOURCES} ${LOGIC_SOURCES})
target_include_directories(host_tests PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../logic)
target_compile_options(host_tests PRIVATE -Wall -Wextra)
add_test(NAME host_tests COMMAND host_tests)
