include_guard(GLOBAL)

find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)

set(
    TDLIB_PURPLE_API_ID_FILE
    ""
    CACHE FILEPATH
    "Path to the owner-only Telegram application API ID file"
)
set(
    TDLIB_PURPLE_API_HASH_FILE
    ""
    CACHE FILEPATH
    "Path to the owner-only Telegram application API hash file"
)

set(_tdlib_purple_removed_legacy_credential_cache FALSE)
foreach(_legacy_variable API_ID API_HASH STUFF)
    get_property(
        _legacy_variable_cached
        CACHE "${_legacy_variable}"
        PROPERTY TYPE
        SET
    )
    if (_legacy_variable_cached)
        unset("${_legacy_variable}" CACHE)
        set(_tdlib_purple_removed_legacy_credential_cache TRUE)
    endif()
endforeach()
if (_tdlib_purple_removed_legacy_credential_cache)
    message(FATAL_ERROR "CREDENTIAL_LEGACY_CACHE_REMOVED")
endif()
unset(_legacy_variable)
unset(_legacy_variable_cached)
unset(_tdlib_purple_removed_legacy_credential_cache)

get_filename_component(
    _TDLIB_PURPLE_CREDENTIALS_ROOT
    "${CMAKE_CURRENT_LIST_DIR}/.."
    ABSOLUTE
)
set(
    _TDLIB_PURPLE_CREDENTIALS_GENERATOR
    "${_TDLIB_PURPLE_CREDENTIALS_ROOT}/credentials/generate-application-credentials.py"
)
function(_tdlib_purple_credentials_report_failure diagnostic)
    if ("${diagnostic}" MATCHES "^CREDENTIAL_[A-Z_]+$")
        message(FATAL_ERROR "${diagnostic}")
    endif()

    message(FATAL_ERROR "CREDENTIAL_GENERATOR_FAILED")
endfunction()

function(
    tdlib_purple_configure_application_credentials
    target_prefix
    output_source
    output_refresh_target
)
    set(_state_output_variable)
    if (ARGC GREATER 3)
        set(_state_output_variable "${ARGV3}")
    endif()

    set(_private_directory "${CMAKE_CURRENT_BINARY_DIR}/.private")
    set(
        _provider_source
        "${_private_directory}/telegram-application-credentials-embedded.c"
    )
    set(_state_header)
    set(_state_arguments)
    if (NOT "${_state_output_variable}" STREQUAL "")
        set(
            _state_header
            "${_private_directory}/telegram-application-credentials-state.h"
        )
        list(APPEND _state_arguments "--state-output" "${_state_header}")
    endif()

    set(_id_configured TRUE)
    set(_hash_configured TRUE)
    if ("${TDLIB_PURPLE_API_ID_FILE}" STREQUAL "")
        set(_id_configured FALSE)
    endif()
    if ("${TDLIB_PURPLE_API_HASH_FILE}" STREQUAL "")
        set(_hash_configured FALSE)
    endif()

    set(_generator_arguments)
    if (_id_configured)
        get_filename_component(
            _api_id_path
            "${TDLIB_PURPLE_API_ID_FILE}"
            ABSOLUTE
            BASE_DIR "${CMAKE_SOURCE_DIR}"
        )
        list(
            APPEND _generator_arguments
            "--api-id-file" "${_api_id_path}"
        )
    endif()
    if (_hash_configured)
        get_filename_component(
            _api_hash_path
            "${TDLIB_PURPLE_API_HASH_FILE}"
            ABSOLUTE
            BASE_DIR "${CMAKE_SOURCE_DIR}"
        )
        list(
            APPEND _generator_arguments
            "--api-hash-file" "${_api_hash_path}"
        )
    endif()

    execute_process(
        COMMAND
            "${Python3_EXECUTABLE}"
            "${_TDLIB_PURPLE_CREDENTIALS_GENERATOR}"
            ${_generator_arguments}
            "--source-root"
            "${_TDLIB_PURPLE_CREDENTIALS_ROOT}"
            "--output"
            "${_provider_source}"
            ${_state_arguments}
        RESULT_VARIABLE _generator_result
        OUTPUT_QUIET
        ERROR_VARIABLE _generator_diagnostic
        ERROR_STRIP_TRAILING_WHITESPACE
    )
    if (NOT _generator_result EQUAL 0)
        _tdlib_purple_credentials_report_failure(
            "${_generator_diagnostic}"
        )
    endif()

    string(
        MAKE_C_IDENTIFIER
        "${target_prefix}_refresh_application_credentials"
        _refresh_target
    )
    add_custom_target(
        "${_refresh_target}" ALL
        COMMAND
            "${Python3_EXECUTABLE}"
            "${_TDLIB_PURPLE_CREDENTIALS_GENERATOR}"
            ${_generator_arguments}
            "--source-root"
            "${_TDLIB_PURPLE_CREDENTIALS_ROOT}"
            "--output"
            "${_provider_source}"
            ${_state_arguments}
        BYPRODUCTS
            "${_provider_source}"
            ${_state_header}
        DEPENDS
            "${_TDLIB_PURPLE_CREDENTIALS_GENERATOR}"
        COMMENT
            "Refreshing private Telegram application credentials"
        VERBATIM
    )

    set_source_files_properties(
        "${_provider_source}"
        PROPERTIES GENERATED TRUE
    )
    if (NOT "${_state_header}" STREQUAL "")
        set_source_files_properties(
            "${_state_header}"
            PROPERTIES GENERATED TRUE
        )
    endif()

    set("${output_source}" "${_provider_source}" PARENT_SCOPE)
    set("${output_refresh_target}" "${_refresh_target}" PARENT_SCOPE)
    if (NOT "${_state_output_variable}" STREQUAL "")
        set("${_state_output_variable}" "${_state_header}" PARENT_SCOPE)
    endif()
endfunction()
