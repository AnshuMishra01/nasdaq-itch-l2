cmake_minimum_required(VERSION 3.28)
project(itch_feedhandler LANGUAGES CXX)

# language standards

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)


# for build we will use default as release
# Link the C++ runtime into every executable. Without this, Windows loads whichever
# libstdc++-6.dll comes first on PATH; Git for Windows ships an older, incompatible one
# (mingw64/bin), and a UCRT64 binary that picks it up segfaults or silently misbehaves.
if(MINGW)
    add_link_options(-static)
endif()

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
    # once per input mode: all three must build the identical book
    foreach(io load mmap chunked)
        add_test(NAME sample_verify_${io}
                 COMMAND itch_count "${ITCH_SAMPLE}" 0 - verify ${ITCH_SAMPLE_HASH} --io=${io})
    endforeach()
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

# input modes: load / mmap / chunked deliver identical messages
add_executable(io_tests tests/io_tests.cpp)
target_include_directories(io_tests PRIVATE src)
target_compile_options(io_tests PRIVATE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow
)
add_test(NAME io_tests COMMAND io_tests)

# full-day scale experiments: I/O split, store sweep, book shape (writes results/scale/)
add_executable(scale_bench src/tools/scale_bench.cpp)
target_include_directories(scale_bench PRIVATE src)
target_compile_options(scale_bench PRIVATE
    -Wall -Wextra -Wpedantic
    -Wconversion -Wsign-conversion
    -Wshadow
)

# which messages remove matched orders at the 09:30 opening cross (results/fullday/auction.md)
add_executable(open_cross src/tools/open_cross.cpp)
target_include_directories(open_cross PRIVATE src)
