#ifndef TELEGRAM_PURPLE3_CONNECTION_PRIVATE_H
#define TELEGRAM_PURPLE3_CONNECTION_PRIVATE_H

#include "telegram-purple3-connection.h"
#include "telegram-purple3-session.h"

G_BEGIN_DECLS

typedef TelegramTdlibSession *(*TelegramTdlibSessionFactory)(
    PurpleConnection *connection,
    const TdlibPurpleApplicationCredentials *credentials,
    GCancellable *connection_cancellable,
    const TelegramTdlibSessionCallbacks *callbacks,
    gpointer data,
    GError **error);

typedef void (*TelegramTdlibReauthorizationConnect)(
    PurpleAccount *account,
    gpointer data);
typedef gboolean (*TelegramTdlibReauthorizationReady)(
    PurpleAccount *account,
    gpointer data);

/* Test-only instance seam. Must be installed before connect_async(). */
G_GNUC_INTERNAL gboolean
telegram_tdlib_connection_set_session_factory_for_test(
    TelegramTdlibConnection *connection,
    TelegramTdlibSessionFactory factory,
    gpointer data,
    GDestroyNotify destroy);

G_GNUC_INTERNAL gboolean
telegram_tdlib_connection_set_reauthorization_connect_for_test(
    TelegramTdlibConnection *connection,
    TelegramTdlibReauthorizationConnect connect,
    gpointer data,
    GDestroyNotify destroy);

G_GNUC_INTERNAL gboolean
telegram_tdlib_connection_set_reauthorization_ready_for_test(
    TelegramTdlibConnection *connection,
    TelegramTdlibReauthorizationReady ready);

/* Test-only registration seam for a controlled GTypeModule. */
G_GNUC_INTERNAL void telegram_tdlib_connection_register_module_for_test(
    GTypeModule *module);

G_END_DECLS

#endif /* TELEGRAM_PURPLE3_CONNECTION_PRIVATE_H */
