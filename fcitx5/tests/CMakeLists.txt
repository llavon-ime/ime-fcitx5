set(IME_FCITX5_TEST_SOURCES
    test_main.cpp
    config_tests.cpp
    bopomofo_tests.cpp
    buffer_tests.cpp
    protocol_tests.cpp
    service_transport_tests.cpp
    keypad_tests.cpp
)

add_executable(llavon_ime_tests ${IME_FCITX5_TEST_SOURCES})
target_link_libraries(llavon_ime_tests PRIVATE ime_fcitx5_shared)
add_test(NAME llavon_ime_tests COMMAND llavon_ime_tests)
set_tests_properties(llavon_ime_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/..")

if(Fcitx5_FOUND AND NOT WIN32)
    add_test(NAME llavon_ime_fcitx5_metadata
        COMMAND ${CMAKE_COMMAND}
            -DADDON_CONF=${CMAKE_BINARY_DIR}/fcitx5/addon/llavon-ime.conf
            -DINPUTMETHOD_CONF=${CMAKE_BINARY_DIR}/fcitx5/inputmethod/llavon-ime.conf
            -P ${CMAKE_CURRENT_LIST_DIR}/check_fcitx5_metadata.cmake)
endif()
