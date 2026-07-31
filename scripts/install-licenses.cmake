cmake_minimum_required(VERSION 3.20)

foreach(required_variable IN ITEMS VCPKG_INSTALLED_DIR DESTINATION PROJECT_ROOT)
    if(NOT DEFINED ${required_variable} OR "${${required_variable}}" STREQUAL "")
        message(FATAL_ERROR "${required_variable} is required")
    endif()
endforeach()

if(NOT IS_DIRECTORY "${VCPKG_INSTALLED_DIR}")
    message(FATAL_ERROR "vcpkg installed directory does not exist: ${VCPKG_INSTALLED_DIR}")
endif()

if(DEFINED VCPKG_TARGET_TRIPLET AND NOT "${VCPKG_TARGET_TRIPLET}" STREQUAL "")
    set(vcpkg_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
else()
    file(GLOB installed_entries
        LIST_DIRECTORIES true
        RELATIVE "${VCPKG_INSTALLED_DIR}"
        "${VCPKG_INSTALLED_DIR}/*")
    set(installed_triplets "")
    foreach(entry IN LISTS installed_entries)
        if(IS_DIRECTORY "${VCPKG_INSTALLED_DIR}/${entry}/share")
            list(APPEND installed_triplets "${entry}")
        endif()
    endforeach()

    list(LENGTH installed_triplets installed_triplet_count)
    if(NOT installed_triplet_count EQUAL 1)
        message(FATAL_ERROR
            "Unable to select one vcpkg triplet under ${VCPKG_INSTALLED_DIR}: "
            "${installed_triplets}. Pass -DVCPKG_TARGET_TRIPLET=<triplet>.")
    endif()
    list(GET installed_triplets 0 VCPKG_TARGET_TRIPLET)
    set(vcpkg_prefix "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()

if(NOT IS_DIRECTORY "${vcpkg_prefix}/share")
    message(FATAL_ERROR "vcpkg triplet share directory does not exist: ${vcpkg_prefix}/share")
endif()

function(copy_license source component output_name required)
    if(NOT EXISTS "${source}")
        if(required)
            message(FATAL_ERROR "Missing license for ${component}: ${source}")
        endif()
        return()
    endif()

    file(MAKE_DIRECTORY "${DESTINATION}/${component}")
    configure_file("${source}" "${DESTINATION}/${component}/${output_name}" COPYONLY)
endfunction()

# This is the target dependency closure used by ime-unix-service. Build-only vcpkg
# helpers and shader compilers are intentionally excluded.
set(required_vcpkg_ports
    brotli
    cpp-httplib
    ctre
    ggml
    llama-cpp
    nlohmann-json
    reflectcpp
    utfcpp
    yyjson
)

foreach(port IN LISTS required_vcpkg_ports)
    copy_license("${vcpkg_prefix}/share/${port}/copyright" "${port}" LICENSE true)
endforeach()

# These target packages are only installed for the Vulkan backend. The Vulkan
# loader may instead be supplied by the operating system.
set(optional_vcpkg_ports
    spirv-headers
    vulkan-headers
    vulkan-loader
)

foreach(port IN LISTS optional_vcpkg_ports)
    copy_license("${vcpkg_prefix}/share/${port}/copyright" "${port}" LICENSE false)
endforeach()

copy_license("${PROJECT_ROOT}/LICENSE" "llavon-ime" LICENSE true)
copy_license("${PROJECT_ROOT}/ime-unix-service/LICENSE" "ime-unix-service" LICENSE true)
copy_license(
    "${PROJECT_ROOT}/packaging/licenses/MODEL_NOTICE.txt"
    "llavon-ime-model"
    NOTICE
    true)

message(STATUS "Installed license notices for vcpkg triplet ${VCPKG_TARGET_TRIPLET} to ${DESTINATION}")
