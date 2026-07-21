cmake_minimum_required(VERSION 3.24)

project(ime_service VERSION 0.2.0 LANGUAGES C CXX)

option(IMESVC_BUILD_TESTS "Build standalone service protocol/session tests" OFF)
option(IMESVC_ENABLE_LLAMA "Enable the optional llama.cpp POSIX session engine" ON)
option(IMESVC_REQUIRE_LLAMA "Fail configuration when the llama.cpp session engine is unavailable" OFF)
option(IMESVC_BUILD_TRAINER "Build the optional LibTorch personal-learning trainer" OFF)
set(IMESVC_LIBTORCH_PREFIX "" CACHE PATH "Absolute prefix of the pinned prebuilt LibTorch SDK")
set(IMESVC_LIBTORCH_VERSION "2.7.1" CACHE STRING "Required LibTorch SDK version")
set(IMESVC_LIBTORCH_ARCHIVE "" CACHE FILEPATH "Downloaded LibTorch SDK archive used to create the prefix")
set(IMESVC_DEFAULT_LIBTORCH_SHA256 "")
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)$")
    set(IMESVC_DEFAULT_LIBTORCH_SHA256 "aa89ac85b91c83d0f976f8d135330d51e38ab777b26ec24f312fd58d079314cb")
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    # libtorch-cxx11-abi-shared-with-deps-2.7.1+cpu.zip
    set(IMESVC_DEFAULT_LIBTORCH_SHA256 "63d572598c8d532128a335018913e795c1bbb32602ce378896dc8cfbb5590976")
endif()
set(IMESVC_LIBTORCH_SHA256 "${IMESVC_DEFAULT_LIBTORCH_SHA256}"
    CACHE STRING "Required SHA256 for the platform-specific pinned LibTorch 2.7.1 archive")
set(IMESVC_TEST_BASE_SAFETENSORS "" CACHE FILEPATH "Optional F32 safetensors fixture for full trainer tests")
set(IMESVC_TEST_BASE_SHA256 "" CACHE STRING "SHA256 of IMESVC_TEST_BASE_SAFETENSORS")
set(IMESVC_TEST_BASE_GGUF "" CACHE FILEPATH "Optional F16/F32 GGUF fixture for llama.cpp parity tests")
set(IMESVC_TEST_RUNTIME_GGUF "" CACHE FILEPATH "Optional deployed Q4 GGUF fixture for personalization-effect tests")

set(IMESVC_TARGET llavon-ime-service)
set(IMESVC_COMMON_SOURCES
    src/main.cpp
    src/platform/server_strategy.cpp
)

if(WIN32)
    add_executable(${IMESVC_TARGET}
        ${IMESVC_COMMON_SOURCES}
        src/engine/llama_resources.cpp
        src/platform/windows_named_pipe_server.hpp
    )
else()
    find_package(nlohmann_json CONFIG REQUIRED)
    find_package(unofficial-sqlite3 CONFIG QUIET)
    if(TARGET unofficial::sqlite3::sqlite3)
        set(IMESVC_SQLITE_TARGET unofficial::sqlite3::sqlite3)
    else()
        find_package(SQLite3 REQUIRED)
        set(IMESVC_SQLITE_TARGET SQLite::SQLite3)
    endif()
    add_executable(${IMESVC_TARGET}
        ${IMESVC_COMMON_SOURCES}
        src/platform/unix_socket_server.cpp
        src/pipe/protocol.cpp
        src/engine/model_runtime.cpp
        src/engine/session_engine.cpp
        src/session/session_manager.cpp
        src/training/feedback_store.cpp
        src/training/training_orchestrator.cpp
        src/training/system_environment.cpp
        src/training/adapter_publisher.cpp
        src/training/sha256.cpp
    )
    target_link_libraries(${IMESVC_TARGET} PRIVATE nlohmann_json::nlohmann_json ${IMESVC_SQLITE_TARGET})
    if(APPLE)
        target_link_libraries(${IMESVC_TARGET} PRIVATE "-framework IOKit" "-framework CoreFoundation" "-framework Foundation")
    endif()
    if(IMESVC_ENABLE_LLAMA)
        find_package(llama CONFIG QUIET)
        find_package(reflectcpp CONFIG QUIET)
        find_package(utf8cpp CONFIG QUIET)
        set(IMESVC_UTF8_TARGET "")
        if(TARGET utf8cpp::utf8cpp)
            set(IMESVC_UTF8_TARGET utf8cpp::utf8cpp)
        elseif(TARGET utf8::cpp)
            set(IMESVC_UTF8_TARGET utf8::cpp)
        endif()
        if(TARGET llama AND TARGET reflectcpp::reflectcpp AND IMESVC_UTF8_TARGET)
            target_sources(${IMESVC_TARGET} PRIVATE src/engine/llama_resources.cpp)
            target_compile_definitions(${IMESVC_TARGET} PRIVATE IMESVC_HAS_LLAMA=1)
            target_link_libraries(${IMESVC_TARGET} PRIVATE llama reflectcpp::reflectcpp ${IMESVC_UTF8_TARGET})
            message(STATUS "ime-service: optional llama.cpp session engine enabled")
        else()
            if(IMESVC_REQUIRE_LLAMA)
                message(FATAL_ERROR "ime-service requires llama, reflectcpp::reflectcpp, and utf8cpp")
            endif()
            message(STATUS "ime-service: optional llama.cpp session engine unavailable; using table runtime")
        endif()
    endif()
endif()

if(IMESVC_BUILD_TRAINER)
    if(WIN32)
        message(FATAL_ERROR "The initial native LibTorch trainer supports POSIX CPU builds only")
    endif()
    if(NOT IS_ABSOLUTE "${IMESVC_LIBTORCH_PREFIX}")
        message(FATAL_ERROR "IMESVC_LIBTORCH_PREFIX must be an absolute path to the pinned prebuilt LibTorch SDK")
    endif()
    if(NOT EXISTS "${IMESVC_LIBTORCH_PREFIX}")
        message(FATAL_ERROR "IMESVC_LIBTORCH_PREFIX does not exist: ${IMESVC_LIBTORCH_PREFIX}")
    endif()
    if(NOT IS_ABSOLUTE "${IMESVC_LIBTORCH_ARCHIVE}" OR NOT EXISTS "${IMESVC_LIBTORCH_ARCHIVE}")
        message(FATAL_ERROR "IMESVC_LIBTORCH_ARCHIVE must name the absolute pinned SDK archive")
    endif()
    string(LENGTH "${IMESVC_LIBTORCH_SHA256}" IMESVC_LIBTORCH_SHA256_LENGTH)
    if(NOT IMESVC_LIBTORCH_SHA256_LENGTH EQUAL 64 OR NOT IMESVC_LIBTORCH_SHA256 MATCHES "^[0-9a-f]+$")
        message(FATAL_ERROR "IMESVC_LIBTORCH_SHA256 must be explicitly pinned for this platform")
    endif()
    file(SHA256 "${IMESVC_LIBTORCH_ARCHIVE}" IMESVC_ACTUAL_LIBTORCH_SHA256)
    if(NOT IMESVC_ACTUAL_LIBTORCH_SHA256 STREQUAL IMESVC_LIBTORCH_SHA256)
        message(FATAL_ERROR "LibTorch archive SHA256 ${IMESVC_ACTUAL_LIBTORCH_SHA256} does not match ${IMESVC_LIBTORCH_SHA256}")
    endif()
    list(PREPEND CMAKE_PREFIX_PATH "${IMESVC_LIBTORCH_PREFIX}")
    find_package(Torch CONFIG REQUIRED)
    file(REAL_PATH "${IMESVC_LIBTORCH_PREFIX}" IMESVC_REAL_LIBTORCH_PREFIX)
    file(REAL_PATH "${Torch_DIR}" IMESVC_REAL_TORCH_DIR)
    string(FIND "${IMESVC_REAL_TORCH_DIR}/" "${IMESVC_REAL_LIBTORCH_PREFIX}/" IMESVC_TORCH_PREFIX_POSITION)
    if(NOT IMESVC_TORCH_PREFIX_POSITION EQUAL 0)
        message(FATAL_ERROR "Torch_DIR must resolve inside the verified IMESVC_LIBTORCH_PREFIX")
    endif()
    if(NOT DEFINED Torch_VERSION)
        message(FATAL_ERROR "The LibTorch SDK did not expose Torch_VERSION for pin verification")
    endif()
    if(NOT Torch_VERSION VERSION_EQUAL IMESVC_LIBTORCH_VERSION)
        message(FATAL_ERROR "LibTorch version ${Torch_VERSION} does not match required ${IMESVC_LIBTORCH_VERSION}")
    endif()
    if(NOT TARGET torch)
        message(FATAL_ERROR "The LibTorch SDK did not export the torch target")
    endif()
    find_package(ggml CONFIG REQUIRED)
    find_package(llama CONFIG REQUIRED)
    if(IMESVC_BUILD_TESTS)
        find_package(reflectcpp CONFIG REQUIRED)
        find_package(utf8cpp CONFIG REQUIRED)
        if(NOT IMESVC_UTF8_TARGET)
            if(TARGET utf8cpp::utf8cpp)
                set(IMESVC_UTF8_TARGET utf8cpp::utf8cpp)
            elseif(TARGET utf8::cpp)
                set(IMESVC_UTF8_TARGET utf8::cpp)
            else()
                message(FATAL_ERROR "trainer tokenizer parity tests require utf8cpp")
            endif()
        endif()
    endif()
    add_executable(llavon-ime-trainer
        src/trainer/main.cpp
        src/trainer/trainer.cpp
        src/ml/safetensors_reader.cpp
        src/ml/llama_model.cpp
        src/ml/lora_linear.cpp
        src/ml/training_dataset.cpp
        src/ml/checkpoint.cpp
        src/ml/gguf_lora_writer.cpp
        src/training/sha256.cpp
        src/training/feedback_store.cpp
        src/training/training_orchestrator.cpp
    )
    target_compile_features(llavon-ime-trainer PRIVATE cxx_std_23)
    target_include_directories(llavon-ime-trainer PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(llavon-ime-trainer PRIVATE torch nlohmann_json::nlohmann_json
                                                ${IMESVC_SQLITE_TARGET} ggml::ggml llama)
    if(TORCH_CXX_FLAGS)
        separate_arguments(IMESVC_TORCH_CXX_FLAGS NATIVE_COMMAND "${TORCH_CXX_FLAGS}")
        target_compile_options(llavon-ime-trainer PRIVATE ${IMESVC_TORCH_CXX_FLAGS})
    endif()
    if(MSVC)
        target_compile_options(llavon-ime-trainer PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(llavon-ime-trainer PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    if(APPLE)
        set_target_properties(llavon-ime-trainer PROPERTIES INSTALL_RPATH "@loader_path/../lib")
        install(DIRECTORY "${IMESVC_LIBTORCH_PREFIX}/lib/" DESTINATION lib COMPONENT trainer
                FILES_MATCHING PATTERN "*.dylib")
    elseif(UNIX)
        set_target_properties(llavon-ime-trainer PROPERTIES INSTALL_RPATH "$ORIGIN/../lib")
        install(DIRECTORY "${IMESVC_LIBTORCH_PREFIX}/lib/" DESTINATION lib COMPONENT trainer
                FILES_MATCHING PATTERN "lib*.so*")
    endif()
    install(TARGETS llavon-ime-trainer RUNTIME DESTINATION bin COMPONENT trainer)
endif()

target_compile_features(${IMESVC_TARGET} PRIVATE cxx_std_23)
set_target_properties(${IMESVC_TARGET} PROPERTIES
    CXX_STANDARD_REQUIRED YES
    CXX_EXTENSIONS NO
)
target_include_directories(${IMESVC_TARGET} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")

if(WIN32)
    target_compile_definitions(${IMESVC_TARGET} PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
        UNICODE
        _UNICODE
        WINVER=0x0A00
        _WIN32_WINNT=0x0A00
    )

    find_package(reflectcpp CONFIG REQUIRED)
    find_package(utf8cpp CONFIG REQUIRED)
    find_package(llama CONFIG REQUIRED)
    find_package(asio CONFIG REQUIRED)

    target_link_libraries(${IMESVC_TARGET} PRIVATE
        gdiplus
        advapi32
        ole32
        oleaut32
        ws2_32

        reflectcpp::reflectcpp
        utf8::cpp
        utf8cpp::utf8cpp
        llama
        asio::asio
    )
endif()

if(MSVC)
    target_compile_options(${IMESVC_TARGET} PRIVATE
        /W4
        /WX
        /utf-8
        /await:strict
    )
else()
    target_compile_options(${IMESVC_TARGET} PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    install(TARGETS ${IMESVC_TARGET}
        RUNTIME_DEPENDENCY_SET llavon_ime_service_RuntimeDeps
        RUNTIME DESTINATION bin
        COMPONENT runtime
    )
    install(RUNTIME_DEPENDENCY_SET llavon_ime_service_RuntimeDeps
        PRE_EXCLUDE_REGEXES [[api-ms-.*]] [[ext-ms-.*]]
        POST_EXCLUDE_REGEXES [[.*[\\/]system32[\\/].*\.dll]]
        RUNTIME DESTINATION bin
        COMPONENT runtime
    )
else()
    install(TARGETS ${IMESVC_TARGET} RUNTIME DESTINATION bin COMPONENT runtime)
endif()


if(IMESVC_BUILD_TESTS AND NOT WIN32)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(ime_service_tests
        tests/protocol_tests.cpp
        src/pipe/protocol.cpp
        src/engine/model_runtime.cpp
        src/engine/session_engine.cpp
        src/session/session_manager.cpp
        src/training/feedback_store.cpp
        src/training/training_orchestrator.cpp
    )
    target_compile_features(ime_service_tests PRIVATE cxx_std_23)
    target_include_directories(ime_service_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(ime_service_tests PRIVATE nlohmann_json::nlohmann_json ${IMESVC_SQLITE_TARGET} Threads::Threads)
    add_test(NAME ime_service_tests COMMAND ime_service_tests)
    add_executable(feedback_store_tests
        tests/feedback_store_tests.cpp
        src/training/feedback_store.cpp
        src/training/training_orchestrator.cpp
    )
    target_compile_features(feedback_store_tests PRIVATE cxx_std_23)
    target_include_directories(feedback_store_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(feedback_store_tests PRIVATE ${IMESVC_SQLITE_TARGET} Threads::Threads)
    if(MSVC)
        target_compile_options(feedback_store_tests PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(feedback_store_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME feedback_store_tests COMMAND feedback_store_tests)
    add_executable(training_lifecycle_tests
        tests/training_lifecycle_tests.cpp
        src/pipe/protocol.cpp
        src/engine/model_runtime.cpp
        src/training/feedback_store.cpp
        src/training/training_orchestrator.cpp
        src/training/adapter_publisher.cpp
        src/training/sha256.cpp
    )
    target_compile_features(training_lifecycle_tests PRIVATE cxx_std_23)
    target_include_directories(training_lifecycle_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_link_libraries(training_lifecycle_tests PRIVATE nlohmann_json::nlohmann_json ${IMESVC_SQLITE_TARGET} Threads::Threads)
    if(MSVC)
        target_compile_options(training_lifecycle_tests PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options(training_lifecycle_tests PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME training_lifecycle_tests COMMAND training_lifecycle_tests)
    if(IMESVC_BUILD_TRAINER)
        add_executable(ml_tests
            tests/ml_tests.cpp
            src/ml/llama_model.cpp
            src/ml/lora_linear.cpp
            src/ml/training_dataset.cpp
            src/ml/checkpoint.cpp
            src/ml/gguf_lora_writer.cpp
            src/training/sha256.cpp
            src/ml/safetensors_reader.cpp
        )
        target_compile_features(ml_tests PRIVATE cxx_std_23)
        target_include_directories(ml_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_compile_definitions(ml_tests PRIVATE
            IMESVC_TEST_TABLES_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/table")
        target_link_libraries(ml_tests PRIVATE torch nlohmann_json::nlohmann_json
                                              ${IMESVC_SQLITE_TARGET} ggml::ggml llama
                                              reflectcpp::reflectcpp ${IMESVC_UTF8_TARGET})
        if(TORCH_CXX_FLAGS)
            target_compile_options(ml_tests PRIVATE ${IMESVC_TORCH_CXX_FLAGS})
        endif()
        if(MSVC)
            target_compile_options(ml_tests PRIVATE /W4 /WX /utf-8)
        else()
            target_compile_options(ml_tests PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_test(NAME ml_tests COMMAND ml_tests)

        add_executable(model_parity_test
            tests/model_parity_test.cpp
            src/ml/safetensors_reader.cpp
            src/ml/llama_model.cpp
            src/ml/lora_linear.cpp
            src/ml/training_dataset.cpp
            src/ml/gguf_lora_writer.cpp
            src/training/sha256.cpp
        )
        target_compile_features(model_parity_test PRIVATE cxx_std_23)
        target_include_directories(model_parity_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(model_parity_test PRIVATE torch nlohmann_json::nlohmann_json
                                                       ${IMESVC_SQLITE_TARGET} ggml::ggml llama)
        if(TORCH_CXX_FLAGS)
            target_compile_options(model_parity_test PRIVATE ${IMESVC_TORCH_CXX_FLAGS})
        endif()
        if(MSVC)
            target_compile_options(model_parity_test PRIVATE /W4 /WX /utf-8)
        else()
            target_compile_options(model_parity_test PRIVATE -Wall -Wextra -Wpedantic)
        endif()

        add_executable(trainer_smoke_test
            tests/trainer_smoke_test.cpp
            src/trainer/trainer.cpp
            src/ml/safetensors_reader.cpp
            src/ml/llama_model.cpp
            src/ml/lora_linear.cpp
            src/ml/training_dataset.cpp
            src/ml/checkpoint.cpp
            src/ml/gguf_lora_writer.cpp
            src/training/feedback_store.cpp
            src/training/training_orchestrator.cpp
            src/training/sha256.cpp
        )
        target_compile_features(trainer_smoke_test PRIVATE cxx_std_23)
        target_include_directories(trainer_smoke_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(trainer_smoke_test PRIVATE torch nlohmann_json::nlohmann_json
                                                       ${IMESVC_SQLITE_TARGET} ggml::ggml llama Threads::Threads)
        if(TORCH_CXX_FLAGS)
            target_compile_options(trainer_smoke_test PRIVATE ${IMESVC_TORCH_CXX_FLAGS})
        endif()
        if(MSVC)
            target_compile_options(trainer_smoke_test PRIVATE /W4 /WX /utf-8)
        else()
            target_compile_options(trainer_smoke_test PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        add_executable(personalization_effect_test
            tests/personalization_effect_test.cpp
            src/trainer/trainer.cpp
            src/ml/safetensors_reader.cpp
            src/ml/llama_model.cpp
            src/ml/lora_linear.cpp
            src/ml/training_dataset.cpp
            src/ml/checkpoint.cpp
            src/ml/gguf_lora_writer.cpp
            src/training/feedback_store.cpp
            src/training/training_orchestrator.cpp
            src/training/sha256.cpp
        )
        target_compile_features(personalization_effect_test PRIVATE cxx_std_23)
        target_include_directories(personalization_effect_test PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
        target_link_libraries(personalization_effect_test PRIVATE torch nlohmann_json::nlohmann_json
                                                             ${IMESVC_SQLITE_TARGET} ggml::ggml llama
                                                             ${IMESVC_UTF8_TARGET} Threads::Threads)
        if(TORCH_CXX_FLAGS)
            target_compile_options(personalization_effect_test PRIVATE ${IMESVC_TORCH_CXX_FLAGS})
        endif()
        if(MSVC)
            target_compile_options(personalization_effect_test PRIVATE /W4 /WX /utf-8)
        else()
            target_compile_options(personalization_effect_test PRIVATE -Wall -Wextra -Wpedantic)
        endif()
        if(IMESVC_TEST_BASE_SAFETENSORS AND IMESVC_TEST_BASE_SHA256 AND IMESVC_TEST_BASE_GGUF)
            set(IMESVC_TEST_TABLES_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/table")
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tables/bopomofo_char.json")
                set(IMESVC_TEST_TABLES_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../tables")
            endif()
            add_test(NAME model_parity_test COMMAND model_parity_test
                "${IMESVC_TEST_BASE_SAFETENSORS}" "${IMESVC_TEST_BASE_SHA256}"
                "${IMESVC_TEST_BASE_GGUF}" "${IMESVC_TEST_TABLES_DIRECTORY}")
            add_test(NAME trainer_smoke_test COMMAND trainer_smoke_test
                "${IMESVC_TEST_BASE_SAFETENSORS}" "${IMESVC_TEST_BASE_SHA256}"
                "${IMESVC_TEST_BASE_GGUF}" "${IMESVC_TEST_TABLES_DIRECTORY}")
            set_tests_properties(model_parity_test trainer_smoke_test PROPERTIES TIMEOUT 900)
        endif()
        if(IMESVC_TEST_BASE_SAFETENSORS AND IMESVC_TEST_BASE_SHA256 AND IMESVC_TEST_RUNTIME_GGUF)
            set(IMESVC_TEST_TABLES_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/table")
            if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../tables/bopomofo_char.json")
                set(IMESVC_TEST_TABLES_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/../tables")
            endif()
            add_test(NAME personalization_effect_test COMMAND personalization_effect_test
                "${IMESVC_TEST_BASE_SAFETENSORS}" "${IMESVC_TEST_BASE_SHA256}"
                "${IMESVC_TEST_RUNTIME_GGUF}" "${IMESVC_TEST_TABLES_DIRECTORY}")
            set_tests_properties(personalization_effect_test PROPERTIES TIMEOUT 1800 LABELS "slow;personalization")
        endif()
    endif()
endif()
