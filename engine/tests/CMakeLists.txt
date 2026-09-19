# Host-free engine tests. These exercise the engine library directly and must
# not depend on fcitx5 or any other host toolkit.
if(NOT LLAVON_IME_TABLE_SOURCE_DIR)
    set(LLAVON_IME_TABLE_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/../../ime-unix-service/ime-core/table")
endif()

set(LLAVON_IME_ENGINE_TEST_SOURCES
    test_main.cpp
    config_tests.cpp
    bopomofo_tests.cpp
    buffer_tests.cpp
    protocol_tests.cpp
    service_transport_tests.cpp
    keypad_tests.cpp
    input_state_tests.cpp
    input_key_tests.cpp
    punctuation_tests.cpp
    candidate_view_tests.cpp
    input_processor_tests.cpp
    prediction_state_tests.cpp
    symbol_menu_tests.cpp
    ascii_tokenizer_tests.cpp
    caret_prefix_sampler_tests.cpp
    sample_adoption_tests.cpp
    accessibility_context_tests.cpp
    real_service_tests.cpp
    fallback_engine_tests.cpp
    mixed_input_decoder_tests.cpp
    phrase_override_store_tests.cpp
    host_engine_tests.cpp
    host_prediction_tests.cpp
    c_api_tests.c
)

enable_testing()

add_executable(llavon_ime_tests ${LLAVON_IME_ENGINE_TEST_SOURCES})
target_link_libraries(llavon_ime_tests PRIVATE llavon_ime_engine)
target_compile_definitions(llavon_ime_tests PRIVATE
    LLAVON_IME_TEST_TABLE_PATH="${LLAVON_IME_TABLE_SOURCE_DIR}/bopomofo_char.json")
add_test(NAME llavon_ime_tests COMMAND llavon_ime_tests)
set_tests_properties(llavon_ime_tests PROPERTIES
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../..")
