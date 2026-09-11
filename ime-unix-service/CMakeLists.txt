cmake_minimum_required(VERSION 3.24)

project(ime_unix_service VERSION 0.2.0 LANGUAGES CXX)

include(GNUInstallDirs)

if(WIN32)
    message(FATAL_ERROR "ime-unix-service requires a Unix platform")
endif()

option(IME_UNIX_SERVICE_BUILD_TESTS "Build standalone service protocol/session tests" OFF)

set(IME_UNIX_SERVICE_TARGET llavon-ime-unix-service)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ime-core/CMakeLists.txt")
    message(FATAL_ERROR "ime-core submodule is missing; run git submodule update --init --recursive")
endif()

add_subdirectory(ime-core EXCLUDE_FROM_ALL)
find_package(Threads REQUIRED)

add_executable(${IME_UNIX_SERVICE_TARGET}
    src/main.cpp
    src/platform/server_strategy.cpp
    src/platform/unix_socket_server.cpp
    src/pipe/protocol.cpp
    src/engine/core_runtime.cpp
    src/engine/session_engine.cpp
    src/engine/stderr_logger.cpp
    src/session/session_manager.cpp
)
target_link_libraries(${IME_UNIX_SERVICE_TARGET} PRIVATE ime-core::ime-core Threads::Threads)

target_compile_features(${IME_UNIX_SERVICE_TARGET} PRIVATE cxx_std_23)
set_target_properties(${IME_UNIX_SERVICE_TARGET} PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(${IME_UNIX_SERVICE_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

target_compile_options(${IME_UNIX_SERVICE_TARGET} PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS ${IME_UNIX_SERVICE_TARGET} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/ime-core/table/"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/llavon-ime/tables")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/ime-unix-service")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/ime-core/LICENSE"
    DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/ime-unix-service"
    RENAME ime-core-LICENSE.txt)

if(IME_UNIX_SERVICE_BUILD_TESTS)
    enable_testing()
    add_executable(ime_unix_service_tests
        tests/protocol_tests.cpp
        src/pipe/protocol.cpp
        src/engine/core_runtime.cpp
        src/engine/session_engine.cpp
        src/engine/stderr_logger.cpp
        src/session/session_manager.cpp
    )
    target_compile_features(ime_unix_service_tests PRIVATE cxx_std_23)
    target_include_directories(ime_unix_service_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(ime_unix_service_tests PRIVATE
        IME_UNIX_SERVICE_TEST_TABLE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/ime-core/table")
    target_link_libraries(ime_unix_service_tests PRIVATE ime-core::ime-core Threads::Threads)
    add_test(NAME ime_unix_service_tests COMMAND ime_unix_service_tests)
endif()
