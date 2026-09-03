cmake_minimum_required(VERSION 3.20)

project(
    SyncWire
    VERSION 0.2.0
    DESCRIPTION "Concurrent TCP file synchronization service"
    LANGUAGES CXX
)

option(SYNCWIRE_BUILD_TESTS "Build SyncWire tests" ON)
option(SYNCWIRE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "SyncWire requires Linux (epoll and POSIX APIs). Use a Linux VM or WSL.")
endif()
find_package(Threads REQUIRED)
find_package(OpenSSL 3.0 REQUIRED)

add_library(syncwire_common
    src/common/authentication.cpp
    src/common/codec.cpp
    src/common/crc32.cpp
    src/common/directory_manifest.cpp
    src/common/directory_sync.cpp
    src/common/file_transfer.cpp
    src/common/frame_io.cpp
    src/common/frame_parser.cpp
    src/common/ping_pong.cpp
    src/common/socket_io.cpp
    src/common/tcp_socket.cpp
    src/common/sync_codec.cpp
    src/common/transfer_codec.cpp
    src/server/client_session.cpp
    src/server/concurrent_server.cpp
)

add_library(SyncWire::common ALIAS syncwire_common)

target_compile_features(syncwire_common PUBLIC cxx_std_20)
target_link_libraries(syncwire_common PUBLIC Threads::Threads OpenSSL::Crypto)
if(SYNCWIRE_SANITIZERS)
    target_compile_options(syncwire_common PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(syncwire_common PUBLIC -fsanitize=address,undefined)
endif()
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

add_executable(syncwire-server apps/syncwire-server/main.cpp)
target_link_libraries(syncwire-server PRIVATE SyncWire::common)
target_compile_features(syncwire-server PRIVATE cxx_std_20)

add_executable(syncwire-client apps/syncwire-client/main.cpp)
target_link_libraries(syncwire-client PRIVATE SyncWire::common)
target_compile_features(syncwire-client PRIVATE cxx_std_20)

if(MSVC)
    target_compile_options(syncwire-server PRIVATE /W4 /permissive-)
    target_compile_options(syncwire-client PRIVATE /W4 /permissive-)
else()
    target_compile_options(syncwire-server PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2
    )
    target_compile_options(syncwire-client PRIVATE
        -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Wformat=2
    )
endif()

if(SYNCWIRE_BUILD_TESTS)
    include(CTest)
    enable_testing()

    add_executable(syncwire_unit_tests
        tests/unit/test_main.cpp
        tests/unit/authentication_tests.cpp
        tests/unit/protocol_tests.cpp
        tests/unit/frame_parser_tests.cpp
        tests/unit/unique_fd_tests.cpp
        tests/unit/socket_io_tests.cpp
        tests/unit/ping_pong_tests.cpp
        tests/unit/transfer_codec_tests.cpp
        tests/unit/file_transfer_tests.cpp
        tests/unit/resume_tests.cpp
        tests/unit/directory_sync_tests.cpp
        tests/unit/concurrent_server_tests.cpp
    )
    target_link_libraries(syncwire_unit_tests PRIVATE SyncWire::common Threads::Threads)
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
    set_tests_properties(syncwire_unit_tests PROPERTIES TIMEOUT 60)
endif()

include(GNUInstallDirs)
install(TARGETS syncwire-client syncwire-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
