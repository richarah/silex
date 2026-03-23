cmake_minimum_required(VERSION 3.20)
project(json-parser VERSION 1.0.0 LANGUAGES CXX)

# C++17 for std::string_view, std::optional
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Optimisation flags (release build by default)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG")

# Source files
add_library(json_parser STATIC
    src/json_parser.cpp
    src/json_value.cpp
    src/json_tokenizer.cpp
)

target_include_directories(json_parser PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Example executable
add_executable(json_example
    examples/main.cpp
)

target_link_libraries(json_example PRIVATE json_parser)

# Test executable (simple unit tests)
add_executable(json_test
    tests/test_parser.cpp
)

target_link_libraries(json_test PRIVATE json_parser)

# Installation (not needed for benchmark, but good practice)
install(TARGETS json_parser json_example
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
    RUNTIME DESTINATION bin
)

install(DIRECTORY include/ DESTINATION include)
