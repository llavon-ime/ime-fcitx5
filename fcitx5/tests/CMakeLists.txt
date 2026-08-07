set(IME_FCITX5_TEST_SOURCES
    test_main.cpp
    config_tests.cpp
    bopomofo_tests.cpp
    buffer_tests.cpp
    protocol_tests.cpp
    service_transport_tests.cpp
    keypad_tests.cpp
    input_state_tests.cpp
    symbol_menu_tests.cpp
)

add_executable(llavon_ime_tests ${IME_FCITX5_TEST_SOURCES})
target_link_libraries(llavon_ime_tests PRIVATE ime_fcitx5_shared)
target_compile_definitions(llavon_ime_tests PRIVATE
    IME_FCITX5_TEST_TABLE_PATH="${IME_FCITX5_TABLE_SOURCE_DIR}/bopomofo_char.json")
add_test(NAME llavon_ime_tests COMMAND llavon_ime_tests)
set_tests_properties(llavon_ime_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/..")

if(Fcitx5_FOUND AND NOT WIN32)
    add_test(NAME llavon_ime_fcitx5_metadata
        COMMAND ${CMAKE_COMMAND}
            -DADDON_CONF=${CMAKE_BINARY_DIR}/fcitx5/addon/llavon-ime.conf
            -DINPUTMETHOD_CONF=${CMAKE_BINARY_DIR}/fcitx5/inputmethod/llavon-ime.conf
            -P ${CMAKE_CURRENT_LIST_DIR}/check_fcitx5_metadata.cmake)

    find_package(Fcitx5Module QUIET COMPONENTS TestFrontend)
    if(Fcitx5ModuleTestFrontend_FOUND)
        add_subdirectory(test/addon)
        add_subdirectory(test/inputmethod)
        configure_file(testdir.h.in ${CMAKE_CURRENT_BINARY_DIR}/testdir.h @ONLY)

        set(IME_FCITX5_ENGINE_TEST_SOURCES
            engine_test_main.cpp
            engine_basic_input_tests.cpp
            engine_hsu_layout_tests.cpp
            engine_symbol_menu_tests.cpp
            engine_candidate_navigation_tests.cpp
            engine_punctuation_tests.cpp
            engine_shift_letter_tests.cpp
        )

        add_executable(llavon_ime_engine_tests ${IME_FCITX5_ENGINE_TEST_SOURCES})
        target_link_libraries(llavon_ime_engine_tests PRIVATE
            Fcitx5::Core
            Fcitx5::Module::TestFrontend
            ime_fcitx5_shared)
        target_include_directories(llavon_ime_engine_tests PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR})
        add_dependencies(llavon_ime_engine_tests llavon-ime-addon)
        add_test(NAME llavon_ime_engine_tests COMMAND llavon_ime_engine_tests)
        set_tests_properties(llavon_ime_engine_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
            ENVIRONMENT "IME_FCITX5_TABLE_PATH=${IME_FCITX5_TABLE_SOURCE_DIR}/bopomofo_char.json")
    endif()
endif()
