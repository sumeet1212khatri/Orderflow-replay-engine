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

set(CORE_SOURCES
  src/core/order_book.cpp
  src/core/simulator.cpp
  src/risk/risk_manager.cpp
  src/journal/journal.cpp
)

# ── Production targets ────────────────────────────────────────────────────────

add_executable(hft_server ${CORE_SOURCES} src/main.cpp)
target_compile_options(hft_server PRIVATE -Wall -Wextra -pthread)
target_link_libraries(hft_server PRIVATE pthread)

add_executable(hft_engine ${CORE_SOURCES} src/engine_cli.cpp)
target_compile_options(hft_engine PRIVATE -Wall -Wextra -pthread)
target_link_libraries(hft_engine PRIVATE pthread)

install(TARGETS hft_engine DESTINATION bin)

# ── Test targets ──────────────────────────────────────────────────────────────

add_executable(hft_test_orderbook
  ${CORE_SOURCES}
  tests/test_order_book.cpp
)
target_compile_options(hft_test_orderbook PRIVATE -Wall -Wextra -pthread)
target_link_libraries(hft_test_orderbook PRIVATE pthread)

add_executable(hft_test_risk
  ${CORE_SOURCES}
  tests/test_risk_manager.cpp
)
target_compile_options(hft_test_risk PRIVATE -Wall -Wextra -pthread)
target_link_libraries(hft_test_risk PRIVATE pthread)

# ── Benchmark target ──────────────────────────────────────────────────────────

add_executable(hft_bench
  ${CORE_SOURCES}
  benchmark/bench_matching_engine.cpp
)
target_compile_options(hft_bench PRIVATE -Wall -Wextra -pthread -O3)
target_link_libraries(hft_bench PRIVATE pthread)

# ── CTest integration ────────────────────────────────────────────────────────

enable_testing()
add_test(NAME OrderBookTests COMMAND hft_test_orderbook)
add_test(NAME RiskManagerTests COMMAND hft_test_risk)