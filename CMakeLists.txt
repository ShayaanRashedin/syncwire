cmake_minimum_required(VERSION 3.20)

project(
    SyncWire
    VERSION 0.1.0
    DESCRIPTION "Concurrent TCP file synchronization service"
    LANGUAGES CXX
)

option(SYNCWIRE_BUILD_TESTS "Build SyncWire tests" ON)

add_library(syncwire_common
    src/common/codec.cpp
    src/common/frame_parser.cpp
)

add_library(SyncWire::common ALIAS syncwire_common)

target_compile_features(syncwire_common PUBLIC cxx_std_20)
target_include_directories(syncwire_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

if(MSVC)
    target_compile_options(syncwire_common PRIVATE /W4 /permissive-)
else()
    target_compile_options(syncwire_common PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wformat=2
    )
endif()

if(SYNCWIRE_BUILD_TESTS)
    include(CTest)
    enable_testing()

    add_executable(syncwire_unit_tests
        tests/unit/test_main.cpp
        tests/unit/protocol_tests.cpp
        tests/unit/frame_parser_tests.cpp
    )
    target_link_libraries(syncwire_unit_tests PRIVATE SyncWire::common)
    target_compile_features(syncwire_unit_tests PRIVATE cxx_std_20)
    target_include_directories(syncwire_unit_tests PRIVATE tests/unit)

    if(MSVC)
        target_compile_options(syncwire_unit_tests PRIVATE /W4 /permissive-)
    else()
        target_compile_options(syncwire_unit_tests PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wsign-conversion
            -Wshadow
            -Wformat=2
        )
    endif()

    add_test(NAME syncwire_unit_tests COMMAND syncwire_unit_tests)
endif()

