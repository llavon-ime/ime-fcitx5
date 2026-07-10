include(FindPackageHandleStandardArgs)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FCITX5_CORE IMPORTED_TARGET Fcitx5Core)

if(FCITX5_CORE_FOUND)
    pkg_get_variable(FCITX_INSTALL_ADDONDIR Fcitx5Core addonlibdir)
    pkg_get_variable(FCITX_INSTALL_PKGDATADIR Fcitx5Core pkgdatadir)
endif()

if(NOT FCITX5_CORE_FOUND AND APPLE)
    set(FCITX5_MACOS_SOURCE_DIR "$ENV{FCITX5_MACOS_SOURCE_DIR}" CACHE PATH "fcitx5-macos source checkout")
    set(FCITX5_MACOS_APP_CONTENTS "/Library/Input Methods/Fcitx5.app/Contents" CACHE PATH "Fcitx5.app Contents path")

    set(_fcitx5_macos_fcitx5_source "")
    if(EXISTS "${FCITX5_MACOS_SOURCE_DIR}/fcitx5/src/lib/fcitx/inputmethodengine.h")
        set(_fcitx5_macos_fcitx5_source "${FCITX5_MACOS_SOURCE_DIR}/fcitx5")
    elseif(EXISTS "${FCITX5_MACOS_SOURCE_DIR}/src/lib/fcitx/inputmethodengine.h")
        set(_fcitx5_macos_fcitx5_source "${FCITX5_MACOS_SOURCE_DIR}")
    endif()

    if(_fcitx5_macos_fcitx5_source AND EXISTS "${FCITX5_MACOS_APP_CONTENTS}/lib/libFcitx5Core.dylib")
        set(_fcitx5_macos_generated_include "${CMAKE_BINARY_DIR}/fcitx5-macos-generated-include")
        file(MAKE_DIRECTORY
            "${_fcitx5_macos_generated_include}/fcitx"
            "${_fcitx5_macos_generated_include}/fcitx-config"
            "${_fcitx5_macos_generated_include}/fcitx-utils")
        foreach(_header IN ITEMS
                "fcitx/fcitxcore_export.h:FCITXCORE"
                "fcitx-config/fcitxconfig_export.h:FCITXCONFIG"
                "fcitx-utils/fcitxutils_export.h:FCITXUTILS")
            string(REPLACE ":" ";" _parts "${_header}")
            list(GET _parts 0 _relative_header)
            list(GET _parts 1 _macro_prefix)
            file(WRITE "${_fcitx5_macos_generated_include}/${_relative_header}"
"#pragma once
#define ${_macro_prefix}_EXPORT __attribute__((visibility(\"default\")))
#define ${_macro_prefix}_NO_EXPORT __attribute__((visibility(\"hidden\")))
#define ${_macro_prefix}_DEPRECATED __attribute__((__deprecated__))
#define ${_macro_prefix}_DEPRECATED_EXPORT ${_macro_prefix}_EXPORT ${_macro_prefix}_DEPRECATED
#define ${_macro_prefix}_DEPRECATED_NO_EXPORT ${_macro_prefix}_NO_EXPORT ${_macro_prefix}_DEPRECATED
")
        endforeach()

        set(_fcitx5_macos_include_dirs
            "${_fcitx5_macos_generated_include}"
            "${_fcitx5_macos_fcitx5_source}/src/lib")

        add_library(Fcitx5::Utils SHARED IMPORTED)
        set_target_properties(Fcitx5::Utils PROPERTIES
            IMPORTED_LOCATION "${FCITX5_MACOS_APP_CONTENTS}/lib/libFcitx5Utils.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${_fcitx5_macos_include_dirs}")

        add_library(Fcitx5::Config SHARED IMPORTED)
        set_target_properties(Fcitx5::Config PROPERTIES
            IMPORTED_LOCATION "${FCITX5_MACOS_APP_CONTENTS}/lib/libFcitx5Config.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${_fcitx5_macos_include_dirs}"
            INTERFACE_LINK_LIBRARIES Fcitx5::Utils)

        add_library(Fcitx5::Core SHARED IMPORTED)
        set_target_properties(Fcitx5::Core PROPERTIES
            IMPORTED_LOCATION "${FCITX5_MACOS_APP_CONTENTS}/lib/libFcitx5Core.dylib"
            INTERFACE_INCLUDE_DIRECTORIES "${_fcitx5_macos_include_dirs}"
            INTERFACE_LINK_LIBRARIES "Fcitx5::Config;Fcitx5::Utils")

        set(FCITX5_CORE_FOUND TRUE)
        set(FCITX5_CORE_LINK_LIBRARIES Fcitx5::Core)
        if(DEFINED ENV{HOME})
            if(NOT DEFINED FCITX_INSTALL_ADDONDIR)
                set(FCITX_INSTALL_ADDONDIR "$ENV{HOME}/Library/fcitx5/lib/fcitx5")
            endif()
            if(NOT DEFINED FCITX_INSTALL_PKGDATADIR)
                set(FCITX_INSTALL_PKGDATADIR "$ENV{HOME}/Library/fcitx5/share/fcitx5")
            endif()
        endif()
    endif()
endif()

find_package_handle_standard_args(Fcitx5 REQUIRED_VARS FCITX5_CORE_LINK_LIBRARIES)

if(Fcitx5_FOUND AND NOT TARGET Fcitx5::Core)
    add_library(Fcitx5::Core ALIAS PkgConfig::FCITX5_CORE)
endif()
