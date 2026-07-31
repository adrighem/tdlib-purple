if (DEFINED GTEST_PATH)
    set(GTEST_LIBRARY_DIR "${GTEST_PATH}/lib")
endif (DEFINED GTEST_PATH)

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
    forum-topic-test.cpp
    forum-topic-file-transfer-test.cpp
    forum-topic-join-test.cpp
    forum-topic-message-update-test.cpp
    forum-topic-reconnect-test.cpp
    forum-topic-receiving-test.cpp
    forum-topic-room-test.cpp
    forum-topic-sending-test.cpp
    forum-topics-adapter-test.cpp
    plugin-lifecycle-test.cpp
    libpurple-mock-test.cpp
    test-transceiver.cpp
    libpurple-mock.cpp
    printout.cpp
    purple-events.cpp
    fixture.cpp
    application-credentials-test.cpp
    application-credentials-test-backend.c

    ${TDLIB_PURPLE_COMMON_SOURCES}
)

target_compile_features(tests PRIVATE c_std_11 cxx_std_17)
target_include_directories(tests
    PRIVATE
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/credentials
        ${CMAKE_BINARY_DIR}
)
target_include_directories(tests SYSTEM PRIVATE ${Purple_INCLUDE_DIRS})
target_link_libraries(tests PRIVATE gtest ${TDLIB_PURPLE_FMT_TARGET} Td::TdStatic ${GLIB_LIBRARIES})

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
    if (TDLIB_PURPLE_USE_BUNDLED_RLOTTIE)
        target_include_directories(tests PRIVATE ${CMAKE_SOURCE_DIR}/rlottie/inc)
    endif (TDLIB_PURPLE_USE_BUNDLED_RLOTTIE)
    target_link_libraries(tests PRIVATE ${TDLIB_PURPLE_RLOTTIE_TARGET})
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

add_executable(td-transport-tests EXCLUDE_FROM_ALL
    td-transport-test-main.cpp
    td-transport-test.cpp
    td-polling-backend-test.cpp
    ${CMAKE_SOURCE_DIR}/td-polling-backend.cpp
    ${CMAKE_SOURCE_DIR}/td-transport.cpp
)
target_compile_features(td-transport-tests PRIVATE cxx_std_14)
target_include_directories(td-transport-tests PRIVATE ${CMAKE_SOURCE_DIR})
target_include_directories(
    td-transport-tests
    SYSTEM PRIVATE
        ${GLIB_INCLUDE_DIRS}
)
if (GLIB_LIBRARY_DIRS)
    target_link_directories(
        td-transport-tests
        PRIVATE
            ${GLIB_LIBRARY_DIRS}
    )
endif()
target_link_libraries(
    td-transport-tests
    PRIVATE
        gtest
        Td::TdStatic
        ${GLIB_LIBRARIES}
)
add_dependencies(tests td-transport-tests)

add_custom_target(
    run-tests
    COMMAND
        "${Python3_EXECUTABLE}"
        "-B"
        "${CMAKE_SOURCE_DIR}/credentials/test/test_generate_application_credentials.py"
    COMMAND
        "${CMAKE_CURRENT_BINARY_DIR}/td-transport-tests"
    COMMAND
        "${CMAKE_CURRENT_BINARY_DIR}/tests"
    DEPENDS td-transport-tests tests
    USES_TERMINAL
)
