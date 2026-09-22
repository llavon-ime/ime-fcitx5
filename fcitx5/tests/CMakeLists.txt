# Addon-specific checks only. Input behaviour is covered by the standard
# raw-key suite (engine/tests/rawkey), which is host-free and runs on Linux and
# macOS alike, so nothing here drives keys through fcitx5 any more.
if(Fcitx5_FOUND AND NOT WIN32)
    add_test(NAME llavon_ime_fcitx5_metadata
        COMMAND ${CMAKE_COMMAND}
            -DADDON_CONF=${CMAKE_BINARY_DIR}/fcitx5/addon/llavon-ime.conf
            -DINPUTMETHOD_CONF=${CMAKE_BINARY_DIR}/fcitx5/inputmethod/llavon-ime.conf
            -P ${CMAKE_CURRENT_LIST_DIR}/check_fcitx5_metadata.cmake)
endif()
