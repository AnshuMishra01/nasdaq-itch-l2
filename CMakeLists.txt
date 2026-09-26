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