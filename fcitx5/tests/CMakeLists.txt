if(Fcitx5_FOUND AND NOT WIN32)
    add_test(NAME llavon_ime_fcitx5_metadata
        COMMAND ${CMAKE_COMMAND}
            -DADDON_CONF=${CMAKE_BINARY_DIR}/fcitx5/addon/llavon-ime.conf
            -DINPUTMETHOD_CONF=${CMAKE_BINARY_DIR}/fcitx5/inputmethod/llavon-ime.conf
            -P ${CMAKE_CURRENT_LIST_DIR}/check_fcitx5_metadata.cmake)

    add_executable(llavon_ime_prediction_coordinator_tests
        prediction_coordinator_tests.cpp
        ../fcitx5/prediction_coordinator.cpp)
    target_link_libraries(llavon_ime_prediction_coordinator_tests PRIVATE
        Fcitx5::Core
        llavon_ime_engine)
    target_include_directories(llavon_ime_prediction_coordinator_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(llavon_ime_prediction_coordinator_tests PRIVATE
        IME_FCITX5_TEST_TABLE_PATH="${IME_FCITX5_TABLE_SOURCE_DIR}/bopomofo_char.json")
    add_test(NAME llavon_ime_prediction_coordinator_tests
        COMMAND llavon_ime_prediction_coordinator_tests)

    add_executable(llavon_ime_prediction_coordinator_integration_tests
        prediction_coordinator_integration_tests.cpp
        ../fcitx5/prediction_coordinator.cpp)
    target_link_libraries(llavon_ime_prediction_coordinator_integration_tests PRIVATE
        Fcitx5::Core
        llavon_ime_engine)
    target_include_directories(llavon_ime_prediction_coordinator_integration_tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_compile_definitions(llavon_ime_prediction_coordinator_integration_tests PRIVATE
        IME_FCITX5_TEST_TABLE_PATH="${IME_FCITX5_TABLE_SOURCE_DIR}/bopomofo_char.json")
    add_test(NAME llavon_ime_prediction_coordinator_integration_tests
        COMMAND llavon_ime_prediction_coordinator_integration_tests)

    find_package(Fcitx5Module QUIET COMPONENTS TestFrontend)
    set(IME_FCITX5_LOCAL_TESTFRONTEND OFF)
    if(APPLE AND NOT TARGET Fcitx5::Module::TestFrontend)
        set(_ime_fcitx5_testfrontend_source "")
        if(EXISTS "${FCITX5_MACOS_SOURCE_DIR}/fcitx5/testing/testfrontend/testfrontend.cpp")
            set(_ime_fcitx5_testfrontend_source
                "${FCITX5_MACOS_SOURCE_DIR}/fcitx5/testing/testfrontend")
        elseif(EXISTS "${FCITX5_MACOS_SOURCE_DIR}/testing/testfrontend/testfrontend.cpp")
            set(_ime_fcitx5_testfrontend_source
                "${FCITX5_MACOS_SOURCE_DIR}/testing/testfrontend")
        endif()

        if(_ime_fcitx5_testfrontend_source)
            add_library(testfrontend MODULE
                "${_ime_fcitx5_testfrontend_source}/testfrontend.cpp")
            target_include_directories(testfrontend PRIVATE
                "${_ime_fcitx5_testfrontend_source}")
            target_link_libraries(testfrontend PRIVATE Fcitx5::Core)
            set_target_properties(testfrontend PROPERTIES
                LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

            add_library(Fcitx5::Module::TestFrontend INTERFACE IMPORTED GLOBAL)
            set_target_properties(Fcitx5::Module::TestFrontend PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${_ime_fcitx5_testfrontend_source}")
            set(IME_FCITX5_LOCAL_TESTFRONTEND ON)
        endif()
    endif()

    if(TARGET Fcitx5::Module::TestFrontend)
        add_subdirectory(test/addon)
        add_subdirectory(test/inputmethod)
        if(IME_FCITX5_LOCAL_TESTFRONTEND)
            configure_file(
                "${_ime_fcitx5_testfrontend_source}/testfrontend.conf"
                "${CMAKE_CURRENT_BINARY_DIR}/test/addon/testfrontend.conf"
                @ONLY)
        endif()
        configure_file(testdir.h.in ${CMAKE_CURRENT_BINARY_DIR}/testdir.h @ONLY)

        set(IME_FCITX5_ENGINE_TEST_SOURCES
            engine_test_main.cpp
            engine_basic_input_tests.cpp
            engine_hsu_layout_tests.cpp
            engine_symbol_menu_tests.cpp
            engine_candidate_navigation_tests.cpp
            engine_punctuation_tests.cpp
            engine_shift_letter_tests.cpp
            engine_context_source_tests.cpp
            engine_async_state_tests.cpp
            engine_lifecycle_tests.cpp
            engine_smart_candidate_tests.cpp
            engine_smart_chat_tests.cpp
            engine_smart_config_tests.cpp
            engine_smart_decoder_tests.cpp
            engine_smart_edge_tests.cpp
            engine_smart_hsu_tests.cpp
            engine_smart_long_tests.cpp
            engine_smart_mixed_tests.cpp
            engine_smart_regression_tests.cpp
            engine_smart_space_tests.cpp
            engine_smart_standard_tests.cpp
            engine_smart_usability_tests.cpp
            engine_smart_work_tests.cpp
            engine_phrase_override_tests.cpp
        )

        add_executable(llavon_ime_engine_tests ${IME_FCITX5_ENGINE_TEST_SOURCES})
        target_link_libraries(llavon_ime_engine_tests PRIVATE
            Fcitx5::Core
            Fcitx5::Module::TestFrontend
            llavon_ime_engine)
        target_include_directories(llavon_ime_engine_tests PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_dependencies(llavon_ime_engine_tests llavon-ime-addon)
        if(IME_FCITX5_LOCAL_TESTFRONTEND)
            add_dependencies(llavon_ime_engine_tests testfrontend)
        endif()
        add_test(NAME llavon_ime_engine_tests COMMAND llavon_ime_engine_tests)
        set_tests_properties(llavon_ime_engine_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
            ENVIRONMENT "IME_FCITX5_TABLE_PATH=${IME_FCITX5_TABLE_SOURCE_DIR}/bopomofo_char.json"
            SKIP_RETURN_CODE 77)

        add_executable(llavon_ime_engine_prediction_integration_tests
            engine_prediction_integration_tests.cpp)
        target_link_libraries(llavon_ime_engine_prediction_integration_tests PRIVATE
            Fcitx5::Core
            Fcitx5::Module::TestFrontend
            llavon_ime_engine)
        target_include_directories(llavon_ime_engine_prediction_integration_tests PRIVATE
            ${CMAKE_CURRENT_BINARY_DIR}
            ${CMAKE_CURRENT_SOURCE_DIR}/..)
        add_dependencies(llavon_ime_engine_prediction_integration_tests llavon-ime-addon)
        if(IME_FCITX5_LOCAL_TESTFRONTEND)
            add_dependencies(llavon_ime_engine_prediction_integration_tests testfrontend)
        endif()
        add_test(NAME llavon_ime_engine_prediction_integration_tests
            COMMAND llavon_ime_engine_prediction_integration_tests)
        set_tests_properties(llavon_ime_engine_prediction_integration_tests PROPERTIES
            WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/.."
            ENVIRONMENT "IME_FCITX5_TABLE_PATH=${IME_FCITX5_TABLE_SOURCE_DIR}/bopomofo_char.json"
            SKIP_RETURN_CODE 77)
    endif()
endif()
