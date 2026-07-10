add_executable(llavon_ime_tests test_main.cpp config_tests.cpp bopomofo_tests.cpp buffer_tests.cpp protocol_tests.cpp service_app_tests.cpp llama_backend_tests.cpp service_client_tests.cpp service_socket_tests.cpp keypad_tests.cpp)
target_link_libraries(llavon_ime_tests PRIVATE ime_fcitx5_shared ime_fcitx5_service_lib)
target_compile_definitions(llavon_ime_tests PRIVATE IME_FCITX5_SERVICE_TEST_PATH="$<TARGET_FILE:llavon-ime-service>")
add_dependencies(llavon_ime_tests llavon-ime-service)
add_test(NAME llavon_ime_tests COMMAND llavon_ime_tests)
set_tests_properties(llavon_ime_tests PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/..")

add_test(NAME llavon_ime_service_smoke_predict COMMAND $<TARGET_FILE:llavon-ime-service> --smoke-predict "ㄋㄧˇ")
set_tests_properties(llavon_ime_service_smoke_predict PROPERTIES
    ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/tests/xdg-config-empty;XDG_RUNTIME_DIR=${CMAKE_BINARY_DIR}/tests/xdg-runtime"
    PASS_REGULAR_EXPRESSION "你"
    TIMEOUT 5
)

if(Fcitx5_FOUND)
    add_test(NAME llavon_ime_fcitx5_metadata
        COMMAND ${CMAKE_COMMAND}
            -DADDON_CONF=${CMAKE_BINARY_DIR}/fcitx5/addon/llavon-ime.conf
            -DINPUTMETHOD_CONF=${CMAKE_BINARY_DIR}/fcitx5/inputmethod/llavon-ime.conf
            -P ${CMAKE_CURRENT_LIST_DIR}/check_fcitx5_metadata.cmake)
endif()
