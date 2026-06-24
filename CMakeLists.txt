cmake_minimum_required(VERSION 3.16 FATAL_ERROR)

set(VERSION 1.0.0) # x-release-please-version
project(tdlib-purple VERSION ${VERSION} LANGUAGES CXX)

find_package(Td REQUIRED)
math(EXPR TDLIB_VERSION_NUMBER "10000*${Td_VERSION_MAJOR} + 100*${Td_VERSION_MINOR} + ${Td_VERSION_PATCH}")
if (${TDLIB_VERSION_NUMBER} LESS 10800)
    # Error message must begin with "tdlib version" for a grep command from readme
    message(FATAL_ERROR "at least tdlib version 1.8.0 is required
(version found: ${Td_VERSION_MAJOR}.${Td_VERSION_MINOR}.${Td_VERSION_PATCH})")
endif (${TDLIB_VERSION_NUMBER} LESS 10800)

set(NoPkgConfig FALSE CACHE BOOL "Do not use pkg-config")
set(NoWebp FALSE CACHE BOOL "Do not decode webp stickers")
set(NoBundledFmt FALSE CACHE BOOL "Do not use bundled fmt library")
set(NoBundledLottie FALSE CACHE BOOL "Do not use bundled rlottie library")
set(NoLottie FALSE CACHE BOOL "Disable animated sticker conversion")
set(NoTranslations FALSE CACHE BOOL "Disable translation support")
set(NoVoip FALSE CACHE BOOL "Disable voice call support")
set(API_ID 94575 CACHE STRING "API id")
set(API_HASH a3406de8d171bb422bb6ddf3bbd800e2 CACHE STRING "API hash")
set(STUFF "" CACHE STRING "")

if (NOT NoPkgConfig)
    include(FindPkgConfig)
    pkg_check_modules(Purple REQUIRED purple)
    if (NOT NoWebp)
        pkg_check_modules(libwebp libwebp)
        pkg_check_modules(libpng libpng)
    endif (NOT NoWebp)
    if (NOT NoVoip)
        pkg_check_modules(tgvoip tgvoip)
    endif(NOT NoVoip)

    pkg_get_variable(PURPLE_PLUGIN_DIR purple plugindir)
    pkg_get_variable(PURPLE_DATA_DIR purple datarootdir)
endif (NOT NoPkgConfig)

if (NOT NoWebp)
    if ("${libwebp_LIBRARIES}" STREQUAL "")
        message(FATAL_ERROR "Webp library not found, build with -DNoWebp=TRUE to disable webp sticker decoding")
    endif ("${libwebp_LIBRARIES}" STREQUAL "")
    if ("${libpng_LIBRARIES}" STREQUAL "")
        message(FATAL_ERROR "libpng not found, build with -DNoWebp=TRUE to disable webp sticker decoding")
    endif ("${libpng_LIBRARIES}" STREQUAL "")
endif (NOT NoWebp)

configure_file(buildopt.h.in buildopt.h)
configure_file(config.cpp.in config.cpp)

set(TDLIB_PURPLE_PLUGIN_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/tdlib-purple.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/td-client.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/td-client-auth.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/transceiver.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/account-data.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/purple-info.cpp
    ${CMAKE_BINARY_DIR}/config.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/client-utils.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/receiving.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/format.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/sticker.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/file-transfer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/call.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/identifiers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/secret-chat.cpp
)

add_library(telegram-tdlib SHARED ${TDLIB_PURPLE_PLUGIN_SOURCES})

# libpurple uses the deprecated glib-type `GParameter` and the deprecated glib-macro `G_CONST_RETURN`, which
# spams the console with useless warnings that we can do nothing about.
target_compile_definitions(telegram-tdlib PRIVATE GLIB_DISABLE_DEPRECATION_WARNINGS)
#target_compile_options(telegram-tdlib PRIVATE -Wmissing-declarations)

target_include_directories(telegram-tdlib
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}
)
target_include_directories(telegram-tdlib SYSTEM PRIVATE ${Purple_INCLUDE_DIRS})
if (Purple_LIBRARY_DIRS)
    target_link_directories(telegram-tdlib PRIVATE ${Purple_LIBRARY_DIRS})
endif (Purple_LIBRARY_DIRS)
target_link_libraries(telegram-tdlib PRIVATE Td::TdStatic ${Purple_LIBRARIES})

set(VERSION_SCRIPT_FLAG "-Wl,--version-script=${CMAKE_SOURCE_DIR}/ld_version_script")
set(CMAKE_EXE_LINKER_FLAGS ${VERSION_SCRIPT_FLAG})
try_compile(VERSION_SCRIPT_SUPPORTED ${CMAKE_BINARY_DIR} ${CMAKE_SOURCE_DIR}/cmake/testflag.cpp)
set(CMAKE_EXE_LINKER_FLAGS "")
if (${VERSION_SCRIPT_SUPPORTED})
    target_link_options(telegram-tdlib PRIVATE ${VERSION_SCRIPT_FLAG})
endif (${VERSION_SCRIPT_SUPPORTED})

if (NOT NoWebp)
    target_include_directories(telegram-tdlib SYSTEM PRIVATE ${libwebp_INCLUDE_DIRS} ${libpng_INCLUDE_DIRS})
    if (libwebp_LIBRARY_DIRS OR libpng_LIBRARY_DIRS)
        target_link_directories(telegram-tdlib PRIVATE ${libwebp_LIBRARY_DIRS} ${libpng_LIBRARY_DIRS})
    endif (libwebp_LIBRARY_DIRS OR libpng_LIBRARY_DIRS)
    target_link_libraries(telegram-tdlib PRIVATE ${libwebp_LIBRARIES} ${libpng_LIBRARIES})
endif (NOT NoWebp)

target_compile_features(telegram-tdlib PRIVATE cxx_std_14)

find_package(fmt CONFIG QUIET)
if (fmt_FOUND)
    set(TDLIB_PURPLE_FMT_TARGET fmt::fmt)
else (fmt_FOUND)
    if (NoBundledFmt)
        message(FATAL_ERROR "fmt library not found, build without -DNoBundledFmt=TRUE to use the bundled fallback")
    endif (NoBundledFmt)
    set(BUILD_SHARED_LIBS OFF)
    add_subdirectory(fmt)
    target_compile_options(fmt PRIVATE -fPIC)
    set(TDLIB_PURPLE_FMT_TARGET fmt::fmt)
endif (fmt_FOUND)
target_link_libraries(telegram-tdlib PRIVATE ${TDLIB_PURPLE_FMT_TARGET})

if (NOT NoLottie)
    if (NOT NoPkgConfig)
        pkg_check_modules(RLOTTIE QUIET IMPORTED_TARGET rlottie)
        if (TARGET PkgConfig::RLOTTIE)
            set(TDLIB_PURPLE_RLOTTIE_TARGET PkgConfig::RLOTTIE)
        endif (TARGET PkgConfig::RLOTTIE)
    endif (NOT NoPkgConfig)

    if (NOT TDLIB_PURPLE_RLOTTIE_TARGET)
        find_path(RLOTTIE_INCLUDE_DIR rlottie.h)
        find_library(RLOTTIE_LIBRARY NAMES rlottie)
        if (RLOTTIE_INCLUDE_DIR AND RLOTTIE_LIBRARY)
            add_library(tdlib-purple-system-rlottie UNKNOWN IMPORTED)
            set_target_properties(tdlib-purple-system-rlottie PROPERTIES
                IMPORTED_LOCATION "${RLOTTIE_LIBRARY}"
                INTERFACE_INCLUDE_DIRECTORIES "${RLOTTIE_INCLUDE_DIR}"
            )
            set(TDLIB_PURPLE_RLOTTIE_TARGET tdlib-purple-system-rlottie)
        endif (RLOTTIE_INCLUDE_DIR AND RLOTTIE_LIBRARY)
    endif (NOT TDLIB_PURPLE_RLOTTIE_TARGET)

    if (TDLIB_PURPLE_RLOTTIE_TARGET)
        target_link_libraries(telegram-tdlib PRIVATE ${TDLIB_PURPLE_RLOTTIE_TARGET})
    else (TDLIB_PURPLE_RLOTTIE_TARGET)
        if (NoBundledLottie)
            message(FATAL_ERROR "rlottie library not found, build without -DNoBundledLottie=TRUE to use the bundled fallback")
        endif (NoBundledLottie)
        set(BUILD_SHARED_LIBS OFF)
        add_subdirectory(rlottie)
        target_compile_options(rlottie PRIVATE -fPIC -Wno-error=unused-value)
        target_include_directories(telegram-tdlib PRIVATE rlottie/inc)
        target_link_libraries(telegram-tdlib PRIVATE rlottie)
        set(TDLIB_PURPLE_RLOTTIE_TARGET rlottie)
        set(TDLIB_PURPLE_USE_BUNDLED_RLOTTIE TRUE)
    endif (TDLIB_PURPLE_RLOTTIE_TARGET)
    # When building with cygwin, lack of LOT_BUILD would add dllimport attribute to function
    # prototypes from rlottie, causing them to have different name mangling from what they had when
    # building rlottie.
    target_compile_definitions(telegram-tdlib PRIVATE LOT_BUILD)
endif (NOT NoLottie)

if (NOT DEFINED SHARE_INSTALL_PREFIX)
    set(SHARE_INSTALL_PREFIX "share")
endif (NOT DEFINED SHARE_INSTALL_PREFIX)

if (NOT tgvoip_INCLUDE_DIRS STREQUAL "")
    target_include_directories(telegram-tdlib SYSTEM PRIVATE ${tgvoip_INCLUDE_DIRS})
endif (NOT tgvoip_INCLUDE_DIRS STREQUAL "")
if (NOT NoVoip)
    target_link_libraries(telegram-tdlib PRIVATE ${tgvoip_LIBRARIES})
endif (NOT NoVoip)

if (DEFINED STANDARD_LIBRARIES_EXTRA)
    set (CMAKE_CXX_STANDARD_LIBRARIES "${CMAKE_CXX_STANDARD_LIBRARIES} ${STANDARD_LIBRARIES_EXTRA}")
endif (DEFINED STANDARD_LIBRARIES_EXTRA)

install(TARGETS telegram-tdlib DESTINATION "${PURPLE_PLUGIN_DIR}")
install(FILES data/telegram16.png DESTINATION "${PURPLE_DATA_DIR}/pixmaps/pidgin/protocols/16" RENAME telegram.png)
install(FILES data/telegram22.png DESTINATION "${PURPLE_DATA_DIR}/pixmaps/pidgin/protocols/22" RENAME telegram.png)
install(FILES data/telegram48.png DESTINATION "${PURPLE_DATA_DIR}/pixmaps/pidgin/protocols/48" RENAME telegram.png)
install(FILES data/tdlib-purple.metainfo.xml DESTINATION "${SHARE_INSTALL_PREFIX}/metainfo")

if (NOT TARGET uninstall)
    configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/cmake/cmake_uninstall.cmake.in"
        "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        @ONLY
    )
    add_custom_target(uninstall
        COMMAND "${CMAKE_COMMAND}" -P "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
        COMMENT "Uninstalling files from install_manifest.txt"
        USES_TERMINAL
    )
endif()

add_subdirectory(test)

add_custom_command(
    OUTPUT libtelegram-tdlib-stripped.dll
    COMMAND i686-w64-mingw32-strip -o libtelegram-tdlib-stripped.dll libtelegram-tdlib.dll
    DEPENDS telegram-tdlib
)
add_custom_command(
    OUTPUT lang.nsi
    COMMAND ${CMAKE_SOURCE_DIR}/po/gen-nsi.sh lang.nsi
    DEPENDS ${CMAKE_SOURCE_DIR}/po/gen-nsi.sh
    DEPENDS ${CMAKE_SOURCE_DIR}/po/LINGUAS
)
add_custom_target(nsis
    makensis -DBUILD_DIR=${CMAKE_BINARY_DIR} -DPRPL_LIBRARY=libtelegram-tdlib-stripped.dll -DPLUGIN_VERSION=${VERSION} ${CMAKE_SOURCE_DIR}/tdlib-purple.nsi
    DEPENDS libtelegram-tdlib-stripped.dll lang.nsi
)

if (NOT NoTranslations)
    find_package(Gettext REQUIRED)
    file(STRINGS po/LINGUAS _linguas)
    foreach(_lang ${_linguas})
        GETTEXT_PROCESS_PO_FILES(
            ${_lang} ALL
            PO_FILES "${CMAKE_SOURCE_DIR}/po/${_lang}.po"
        )
        install(
            FILES ${CMAKE_CURRENT_BINARY_DIR}/${_lang}.gmo
            DESTINATION "${SHARE_INSTALL_PREFIX}/locale/${_lang}/LC_MESSAGES"
            RENAME tdlib-purple.mo
        )
        add_custom_target(idontwanttohavethisrule_${_lang} DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/${_lang}.gmo)
        add_dependencies(nsis idontwanttohavethisrule_${_lang})
    endforeach()

    find_package(Intl REQUIRED)
    target_include_directories(telegram-tdlib PRIVATE ${Intl_INCLUDE_DIRS})
    target_link_libraries(telegram-tdlib PRIVATE ${Intl_LIBRARIES})
endif (NOT NoTranslations)
