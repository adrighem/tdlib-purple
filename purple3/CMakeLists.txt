cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(VERSION 1.1.1) # x-release-please-version
project(tdlib-purple-purple3 VERSION ${VERSION} LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(Purple3 REQUIRED IMPORTED_TARGET purple-3)
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
    target_compile_features(${target} PRIVATE c_std_11)

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

add_library(telegram-tdlib-purple3 MODULE
    "${PURPLE3_RESOURCES_C}"
    telegram-purple3-connection.c
    telegram-purple3-connection.h
    telegram-purple3-plugin.c
    telegram-purple3-protocol.c
    telegram-purple3-protocol.h
)
target_compile_definitions(telegram-tdlib-purple3 PRIVATE
    GETTEXT_PACKAGE="tdlib-purple"
    TDLIB_PURPLE_VERSION="${PROJECT_VERSION}"
)
target_link_libraries(telegram-tdlib-purple3 PRIVATE PkgConfig::Purple3)
set_target_properties(telegram-tdlib-purple3 PROPERTIES
    C_VISIBILITY_PRESET hidden
    OUTPUT_NAME telegram-tdlib
)
tdlib_purple3_target_defaults(telegram-tdlib-purple3)

include(CTest)
if (BUILD_TESTING)
    add_executable(purple3-plugin-smoke
        test/plugin-smoke.c
    )
    target_link_libraries(purple3-plugin-smoke PRIVATE PkgConfig::Purple3)
    tdlib_purple3_target_defaults(purple3-plugin-smoke)
    add_dependencies(purple3-plugin-smoke telegram-tdlib-purple3)

    set(PURPLE3_TEST_PROFILE "${CMAKE_CURRENT_BINARY_DIR}/test-profile")
    file(MAKE_DIRECTORY
        "${PURPLE3_TEST_PROFILE}/cache"
        "${PURPLE3_TEST_PROFILE}/config"
        "${PURPLE3_TEST_PROFILE}/data"
    )

    add_test(
        NAME purple3-plugin-smoke
        COMMAND purple3-plugin-smoke
    )
    set_tests_properties(purple3-plugin-smoke PROPERTIES
        TIMEOUT 10
        ENVIRONMENT
            "PURPLE_PLUGIN_PATH=${CMAKE_CURRENT_BINARY_DIR};PURPLE_PLUGINS_SKIP=1;GSETTINGS_BACKEND=memory;HOME=${PURPLE3_TEST_PROFILE};XDG_CACHE_HOME=${PURPLE3_TEST_PROFILE}/cache;XDG_CONFIG_HOME=${PURPLE3_TEST_PROFILE}/config;XDG_DATA_HOME=${PURPLE3_TEST_PROFILE}/data;LC_ALL=C;LANG=C;LANGUAGE=C"
    )
endif()
