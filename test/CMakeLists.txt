if (DEFINED GTEST_PATH)
    set(GTEST_LIBRARY_DIR "${GTEST_PATH}/lib")
endif (DEFINED GTEST_PATH)

set(GLIB_LIBRARIES "glib-2.0" CACHE STRING "GLib libraries")

# Compiling plugin sources again is not optimal but it's the easy way
add_executable(tests EXCLUDE_FROM_ALL
    test-main.cpp
    login-test.cpp
    private-chat-test.cpp
    group-chat-test.cpp
    supergroup-test.cpp
    file-transfer-test.cpp
    secret-chat-test.cpp
    message-split-test.cpp
    message-order-test.cpp
    message-history-test.cpp
    test-transceiver.cpp
    libpurple-mock.cpp
    printout.cpp
    purple-events.cpp
    fixture.cpp

    ${TDLIB_PURPLE_PLUGIN_SOURCES}
)

target_compile_features(tests PRIVATE cxx_std_17)
target_include_directories(tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_BINARY_DIR}
)
target_include_directories(tests SYSTEM PRIVATE ${Purple_INCLUDE_DIRS})
target_link_libraries(tests PRIVATE gtest fmt::fmt Td::TdStatic ${GLIB_LIBRARIES})

if (DEFINED GTEST_PATH)
    target_include_directories(tests PRIVATE ${GTEST_PATH}/include)
    target_link_directories(tests PRIVATE ${GTEST_LIBRARY_DIR})
endif (DEFINED GTEST_PATH)

if (NOT NoWebp)
    if (libwebp_LIBRARY_DIRS OR libpng_LIBRARY_DIRS)
        target_link_directories(tests PRIVATE ${libwebp_LIBRARY_DIRS} ${libpng_LIBRARY_DIRS})
    endif (libwebp_LIBRARY_DIRS OR libpng_LIBRARY_DIRS)
    target_link_libraries(tests PRIVATE ${libwebp_LIBRARIES} ${libpng_LIBRARIES})
endif (NOT NoWebp)

if (NOT NoLottie)
    if (NOT NoBundledLottie)
        target_include_directories(tests PRIVATE ${CMAKE_SOURCE_DIR}/rlottie/inc)
    endif (NOT NoBundledLottie)
    target_link_libraries(tests PRIVATE rlottie)
    target_compile_definitions(tests PRIVATE LOT_BUILD)
endif (NOT NoLottie)

if (NOT NoTranslations)
    target_include_directories(tests PRIVATE ${Intl_INCLUDE_DIRS})
    target_link_libraries(tests PRIVATE ${Intl_LIBRARIES})
endif (NOT NoTranslations)

if (NOT tgvoip_INCLUDE_DIRS STREQUAL "")
    target_include_directories(tests SYSTEM PRIVATE ${tgvoip_INCLUDE_DIRS})
endif (NOT tgvoip_INCLUDE_DIRS STREQUAL "")
if (NOT NoVoip)
    target_link_libraries(tests PRIVATE ${tgvoip_LIBRARIES})
endif (NOT NoVoip)

add_custom_target(run-tests ${CMAKE_CURRENT_BINARY_DIR}/tests DEPENDS tests)
