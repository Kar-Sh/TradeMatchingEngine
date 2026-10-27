cmake_minimum_required(VERSION 3.15)
project(TradeMatchingEngine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)
find_package(Threads REQUIRED)

# Core logic as a library: used by the application and by the tests
add_library(engine_core functions.cpp)
target_include_directories(engine_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(engine_core PUBLIC nlohmann_json::nlohmann_json spdlog::spdlog Threads::Threads)
target_compile_options(engine_core PRIVATE -Wall -Wextra) 

add_executable(matching_engine main.cpp)
target_link_libraries(matching_engine PRIVATE engine_core) 

option(BUILD_TESTING "Build unit tests" ON)
if(BUILD_TESTING)
    enable_testing()
    find_package(GTest REQUIRED)
    add_executable(engine_tests tests/engine_tests.cpp)
    target_link_libraries(engine_tests PRIVATE engine_core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(engine_tests)
endif()
