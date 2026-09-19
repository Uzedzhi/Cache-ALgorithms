cmake_minimum_required(VERSION 3.14)    # минимальная необходимая версия (нужна для gtest_discover_tests)
project("cache_sim")                    # название проекта

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

SET(SRCS                                # файлы для компиляции
    src/CacheTypes.cpp
    src/InputReader.cpp)

add_library(lclib STATIC ${SRCS})
target_include_directories(lclib PUBLIC "include")

add_executable(cache_sim src/main.cpp)
target_link_libraries(cache_sim lclib)

# тесты: build/maintest или ctest --test-dir build
enable_testing()
find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(maintest Tests/maintest.cpp)
target_link_libraries(maintest lclib GTest::gtest_main)
gtest_discover_tests(maintest)