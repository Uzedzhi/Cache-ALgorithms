cmake_minimum_required(VERSION 3.11)    # минимальная необходимая версия
project("cache_sim")                    # название проекта

SET(SRCS                                # файлы для компиляции
    src/CacheTypes.cpp
    src/InputReader.cpp)

add_library(lclib STATIC ${SRCS})
target_include_directories(lclib PUBLIC "include")

add_executable(cache_sim src/main.cpp)
target_link_libraries(cache_sim lclib)

SET(TESTS ${CMAKE_SOURCE_DIR}/Tests)
file(GLOB testfiles "Tests/*.dat")
foreach(file ${testfiles})
    add_test(NAME ${file}
    COMMAND bash -c "${CMAKE_SOURCE_DIR}/runtest.sh ${file} ./cache_sim"
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
    set_tests_properties(${file} PROPERTIES DEPENDS cache_sim)
endforeach()