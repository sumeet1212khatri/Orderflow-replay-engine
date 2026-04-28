cmake_minimum_required(VERSION 3.16)
project(hft_simulator CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -DNDEBUG")
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

include_directories(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/src
)

set(SOURCES
  src/core/order_book.cpp
  src/core/simulator.cpp
  src/risk/risk_manager.cpp
  src/journal/journal.cpp
  src/main.cpp
)

add_executable(hft_server ${SOURCES})
target_compile_options(hft_server PRIVATE -Wall -Wextra -pthread)
target_link_libraries(hft_server PRIVATE pthread)

add_executable(hft_engine
  src/core/order_book.cpp
  src/core/simulator.cpp
  src/risk/risk_manager.cpp
  src/journal/journal.cpp
  src/engine_cli.cpp
)
target_compile_options(hft_engine PRIVATE -pthread)
target_link_libraries(hft_engine PRIVATE pthread)

install(TARGETS hft_engine DESTINATION bin)