cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(VERSION 2.1.0) # x-release-please-version
project(tdlib-purple-purple3 VERSION ${VERSION} LANGUAGES C CXX)

find_package(Td REQUIRED)
math(EXPR TDLIB_VERSION_NUMBER
    "10000*${Td_VERSION_MAJOR} + 100*${Td_VERSION_MINOR} + ${Td_VERSION_PATCH}"
)
if (TDLIB_VERSION_NUMBER LESS 10865)
    message(FATAL_ERROR "at least tdlib version 1.8.65 is required
(version found: ${Td_VERSION_MAJOR}.${Td_VERSION_MINOR}.${Td_VERSION_PATCH})")
endif()
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(Purple3 REQUIRED IMPORTED_TARGET purple-3)
include(
    "${CMAKE_CURRENT_SOURCE_DIR}/../cmake/TelegramApplicationCredentials.cmake"
)
find_program(
    GLIB_COMPILE_RESOURCES
    NAMES glib-compile-resources
)
if (NOT GLIB_COMPILE_RESOURCES)
    message(FATAL_ERROR "glib-compile-resources was not found")
endif()

option(
    TDLIB_PURPLE3_WARNINGS_AS_ERRORS
    "Treat warnings in the Purple 3 adapter as errors"
    ON
)

function(tdlib_purple3_target_defaults target)
    target_compile_features(${target} PRIVATE c_std_11 cxx_std_14)

    if (CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if (TDLIB_PURPLE3_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    elseif (MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if (TDLIB_PURPLE3_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    endif()
endfunction()

function(tdlib_purple3_test_target_defaults target)
    tdlib_purple3_target_defaults(${target})

    # GLib turns g_test_init() into an immediate failure when assertions are
    # disabled. Tests must retain their assertions even in a release build.
    if (CMAKE_C_COMPILER_ID MATCHES "^(GNU|Clang|AppleClang)$")
        target_compile_options(${target} PRIVATE -UG_DISABLE_ASSERT)
    elseif (MSVC)
        target_compile_options(${target} PRIVATE /UG_DISABLE_ASSERT)
    endif()
endfunction()

tdlib_purple_configure_application_credentials(
    "tdlib-purple-purple3"
    TDLIB_PURPLE3_CREDENTIALS_PROVIDER_SOURCE
    TDLIB_PURPLE3_CREDENTIALS_REFRESH_TARGET
    TDLIB_PURPLE3_CREDENTIALS_STATE_HEADER
)

set(PURPLE3_RESOURCES_C
    "${CMAKE_CURRENT_BINARY_DIR}/telegram-purple3-resources.c"
)
add_custom_command(
    OUTPUT "${PURPLE3_RESOURCES_C}"
    COMMAND
        "${GLIB_COMPILE_RESOURCES}"
        --c-name=telegram_tdlib_resources
        --generate-source
        --internal
        "--target=${PURPLE3_RESOURCES_C}"
        "--sourcedir=${CMAKE_CURRENT_SOURCE_DIR}/../data"
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/telegram-purple3.gresource.xml"
    DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/resources/telegram-purple3.gresource.xml"
        "${CMAKE_CURRENT_SOURCE_DIR}/../data/telegram16.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/../data/telegram22.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/../data/telegram48.png"
    VERBATIM
)

set(PURPLE3_CONNECTION_CORE_SOURCES
    "${CMAKE_CURRENT_SOURCE_DIR}/../td-auth-controller.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../td-transport.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../td-polling-backend.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/../module-activity.cpp"
    telegram-purple3-auth-presenter.cpp
    telegram-purple3-auth-presenter.h
    telegram-purple3-session.cpp
    telegram-purple3-session.h
    telegram-purple3-session-private.h
)

add_library(telegram-tdlib-purple3-core STATIC
    ${PURPLE3_CONNECTION_CORE_SOURCES}
)
target_include_directories(telegram-tdlib-purple3-core PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../credentials"
)
target_compile_definitions(telegram-tdlib-purple3-core PRIVATE
    GETTEXT_PACKAGE="tdlib-purple"
)
target_link_libraries(telegram-tdlib-purple3-core
    PRIVATE
        PkgConfig::Purple3
        Td::TdStatic
        Threads::Threads
)
set_target_properties(telegram-tdlib-purple3-core PROPERTIES
    CXX_VISIBILITY_PRESET hidden
    POSITION_INDEPENDENT_CODE ON
    VISIBILITY_INLINES_HIDDEN YES
)
tdlib_purple3_target_defaults(telegram-tdlib-purple3-core)

add_library(telegram-tdlib-purple3 MODULE
    "${PURPLE3_RESOURCES_C}"
    "${CMAKE_CURRENT_SOURCE_DIR}/../credentials/telegram-application-credentials.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../credentials/telegram-application-credentials.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../credentials/telegram-application-credentials-private.h"
    "${TDLIB_PURPLE3_CREDENTIALS_PROVIDER_SOURCE}"
    telegram-purple3-application-credentials.c
    telegram-purple3-application-credentials.h
    telegram-purple3-connection.c
    telegram-purple3-connection.h
    telegram-purple3-connection-private.h
    telegram-purple3-plugin.c
    telegram-purple3-protocol.c
    telegram-purple3-protocol.h
)
target_include_directories(telegram-tdlib-purple3 PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/../credentials"
)
target_compile_definitions(telegram-tdlib-purple3 PRIVATE
    GETTEXT_PACKAGE="tdlib-purple"
    TDLIB_PURPLE_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(telegram-tdlib-purple3 PRIVATE
    telegram-tdlib-purple3-core
    PkgConfig::Purple3
)
set_target_properties(telegram-tdlib-purple3 PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    OUTPUT_NAME telegram-tdlib
    VISIBILITY_INLINES_HIDDEN YES
)
tdlib_purple3_target_defaults(telegram-tdlib-purple3)
if (NOT "${TDLIB_PURPLE3_CREDENTIALS_REFRESH_TARGET}" STREQUAL "")
    add_dependencies(
        telegram-tdlib-purple3
        "${TDLIB_PURPLE3_CREDENTIALS_REFRESH_TARGET}"
    )
endif()

include(CTest)
if (BUILD_TESTING)
    set(
        PURPLE3_APPLICATION_CREDENTIALS_CORE
        "${CMAKE_CURRENT_SOURCE_DIR}/../credentials/telegram-application-credentials.c"
        telegram-purple3-application-credentials.c
    )

    add_executable(purple3-application-credentials-test
        ${PURPLE3_APPLICATION_CREDENTIALS_CORE}
        test/application-credentials-test.c
        test/application-credentials-test-backend.c
        test/application-credentials-test-backend.h
    )
    target_include_directories(purple3-application-credentials-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../credentials"
        "${CMAKE_CURRENT_SOURCE_DIR}/test"
    )
    target_compile_definitions(
        purple3-application-credentials-test
        PRIVATE GETTEXT_PACKAGE="tdlib-purple"
    )
    target_link_libraries(
        purple3-application-credentials-test
        PRIVATE PkgConfig::Purple3
    )
    tdlib_purple3_test_target_defaults(
        purple3-application-credentials-test
    )

    add_executable(purple3-plugin-smoke
        "${TDLIB_PURPLE3_CREDENTIALS_STATE_HEADER}"
        test/plugin-smoke.c
    )
    target_include_directories(purple3-plugin-smoke PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/.private"
    )
    target_link_libraries(purple3-plugin-smoke PRIVATE PkgConfig::Purple3)
    tdlib_purple3_test_target_defaults(purple3-plugin-smoke)
    add_dependencies(purple3-plugin-smoke telegram-tdlib-purple3)
    if (NOT "${TDLIB_PURPLE3_CREDENTIALS_REFRESH_TARGET}" STREQUAL "")
        add_dependencies(
            purple3-plugin-smoke
            "${TDLIB_PURPLE3_CREDENTIALS_REFRESH_TARGET}"
        )
    endif()

    add_executable(purple3-auth-presenter-test
        test/auth-presenter-test.cpp
    )
    target_include_directories(purple3-auth-presenter-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
    )
    target_compile_definitions(
        purple3-auth-presenter-test
        PRIVATE GETTEXT_PACKAGE="tdlib-purple"
    )
    target_link_libraries(purple3-auth-presenter-test PRIVATE
        telegram-tdlib-purple3-core
        PkgConfig::Purple3
        Td::TdStatic
    )
    tdlib_purple3_test_target_defaults(purple3-auth-presenter-test)

    add_executable(purple3-session-test
        "${CMAKE_CURRENT_SOURCE_DIR}/../credentials/telegram-application-credentials.c"
        telegram-purple3-application-credentials.c
        telegram-purple3-connection.c
        telegram-purple3-connection-private.h
        test/application-credentials-test-backend.c
        test/application-credentials-test-backend.h
        test/session-test.cpp
    )
    target_include_directories(purple3-session-test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/.."
        "${CMAKE_CURRENT_SOURCE_DIR}/../credentials"
        "${CMAKE_CURRENT_SOURCE_DIR}/test"
    )
    target_compile_definitions(
        purple3-session-test
        PRIVATE GETTEXT_PACKAGE="tdlib-purple"
    )
    target_link_libraries(purple3-session-test PRIVATE
        telegram-tdlib-purple3-core
        PkgConfig::Purple3
        Td::TdStatic
        Threads::Threads
    )
    tdlib_purple3_test_target_defaults(purple3-session-test)

    set(PURPLE3_TEST_PROFILE "${CMAKE_CURRENT_BINARY_DIR}/test-profile")
    file(MAKE_DIRECTORY
        "${PURPLE3_TEST_PROFILE}/cache"
        "${PURPLE3_TEST_PROFILE}/config"
        "${PURPLE3_TEST_PROFILE}/data"
    )

    add_test(
        NAME purple3-application-credentials
        COMMAND purple3-application-credentials-test
    )
    set_tests_properties(
        purple3-application-credentials
        PROPERTIES
            TIMEOUT 10
            ENVIRONMENT
                "G_DEBUG=fatal-warnings;LC_ALL=C;LANG=C;LANGUAGE=C"
    )

    add_test(
        NAME purple3-plugin-smoke
        COMMAND purple3-plugin-smoke
    )
    add_test(
        NAME purple3-auth-presenter
        COMMAND purple3-auth-presenter-test
    )
    add_test(
        NAME purple3-session
        COMMAND purple3-session-test
    )
    set_tests_properties(purple3-plugin-smoke PROPERTIES
        TIMEOUT 10
        ENVIRONMENT
            "PURPLE_PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR};PURPLE_PLUGINS_SKIP=1;GSETTINGS_BACKEND=memory;G_DEBUG=fatal-warnings;HOME=${PURPLE3_TEST_PROFILE};XDG_CACHE_HOME=${PURPLE3_TEST_PROFILE}/cache;XDG_CONFIG_HOME=${PURPLE3_TEST_PROFILE}/config;XDG_DATA_HOME=${PURPLE3_TEST_PROFILE}/data;LC_ALL=C;LANG=C;LANGUAGE=C"
    )
    set_tests_properties(
        purple3-auth-presenter
        purple3-session
        PROPERTIES
        TIMEOUT 10
        ENVIRONMENT "G_DEBUG=fatal-warnings;LC_ALL=C;LANG=C;LANGUAGE=C"
    )
endif()
