cmake_minimum_required(VERSION 3.28)
project(itch_feedhandler LANGUAGES CXX)

# language standards

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# for build we will use default as release
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()


# the program
add_executable(itch_count src/main.cpp)
target_include_directories(itch_count PRIVATE src)

target_compile_options(itch_count PRIVATE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow
)

# Debug builds: libstdc++ checks bounds on operator[] etc. and aborts on misuse
target_compile_definitions(itch_count PRIVATE
    $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
)

# std::print on MinGW lives in this extra library
target_link_libraries(itch_count PRIVATE stdc++exp)

# tool: message counts by type per time bucket (streams; works on full-day files)
add_executable(itch_window src/tools/msg_window.cpp)
target_compile_options(itch_window PRIVATE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow
)


# ---- M7 verification ----
enable_testing()

# layer 3: decoder + book unit tests, run on every build via ctest
add_executable(decode_tests tests/decode_tests.cpp)
target_include_directories(decode_tests PRIVATE src)
target_compile_options(decode_tests PRIVATE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow
)
add_test(NAME decode_tests COMMAND decode_tests)

# layers 1 + 4: store-vs-book cross-check and state hash on the sample file.
# The data is not in git, so this test only exists where the file does.
# If a change is *meant* to alter the final state, update ITCH_SAMPLE_HASH and say why.
set(ITCH_SAMPLE "${CMAKE_SOURCE_DIR}/data/12302019.NASDAQ_ITCH50.itch50.sample")
set(ITCH_SAMPLE_HASH "96fd73902096c354")
if(EXISTS "${ITCH_SAMPLE}")
    add_test(NAME sample_verify
             COMMAND itch_count "${ITCH_SAMPLE}" 0 - verify ${ITCH_SAMPLE_HASH})
endif()

# M9: order-store experiment (writes results/m9/)
add_executable(store_bench src/tools/store_bench.cpp)
target_include_directories(store_bench PRIVATE src)
target_compile_options(store_bench PRIVATE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow
)

# M9: every store variant against std::unordered_map
add_executable(store_tests tests/store_tests.cpp)
target_include_directories(store_tests PRIVATE src)
target_compile_options(store_tests PRIVATE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow
)
add_test(NAME store_tests COMMAND store_tests)

# M8: per-message latency percentiles (writes results/m8/)
add_executable(latency_bench src/tools/latency_bench.cpp)
target_include_directories(latency_bench PRIVATE src)
target_compile_options(latency_bench PRIVATE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow
)
