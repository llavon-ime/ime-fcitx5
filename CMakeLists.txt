cmake_minimum_required(VERSION 3.24)

project(ime_core VERSION 0.1.0 LANGUAGES CXX)

include(CMakePackageConfigHelpers)
include(GNUInstallDirs)

option(IME_CORE_BUILD_TESTS "Build ime-core tests" OFF)

find_package(llama CONFIG REQUIRED)
find_package(reflectcpp CONFIG REQUIRED)
find_package(utf8cpp CONFIG REQUIRED)

set(IME_CORE_UTF8_TARGET "")
if(TARGET utf8cpp::utf8cpp)
    set(IME_CORE_UTF8_TARGET utf8cpp::utf8cpp)
elseif(TARGET utf8::cpp)
    set(IME_CORE_UTF8_TARGET utf8::cpp)
else()
    message(FATAL_ERROR "ime-core requires an utf8cpp CMake target")
endif()

add_library(ime-core STATIC
    src/core.cpp
    include/ime-core/core.hpp
    include/ime-core/logger.hpp
    src/bopomofo/bopomofo.hpp
    src/engine/engine.hpp
    src/engine/llama_engine.hpp
    src/engine/tokenizer.hpp
    src/utils/core_paths.hpp
)
add_library(ime-core::ime-core ALIAS ime-core)

target_compile_features(ime-core PUBLIC cxx_std_23)
set_target_properties(ime-core PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
    EXPORT_NAME ime-core
)

target_include_directories(ime-core
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
        "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(ime-core
    PRIVATE
        llama
        reflectcpp::reflectcpp
        ${IME_CORE_UTF8_TARGET}
)

if(MSVC)
    target_compile_options(ime-core PRIVATE /W4 /WX /utf-8)
else()
    target_compile_options(ime-core PRIVATE -Wall -Wextra -Wpedantic)
endif()

install(TARGETS ime-core
    EXPORT ime-core-targets
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(DIRECTORY include/ DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(DIRECTORY table/ DESTINATION "${CMAKE_INSTALL_DATADIR}/ime-core/tables")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/ime-core")

configure_package_config_file(
    cmake/ime-core-config.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/ime-core-config.cmake"
    INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ime-core"
)
write_basic_package_version_file(
    "${CMAKE_CURRENT_BINARY_DIR}/ime-core-config-version.cmake"
    VERSION "${PROJECT_VERSION}"
    COMPATIBILITY SameMajorVersion
)

install(EXPORT ime-core-targets
    FILE ime-core-targets.cmake
    NAMESPACE ime-core::
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ime-core"
)
install(FILES
    "${CMAKE_CURRENT_BINARY_DIR}/ime-core-config.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/ime-core-config-version.cmake"
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/ime-core"
)

if(IME_CORE_BUILD_TESTS)
    enable_testing()
    add_executable(ime-core-api-tests tests/core_api_tests.cpp)
    target_compile_features(ime-core-api-tests PRIVATE cxx_std_23)
    target_link_libraries(ime-core-api-tests PRIVATE ime-core::ime-core)
    add_test(NAME ime-core-api-tests COMMAND ime-core-api-tests)
endif()
