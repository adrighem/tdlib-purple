#ifndef TELEGRAM_PURPLE3_SESSION_PRIVATE_H
#define TELEGRAM_PURPLE3_SESSION_PRIVATE_H

#include "telegram-purple3-session.h"

#include "td-polling-backend.h"

#include <string>

struct TelegramTdlibSessionDependencies {
    TdPollingBackend::ClientFactory clientFactory;
    TdPollingBackend::CloseTimeoutSourceFactory closeTimeoutSourceFactory;
    unsigned closeTimeoutSeconds = 10;
    double pollTimeoutSeconds = 0.1;
    std::string dataRoot;
    PurpleUi *ui = nullptr;
};

/* Test seam. Production uses telegram_tdlib_session_new(). */
TelegramTdlibSession *telegramTdlibSessionCreate(
    PurpleConnection *connection,
    const TdlibPurpleApplicationCredentials &credentials,
    GCancellable *connectionCancellable,
    const TelegramTdlibSessionCallbacks &callbacks,
    TelegramTdlibSessionDependencies dependencies,
    GError **error) noexcept;

#endif /* TELEGRAM_PURPLE3_SESSION_PRIVATE_H */
