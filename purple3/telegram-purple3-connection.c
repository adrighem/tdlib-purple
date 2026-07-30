/*
 * tdlib-purple - Telegram protocol plugin for libpurple
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include <gio/gio.h>
#include <glib/gi18n-lib.h>

#include "telegram-purple3-connection.h"

struct _TelegramTdlibConnection {
    PurpleConnection parent;
};

static int telegram_tdlib_connection_connect_tag;
static int telegram_tdlib_connection_disconnect_tag;

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    TelegramTdlibConnection,
    telegram_tdlib_connection,
    PURPLE_TYPE_CONNECTION,
    G_TYPE_FLAG_FINAL,
    {})

static void
telegram_tdlib_connection_connect_async(PurpleConnection *connection,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer data)
{
    GTask *task = NULL;

    task = g_task_new(connection, cancellable, callback, data);
    g_task_set_source_tag(
        task, &telegram_tdlib_connection_connect_tag);

    if (!g_task_return_error_if_cancelled(task)) {
        g_task_return_new_error_literal(
            task,
            G_IO_ERROR,
            G_IO_ERROR_NOT_SUPPORTED,
            _("Telegram connectivity is not implemented in the Purple 3 "
              "adapter yet."));
    }

    g_clear_object(&task);
}

static gboolean
telegram_tdlib_connection_connect_finish(PurpleConnection *connection,
                                         GAsyncResult *result,
                                         GError **error)
{
    PurpleAccount *account = NULL;
    gboolean success = FALSE;

    g_return_val_if_fail(
        TELEGRAM_TDLIB_IS_CONNECTION(connection), FALSE);
    g_return_val_if_fail(
        g_task_is_valid(result, connection), FALSE);
    g_return_val_if_fail(
        g_async_result_is_tagged(
            result, &telegram_tdlib_connection_connect_tag),
        FALSE);

    success = g_task_propagate_boolean(G_TASK(result), error);
    if (!success) {
        /*
         * PurpleAccount and PurpleConnection own each other. Purple's failed
         * connect callback currently marks the account disconnected without
         * clearing its connection, so break the cycle while the GTask still
         * keeps this connection alive. Do not clear a newer connection.
         */
        account = purple_connection_get_account(connection);
        if (PURPLE_IS_ACCOUNT(account) &&
            purple_account_get_connection(account) == connection)
        {
            purple_account_set_connection(account, NULL);
        }
    }

    return success;
}

static void
telegram_tdlib_connection_disconnect_async(
    PurpleConnection *connection,
    G_GNUC_UNUSED const char *message,
    GCancellable *cancellable,
    GAsyncReadyCallback callback,
    gpointer data)
{
    GTask *task = NULL;

    task = g_task_new(connection, cancellable, callback, data);
    g_task_set_source_tag(
        task, &telegram_tdlib_connection_disconnect_tag);

    /*
     * Disconnect is currently only local cleanup and must remain idempotent.
     * A cancelled connection lifetime must not prevent that cleanup.
     */
    g_task_set_check_cancellable(task, FALSE);
    g_task_return_boolean(task, TRUE);

    g_clear_object(&task);
}

static gboolean
telegram_tdlib_connection_disconnect_finish(PurpleConnection *connection,
                                            GAsyncResult *result,
                                            GError **error)
{
    g_return_val_if_fail(
        TELEGRAM_TDLIB_IS_CONNECTION(connection), FALSE);
    g_return_val_if_fail(
        g_task_is_valid(result, connection), FALSE);
    g_return_val_if_fail(
        g_async_result_is_tagged(
            result, &telegram_tdlib_connection_disconnect_tag),
        FALSE);

    return g_task_propagate_boolean(G_TASK(result), error);
}

static void
telegram_tdlib_connection_init(
    G_GNUC_UNUSED TelegramTdlibConnection *connection)
{
}

static void
telegram_tdlib_connection_class_finalize(
    G_GNUC_UNUSED TelegramTdlibConnectionClass *klass)
{
}

static void
telegram_tdlib_connection_class_init(TelegramTdlibConnectionClass *klass)
{
    PurpleConnectionClass *connection_class =
        PURPLE_CONNECTION_CLASS(klass);

    connection_class->connect_async =
        telegram_tdlib_connection_connect_async;
    connection_class->connect_finish =
        telegram_tdlib_connection_connect_finish;
    connection_class->disconnect_async =
        telegram_tdlib_connection_disconnect_async;
    connection_class->disconnect_finish =
        telegram_tdlib_connection_disconnect_finish;
}

void
telegram_tdlib_connection_register(GPluginNativePlugin *plugin)
{
    telegram_tdlib_connection_register_type(G_TYPE_MODULE(plugin));
}
