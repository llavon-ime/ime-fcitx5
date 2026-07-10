cmake_minimum_required(VERSION 3.20)
project(llavon-ime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(GNUInstallDirs)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})

option(IME_FCITX5_BUILD_TESTS "Build fcitx5 tests" ON)
set(IME_FCITX5_SERVICE_NAME "llavon-ime-service" CACHE STRING "Installed fcitx5 service executable name")
set(IME_FCITX5_FCITX_PLUGIN_DIR "plugin" CACHE STRING "macOS fcitx5-macos plugin metadata install directory")
set(IME_FCITX5_INSTALLED_MODEL_PATH "" CACHE FILEPATH "Installed default GGUF model path")

set(IME_FCITX5_INSTALLED_SERVICE_PATH "${CMAKE_INSTALL_FULL_BINDIR}/${IME_FCITX5_SERVICE_NAME}")
set(IME_FCITX5_INSTALLED_TABLE_DIR "${CMAKE_INSTALL_FULL_DATADIR}/llavon-ime/tables")
set(IME_FCITX5_INSTALLED_TABLE_PATH "${CMAKE_INSTALL_FULL_DATADIR}/llavon-ime/tables/bopomofo_char.json")

include(LlamaCpp)

function(ime_fcitx5_install_destination output directory)
    if(IS_ABSOLUTE "${directory}")
        file(RELATIVE_PATH relative_directory "${CMAKE_INSTALL_PREFIX}" "${directory}")
        if(NOT relative_directory MATCHES "^\\.\\.")
            set(${output} "${relative_directory}" PARENT_SCOPE)
            return()
        endif()
    endif()

    set(${output} "${directory}" PARENT_SCOPE)
endfunction()

add_library(ime_fcitx5_shared STATIC shared/ime_fcitx5_shared.cpp)
set_target_properties(ime_fcitx5_shared PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ime_fcitx5_shared PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/shared")
target_compile_definitions(ime_fcitx5_shared PRIVATE
    IME_FCITX5_SERVICE_NAME="${IME_FCITX5_SERVICE_NAME}"
    IME_FCITX5_INSTALLED_SERVICE_PATH="${IME_FCITX5_INSTALLED_SERVICE_PATH}"
    IME_FCITX5_INSTALLED_MODEL_PATH="${IME_FCITX5_INSTALLED_MODEL_PATH}"
)
target_compile_options(ime_fcitx5_shared PRIVATE -Wall -Wextra -Wpedantic)

find_package(nlohmann_json CONFIG REQUIRED)
find_package(asio CONFIG REQUIRED)
find_package(Threads REQUIRED)
target_sources(ime_fcitx5_shared PRIVATE shared/config/config.cpp)
target_sources(ime_fcitx5_shared PRIVATE
    shared/text/utf.cpp
    shared/bopomofo/keymap.cpp
    shared/bopomofo/syllable.cpp
    shared/bopomofo/table_engine.cpp
    shared/buffer/composition_buffer.cpp
    shared/engine/fallback_engine.cpp
    shared/input/keypad.cpp
    shared/engine/service_client.cpp
    shared/protocol/protocol.cpp
    shared/protocol/binary_codec.cpp
    shared/ipc/unix_socket.cpp
)
target_link_libraries(ime_fcitx5_shared PUBLIC nlohmann_json::nlohmann_json Threads::Threads)

add_library(ime_fcitx5_service_lib STATIC service/service_app.cpp service/llama_backend.cpp)
set_target_properties(ime_fcitx5_service_lib PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(ime_fcitx5_service_lib PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ime_fcitx5_service_lib PUBLIC ime_fcitx5_shared)
target_compile_definitions(ime_fcitx5_service_lib PRIVATE
    IME_FCITX5_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    IME_FCITX5_INSTALLED_TABLE_DIR="${IME_FCITX5_INSTALLED_TABLE_DIR}"
    IME_FCITX5_INSTALLED_TABLE_PATH="${IME_FCITX5_INSTALLED_TABLE_PATH}"
)
target_compile_options(ime_fcitx5_service_lib PRIVATE -Wall -Wextra -Wpedantic)
ime_fcitx5_configure_llama(ime_fcitx5_service_lib)

add_executable(llavon-ime-service service/service_main.cpp)
target_link_libraries(llavon-ime-service PRIVATE ime_fcitx5_service_lib asio::asio)
set_target_properties(llavon-ime-service PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/service")
install(TARGETS llavon-ime-service RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES "${CMAKE_CURRENT_SOURCE_DIR}/../tables/bopomofo_char.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/llavon-ime/tables")
install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../tables/tokens"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/llavon-ime/tables"
        FILES_MATCHING PATTERN "*.json")

find_package(Fcitx5 QUIET)
if(Fcitx5_FOUND)
    find_package(Fcitx5Utils QUIET CONFIG)
    if(NOT DEFINED FCITX_INSTALL_ADDONDIR)
        set(FCITX_INSTALL_ADDONDIR "${CMAKE_INSTALL_FULL_LIBDIR}/fcitx5")
    endif()
    if(NOT DEFINED FCITX_INSTALL_PKGDATADIR)
        set(FCITX_INSTALL_PKGDATADIR "${CMAKE_INSTALL_FULL_DATADIR}/fcitx5")
    endif()
    ime_fcitx5_install_destination(IME_FCITX5_FCITX_ADDONDIR "${FCITX_INSTALL_ADDONDIR}")
    ime_fcitx5_install_destination(IME_FCITX5_FCITX_PKGDATADIR "${FCITX_INSTALL_PKGDATADIR}")
    if(IS_ABSOLUTE "${IME_FCITX5_FCITX_PKGDATADIR}")
        set(IME_FCITX5_FCITX_PKGDATADIR_INSTALL_PATH "${IME_FCITX5_FCITX_PKGDATADIR}")
    else()
        set(IME_FCITX5_FCITX_PKGDATADIR_INSTALL_PATH "${CMAKE_INSTALL_PREFIX}/${IME_FCITX5_FCITX_PKGDATADIR}")
    endif()
    if(IS_ABSOLUTE "${IME_FCITX5_FCITX_PLUGIN_DIR}")
        set(IME_FCITX5_FCITX_PLUGIN_INSTALL_PATH "${IME_FCITX5_FCITX_PLUGIN_DIR}")
    else()
        set(IME_FCITX5_FCITX_PLUGIN_INSTALL_PATH "${CMAKE_INSTALL_PREFIX}/${IME_FCITX5_FCITX_PLUGIN_DIR}")
    endif()

    add_library(llavon-ime-addon MODULE fcitx5/ime_engine.cpp fcitx5/ime_config.cpp)
    target_include_directories(llavon-ime-addon PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_link_libraries(llavon-ime-addon PRIVATE Fcitx5::Core ime_fcitx5_shared)
    target_compile_definitions(llavon-ime-addon PRIVATE
        IME_FCITX5_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        IME_FCITX5_INSTALLED_TABLE_PATH="${IME_FCITX5_INSTALLED_TABLE_PATH}"
    )
    target_compile_options(llavon-ime-addon PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(llavon-ime-addon PROPERTIES PREFIX "")
    configure_file(fcitx5/addon.conf.in "${CMAKE_BINARY_DIR}/fcitx5/addon/llavon-ime.conf" @ONLY)
    configure_file(fcitx5/inputmethod.conf.in "${CMAKE_BINARY_DIR}/fcitx5/inputmethod/llavon-ime.conf" @ONLY)
    install(TARGETS llavon-ime-addon LIBRARY DESTINATION "${IME_FCITX5_FCITX_ADDONDIR}")
    install(FILES "${CMAKE_BINARY_DIR}/fcitx5/addon/llavon-ime.conf"
            DESTINATION "${IME_FCITX5_FCITX_PKGDATADIR}/addon")
    install(FILES "${CMAKE_BINARY_DIR}/fcitx5/inputmethod/llavon-ime.conf"
            DESTINATION "${IME_FCITX5_FCITX_PKGDATADIR}/inputmethod")
    if(APPLE)
        configure_file(fcitx5/plugin.json.in "${CMAKE_BINARY_DIR}/fcitx5/plugin/llavon-ime.json" @ONLY)
        install(FILES "${CMAKE_BINARY_DIR}/fcitx5/plugin/llavon-ime.json"
                DESTINATION "${IME_FCITX5_FCITX_PLUGIN_DIR}")
    endif()
else()
    message(STATUS "fcitx5 development files not found; skipping llavon-ime-addon")
endif()

if(IME_FCITX5_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
