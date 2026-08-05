#include "buildopt.h"
#include "fixture.h"
#include "libpurple-mock.h"
#include "purple2-qr-presenter.h"

#include <td/telegram/td_api.hpp>

#include <gtest/gtest.h>

#ifdef TDLIB_PURPLE_HAVE_PURPLE2_QR
#include <png.h>
#endif

#include <algorithm>
#include <cstddef>
#include <vector>

using namespace td::td_api;

namespace {

void wipeBytes(std::vector<unsigned char> &bytes)
{
    volatile unsigned char *data = bytes.empty() ? nullptr : bytes.data();
    for (std::size_t index = 0; index < bytes.size(); ++index)
        data[index] = 0;
    bytes.clear();
}

class Purple2QrPresenterTest : public CommTest {
};

TEST_F(Purple2QrPresenterTest, CapabilityGateRequiresIconAndCloseOperations)
{
    setPurpleRequestUiCapabilities(false, true, true);
    EXPECT_FALSE(Purple2QrPresenter::available());
    setPurpleRequestUiCapabilities(true, false, true);
    EXPECT_FALSE(Purple2QrPresenter::available());
    setPurpleRequestUiCapabilities(true, true, false);
    EXPECT_FALSE(Purple2QrPresenter::available());

    setPurpleRequestUiCapabilities(true, true, true);
#ifdef TDLIB_PURPLE_HAVE_PURPLE2_QR
    EXPECT_TRUE(Purple2QrPresenter::available());
#else
    EXPECT_FALSE(Purple2QrPresenter::available());
#endif
}

TEST_F(Purple2QrPresenterTest, TextUiUsesPhoneAuthentication)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2));

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitPhoneNumber>()));
    tgl.verifyRequest(setAuthenticationPhoneNumber(
        "+" + selfPhoneNumber, nullptr));
}

TEST_F(Purple2QrPresenterTest, UnsupportedPersistedQrStateFailsClosed)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2));

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitOtherDeviceConfirmation>(
            "tg://login?token=synthetic-persisted-state")));

    prpl.verifyEvents(ConnectionErrorEvent(
        connection,
        "QR authentication requires a graphical libpurple client"));
    tgl.verifyNoRequests();
}

#ifdef TDLIB_PURPLE_HAVE_PURPLE2_QR

TEST_F(Purple2QrPresenterTest, QrRequestErrorFallsBackToPhoneOnce)
{
    setPurpleRequestUiCapabilities(true, true, true);
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2));

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitPhoneNumber>()));
    tgl.verifyRequest(requestQrCodeAuthentication(
        std::vector<td::td_api::int53>()));
    tgl.reply(make_object<error>(400, "synthetic QR rejection"));
    tgl.verifyRequest(setAuthenticationPhoneNumber(
        "+" + selfPhoneNumber, nullptr));

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitPhoneNumber>()));
    tgl.verifyNoRequests();
}

TEST_F(Purple2QrPresenterTest, RendersBoundedPngWithQuietBorder)
{
    setPurpleRequestUiCapabilities(true, true, true);
    Purple2QrPresenter presenter(
        connection, account, [](TdAuthPromptId) {});

    ASSERT_EQ(
        Purple2QrPresentationResult::Presented,
        presenter.show(
            TdAuthPromptId(7),
            "tg://login?token=synthetic-render-check"));
    prpl.verifyEvents(
        RequestActionEvent(connection, account, nullptr, nullptr, 1));
    ASSERT_EQ(purpleRequestIconCount(), 1U);
    ASSERT_GT(purpleRequestIconSize(0), 0U);

    std::vector<unsigned char> encoded = purpleRequestIconCopy(0);
    png_image image = {};
    image.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_memory(
            &image, encoded.data(), encoded.size())) {
        wipeBytes(encoded);
        FAIL() << "QR PNG could not be decoded";
    }
    EXPECT_EQ(image.width, image.height);
    EXPECT_LE(image.width, 128U);
    image.format = PNG_FORMAT_GRAY;
    std::vector<unsigned char> pixels(PNG_IMAGE_SIZE(image));
    if (!png_image_finish_read(
            &image, nullptr, pixels.data(), 0, nullptr)) {
        png_image_free(&image);
        wipeBytes(pixels);
        wipeBytes(encoded);
        FAIL() << "QR PNG pixels could not be decoded";
    }

    bool borderIsWhite = image.width >= 8;
    for (png_uint_32 y = 0; borderIsWhite && y < image.height; ++y) {
        for (png_uint_32 x = 0; x < image.width; ++x) {
            if (x < 4 || y < 4 || x >= image.width - 4 ||
                y >= image.height - 4) {
                borderIsWhite =
                    pixels[static_cast<std::size_t>(y) * image.width + x] ==
                    0xff;
                if (!borderIsWhite)
                    break;
            }
        }
    }
    EXPECT_TRUE(borderIsWhite);
    png_image_free(&image);
    wipeBytes(pixels);
    wipeBytes(encoded);
}

TEST_F(Purple2QrPresenterTest, RotationRejectsStaleCancelCallback)
{
    setPurpleRequestUiCapabilities(true, true, true);
    unsigned cancellations = 0;
    TdAuthPromptId cancelledPrompt;
    Purple2QrPresenter presenter(
        connection,
        account,
        [&](TdAuthPromptId prompt) {
            ++cancellations;
            cancelledPrompt = prompt;
        });
    const TdAuthPromptId prompt(11);

    ASSERT_EQ(
        Purple2QrPresentationResult::Presented,
        presenter.show(prompt, "tg://login?token=synthetic-first"));
    prpl.verifyEvents(
        RequestActionEvent(connection, account, nullptr, nullptr, 1));
    ASSERT_EQ(
        Purple2QrPresentationResult::Presented,
        presenter.show(prompt, "tg://login?token=synthetic-second"));
    prpl.verifyEvents(
        RequestActionEvent(connection, account, nullptr, nullptr, 1));

    EXPECT_TRUE(purpleRequestIconClosed(0));
    invokePurpleRequestIconAction(0);
    EXPECT_EQ(cancellations, 0U);
    invokePurpleRequestIconAction(1);
    EXPECT_EQ(cancellations, 1U);
    EXPECT_EQ(cancelledPrompt.value(), prompt.value());
}

TEST_F(Purple2QrPresenterTest, NullUiHandleFailsWithoutCancellation)
{
    setPurpleRequestUiCapabilities(true, true, true);
    setPurpleRequestIconHandleAvailable(false);
    unsigned cancellations = 0;
    Purple2QrPresenter presenter(
        connection,
        account,
        [&](TdAuthPromptId) { ++cancellations; });

    EXPECT_EQ(
        Purple2QrPresentationResult::Failed,
        presenter.show(
            TdAuthPromptId(13),
            "tg://login?token=synthetic-null-handle"));
    prpl.verifyEvents(
        RequestActionEvent(connection, account, nullptr, nullptr, 1));
    EXPECT_EQ(cancellations, 0U);
}

#endif

} // namespace
