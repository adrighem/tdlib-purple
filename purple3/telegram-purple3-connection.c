/*
 * tdlib-purple - Telegram client for libpurple using TDLib
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
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

#include "telegram-purple3-application-credentials.h"
#include "telegram-purple3-connection.h"
#include "telegram-purple3-connection-private.h"
#include "telegram-purple3-session.h"

struct _TelegramTdlibConnection {
    PurpleConnection parent;
    TelegramTdlibSession *session;
    GTask *connect_task;
    GPtrArray *disconnect_tasks;
    TelegramTdlibSessionCloseResult close_result;
    gboolean close_result_known;
    TelegramTdlibSessionFactory session_factory;
    gpointer session_factory_data;
    GDestroyNotify session_factory_destroy;
    TelegramTdlibReauthorizationConnect reauthorization_connect;
    TelegramTdlibReauthorizationReady reauthorization_ready;
    gpointer reauthorization_connect_data;
    GDestroyNotify reauthorization_connect_destroy;
    gboolean preserve_runtime_error_for_disconnect;
    gboolean recovery_disconnect;
    gboolean wait_for_connect_completion;
    GSource *reauthorization_source;
    gint64 reauthorization_deadline;
    GMainContext *owner_context;
};

static int telegram_tdlib_connection_connect_tag;
static int telegram_tdlib_connection_disconnect_tag;
static const guint telegram_tdlib_reauthorization_poll_milliseconds = 5;
static const gint64 telegram_tdlib_reauthorization_timeout =
    5 * G_TIME_SPAN_SECOND;

static GQuark
telegram_tdlib_reauthorization_quark(void)
{
    return g_quark_from_static_string(
        "telegram-tdlib-reauthorization-probe");
}

static gboolean
telegram_tdlib_account_has_reauthorization_probe(PurpleAccount *account)
{
    return PURPLE_IS_ACCOUNT(account) &&
           g_object_get_qdata(
               G_OBJECT(account),
               telegram_tdlib_reauthorization_quark()) != NULL;
}

static void
telegram_tdlib_account_set_reauthorization_probe(PurpleAccount *account,
                                                  gboolean active)
{
    if (!PURPLE_IS_ACCOUNT(account)) {
        return;
    }
    g_object_set_qdata(
        G_OBJECT(account),
        telegram_tdlib_reauthorization_quark(),
        active ? GINT_TO_POINTER(TRUE) : NULL);
}

G_DEFINE_DYNAMIC_TYPE_EXTENDED(
    TelegramTdlibConnection,
    telegram_tdlib_connection,
    PURPLE_TYPE_CONNECTION,
    G_TYPE_FLAG_FINAL,
    {})

static TelegramTdlibSession *
telegram_tdlib_connection_default_session_factory(
    PurpleConnection *connection,
    const TdlibPurpleApplicationCredentials *credentials,
    GCancellable *connection_cancellable,
    const TelegramTdlibSessionCallbacks *callbacks,
    G_GNUC_UNUSED gpointer data,
    GError **error)
{
    return telegram_tdlib_session_new(
        connection,
        credentials,
        connection_cancellable,
        callbacks,
        error);
}

static void
telegram_tdlib_connection_complete_connect_error(
    TelegramTdlibConnection *self,
    GError *error)
{
    GTask *task = g_steal_pointer(&self->connect_task);

    if (task == NULL) {
        g_clear_error(&error);
        return;
    }

    if (error == NULL) {
        error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            _("Telegram connection failed."));
    }

    g_task_return_error(task, error);
    g_object_unref(task);
}

static void
telegram_tdlib_connection_abort_reauthorization(
    TelegramTdlibConnection *self,
    PurpleAccount *account,
    GIOErrorEnum code,
    const char *message,
    gboolean report_account_error)
{
    GError *error = g_error_new_literal(G_IO_ERROR, code, message);

    telegram_tdlib_account_set_reauthorization_probe(account, FALSE);
    g_clear_pointer(&self->reauthorization_source, g_source_unref);
    if (self->connect_task != NULL) {
        telegram_tdlib_connection_complete_connect_error(self, error);
        error = NULL;
    } else if (report_account_error && PURPLE_IS_ACCOUNT(account)) {
        purple_account_set_error(account, error);
    }
    g_clear_error(&error);

    if (PURPLE_IS_ACCOUNT(account) &&
        purple_account_get_connection(account) == PURPLE_CONNECTION(self) &&
        !purple_account_get_disconnecting(account) &&
        !purple_account_get_disconnected(account)) {
        purple_account_disconnect(account, NULL);
    }
}

static gboolean
telegram_tdlib_connection_finish_reauthorization(gpointer data)
{
    TelegramTdlibConnection *self = TELEGRAM_TDLIB_CONNECTION(data);
    PurpleConnection *connection = PURPLE_CONNECTION(self);
    PurpleAccount *account = purple_connection_get_account(connection);

    if (!PURPLE_IS_ACCOUNT(account) ||
        !telegram_tdlib_account_has_reauthorization_probe(account) ||
        !purple_account_get_enabled(account))
    {
        telegram_tdlib_connection_abort_reauthorization(
            self,
            account,
            G_IO_ERROR_CANCELLED,
            _("Telegram authorization restart was cancelled."),
            FALSE);
        return G_SOURCE_REMOVE;
    }

    if (g_get_monotonic_time() >= self->reauthorization_deadline) {
        telegram_tdlib_connection_abort_reauthorization(
            self,
            account,
            G_IO_ERROR_TIMED_OUT,
            _("Telegram authorization restart timed out."),
            TRUE);
        return G_SOURCE_REMOVE;
    }

    const gboolean ready = self->reauthorization_ready != NULL
        ? self->reauthorization_ready(
              account, self->reauthorization_connect_data)
        : purple_account_get_disconnected(account) &&
              purple_account_get_connection(account) == NULL;
    if (!ready) {
        return G_SOURCE_CONTINUE;
    }

    if (self->connect_task != NULL) {
        telegram_tdlib_connection_complete_connect_error(
            self,
            g_error_new_literal(
                G_IO_ERROR,
                G_IO_ERROR_CANCELLED,
                _("Telegram authorization is restarting.")));
        self->wait_for_connect_completion = TRUE;
        return G_SOURCE_CONTINUE;
    }

    if (self->wait_for_connect_completion) {
        self->wait_for_connect_completion = FALSE;
        return G_SOURCE_CONTINUE;
    }

    g_clear_pointer(&self->reauthorization_source, g_source_unref);
    if (self->reauthorization_connect != NULL) {
        self->reauthorization_connect(
            account, self->reauthorization_connect_data);
    } else {
        purple_account_connect(account);
    }
    return G_SOURCE_REMOVE;
}

static void
telegram_tdlib_connection_session_ready(PurpleConnection *connection)
{
    TelegramTdlibConnection *self =
        TELEGRAM_TDLIB_CONNECTION(connection);
    PurpleAccount *account = NULL;
    GTask *task = NULL;

    task = g_steal_pointer(&self->connect_task);
    if (task == NULL) {
        return;
    }

    account = purple_connection_get_account(connection);
    if (!PURPLE_IS_ACCOUNT(account) ||
        purple_account_get_connection(account) != connection)
    {
        g_task_return_new_error_literal(
            task,
            G_IO_ERROR,
            G_IO_ERROR_CANCELLED,
            _("Telegram connection was cancelled."));
    } else {
        telegram_tdlib_account_set_reauthorization_probe(account, FALSE);
        purple_account_ready(account);
        g_task_return_boolean(task, TRUE);
    }
    g_object_unref(task);
}

static void
telegram_tdlib_connection_session_connect_failed(
    PurpleConnection *connection,
    TelegramTdlibSessionFailure failure)
{
    TelegramTdlibConnection *self =
        TELEGRAM_TDLIB_CONNECTION(connection);
    GError *error = NULL;

    switch (failure) {
    case TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED:
        error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_CANCELLED,
            _("Telegram connection was cancelled."));
        break;
    case TELEGRAM_TDLIB_SESSION_FAILURE_AUTHORIZATION:
        error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_PERMISSION_DENIED,
            _("Telegram authorization failed."));
        break;
    case TELEGRAM_TDLIB_SESSION_FAILURE_BACKEND:
        error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            _("Telegram connection failed."));
        break;
    }

    if (error == NULL) {
        error = g_error_new_literal(
            G_IO_ERROR,
            G_IO_ERROR_FAILED,
            _("Telegram connection failed."));
    }
    telegram_tdlib_connection_complete_connect_error(self, error);
}

static void
telegram_tdlib_connection_session_runtime_failed(
    PurpleConnection *connection)
{
    TelegramTdlibConnection *self =
        TELEGRAM_TDLIB_CONNECTION(connection);
    PurpleAccount *account = purple_connection_get_account(connection);

    if (!PURPLE_IS_ACCOUNT(account) ||
        purple_account_get_connection(account) != connection ||
        purple_account_get_disconnecting(account) ||
        purple_account_get_disconnected(account))
    {
        return;
    }

    /*
     * Purple stores this primary error after starting disconnect, then
     * replaces it if disconnect_finish() reports a cleanup error. Mark only
     * the task created by this reentrant call so that the primary error wins.
     */
    self->preserve_runtime_error_for_disconnect = TRUE;
    purple_account_disconnect_with_new_error(
        account,
        NULL,
        G_IO_ERROR,
        G_IO_ERROR_FAILED,
        "%s",
        _("The Telegram connection stopped unexpectedly."));
    self->preserve_runtime_error_for_disconnect = FALSE;
}

static void
telegram_tdlib_connection_session_reauthorization_required(
    PurpleConnection *connection)
{
    TelegramTdlibConnection *self =
        TELEGRAM_TDLIB_CONNECTION(connection);
    PurpleAccount *account = purple_connection_get_account(connection);

    if (!PURPLE_IS_ACCOUNT(account) ||
        purple_account_get_connection(account) != connection) {
        telegram_tdlib_connection_abort_reauthorization(
            self,
            account,
            G_IO_ERROR_CANCELLED,
            _("Telegram authorization restart was cancelled."),
            FALSE);
        return;
    }
    if (!purple_account_get_enabled(account) ||
        purple_account_get_disconnecting(account) ||
        purple_account_get_disconnected(account)) {
        telegram_tdlib_connection_abort_reauthorization(
            self,
            account,
            G_IO_ERROR_CANCELLED,
            _("Telegram authorization restart was cancelled."),
            FALSE);
        return;
    }

    if (telegram_tdlib_account_has_reauthorization_probe(account)) {
        telegram_tdlib_account_set_reauthorization_probe(account, FALSE);
        if (self->connect_task != NULL) {
            telegram_tdlib_connection_complete_connect_error(
                self,
                g_error_new_literal(
                    G_IO_ERROR,
                    G_IO_ERROR_PERMISSION_DENIED,
                    _("Telegram requested authorization again repeatedly.")));
        } else {
            telegram_tdlib_connection_session_runtime_failed(connection);
        }
        return;
    }

    telegram_tdlib_account_set_reauthorization_probe(account, TRUE);
    self->recovery_disconnect = TRUE;
    self->reauthorization_deadline =
        g_get_monotonic_time() + telegram_tdlib_reauthorization_timeout;
    self->wait_for_connect_completion = FALSE;
    self->reauthorization_source = g_timeout_source_new(
        telegram_tdlib_reauthorization_poll_milliseconds);
    g_source_set_priority(
        self->reauthorization_source, G_PRIORITY_DEFAULT_IDLE);
    g_source_set_callback(
        self->reauthorization_source,
        telegram_tdlib_connection_finish_reauthorization,
        g_object_ref(self),
        g_object_unref);
    if (g_source_attach(
            self->reauthorization_source,
            self->owner_context) == 0) {
        g_source_destroy(self->reauthorization_source);
        g_clear_pointer(&self->reauthorization_source, g_source_unref);
        telegram_tdlib_connection_abort_reauthorization(
            self,
            account,
            G_IO_ERROR_FAILED,
            _("Telegram authorization could not be restarted."),
            TRUE);
        return;
    }

    purple_account_disconnect(account, NULL);
}

static void
telegram_tdlib_connection_complete_disconnect_tasks(
    TelegramTdlibConnection *self,
    TelegramTdlibSessionCloseResult result)
{
    GPtrArray *tasks = g_steal_pointer(&self->disconnect_tasks);

    if (result != TELEGRAM_TDLIB_SESSION_CLOSE_CLOSED &&
        result != TELEGRAM_TDLIB_SESSION_CLOSE_TIMED_OUT &&
        result != TELEGRAM_TDLIB_SESSION_CLOSE_FAILED)
    {
        result = TELEGRAM_TDLIB_SESSION_CLOSE_FAILED;
    }

    self->disconnect_tasks = g_ptr_array_new_with_free_func(g_object_unref);
    self->close_result = result;
    self->close_result_known = TRUE;

    for (guint index = 0; index < tasks->len; index++) {
        GTask *task = g_ptr_array_index(tasks, index);
        const gboolean preserve_runtime_error =
            GPOINTER_TO_INT(g_task_get_task_data(task));

        if (preserve_runtime_error &&
            result != TELEGRAM_TDLIB_SESSION_CLOSE_CLOSED)
        {
            g_task_return_boolean(task, TRUE);
            continue;
        }

        switch (result) {
        case TELEGRAM_TDLIB_SESSION_CLOSE_CLOSED:
            g_task_return_boolean(task, TRUE);
            break;
        case TELEGRAM_TDLIB_SESSION_CLOSE_TIMED_OUT:
            g_task_return_new_error_literal(
                task,
                G_IO_ERROR,
                G_IO_ERROR_TIMED_OUT,
                _("Telegram cleanup is still finishing in the background."));
            break;
        case TELEGRAM_TDLIB_SESSION_CLOSE_FAILED:
            g_task_return_new_error_literal(
                task,
                G_IO_ERROR,
                G_IO_ERROR_FAILED,
                _("Telegram cleanup failed."));
            break;
        }
    }
    g_ptr_array_unref(tasks);
}

static void
telegram_tdlib_connection_session_closed(
    PurpleConnection *connection,
    TelegramTdlibSessionCloseResult result)
{
    telegram_tdlib_connection_complete_disconnect_tasks(
        TELEGRAM_TDLIB_CONNECTION(connection), result);
}

static void
telegram_tdlib_connection_connect_async(PurpleConnection *connection,
                                        GCancellable *cancellable,
                                        GAsyncReadyCallback callback,
                                        gpointer data)
{
    TdlibPurpleApplicationCredentials credentials = {0};
    GError *credentials_error = NULL;
    TelegramTdlibConnection *self =
        TELEGRAM_TDLIB_CONNECTION(connection);
    TelegramTdlibSessionFactory session_factory = NULL;
    static const TelegramTdlibSessionCallbacks session_callbacks = {
        telegram_tdlib_connection_session_ready,
        telegram_tdlib_connection_session_connect_failed,
        telegram_tdlib_connection_session_runtime_failed,
        telegram_tdlib_connection_session_reauthorization_required,
        telegram_tdlib_connection_session_closed,
    };
    PurpleAccount *account = purple_connection_get_account(connection);

    /*
     * Purple checks enabled before its asynchronous protocol capability check,
     * but it does not repeat that check before creating the connection. Close
     * that race, and reject connections superseded by account removal or a
     * newer connection instance, before creating any TDLib state.
     */
    if (!PURPLE_IS_ACCOUNT(account) ||
        !purple_account_get_enabled(account) ||
        purple_account_get_connection(account) != connection)
    {
        g_task_report_new_error(
            G_OBJECT(connection),
            callback,
            data,
            &telegram_tdlib_connection_connect_tag,
            G_IO_ERROR,
            G_IO_ERROR_CANCELLED,
            "%s",
            _("Telegram connection was cancelled."));
        return;
    }

    if (self->connect_task != NULL || self->session != NULL) {
        g_task_report_new_error(
            G_OBJECT(connection),
            callback,
            data,
            &telegram_tdlib_connection_connect_tag,
            G_IO_ERROR,
            G_IO_ERROR_PENDING,
            "%s",
            _("A Telegram connection is already active."));
        return;
    }

    self->connect_task = g_task_new(connection, cancellable, callback, data);
    g_task_set_source_tag(
        self->connect_task, &telegram_tdlib_connection_connect_tag);

    if (cancellable != NULL && g_cancellable_is_cancelled(cancellable)) {
        telegram_tdlib_connection_complete_connect_error(
            self,
            g_error_new_literal(
                G_IO_ERROR,
                G_IO_ERROR_CANCELLED,
                _("Telegram connection was cancelled.")));
        return;
    }

    if (!telegram_tdlib_copy_application_credentials(
            &credentials, &credentials_error))
    {
        telegram_tdlib_connection_complete_connect_error(
            self, credentials_error);
        return;
    }

    /*
     * Once a session can exist, its owner-context state machine selects the
     * winner between cancellation and Ready. Before that point, retain normal
     * GTask cancellation semantics for immediate configuration failures.
     */
    g_task_set_check_cancellable(self->connect_task, FALSE);

    session_factory = self->session_factory;
    if (session_factory == NULL) {
        session_factory =
            telegram_tdlib_connection_default_session_factory;
    }
    self->session = session_factory(
        connection,
        &credentials,
        cancellable,
        &session_callbacks,
        self->session_factory_data,
        &credentials_error);
    credentials = (TdlibPurpleApplicationCredentials){0};
    if (self->session == NULL) {
        telegram_tdlib_connection_complete_connect_error(
            self, credentials_error);
        return;
    }

    if (!telegram_tdlib_session_start(
            self->session, &credentials_error))
    {
        telegram_tdlib_session_free(g_steal_pointer(&self->session));
        telegram_tdlib_connection_complete_connect_error(
            self, credentials_error);
    }
}

static gboolean
telegram_tdlib_connection_connect_finish(PurpleConnection *connection,
                                         GAsyncResult *result,
                                         GError **error)
{
    PurpleAccount *account = NULL;
    TelegramTdlibConnection *self =
        TELEGRAM_TDLIB_CONNECTION(connection);
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
        if (!self->recovery_disconnect) {
            telegram_tdlib_account_set_reauthorization_probe(
                account, FALSE);
        }
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
    TelegramTdlibConnection *self =
        TELEGRAM_TDLIB_CONNECTION(connection);
    GTask *task = NULL;

    if (!self->recovery_disconnect) {
        telegram_tdlib_account_set_reauthorization_probe(
            purple_connection_get_account(connection), FALSE);
    }

    task = g_task_new(connection, cancellable, callback, data);
    g_task_set_source_tag(
        task, &telegram_tdlib_connection_disconnect_tag);
    /* See telegram_tdlib_connection_session_runtime_failed(). */
    g_task_set_task_data(
        task,
        GINT_TO_POINTER(self->preserve_runtime_error_for_disconnect),
        NULL);

    /*
     * Disconnect is currently only local cleanup and must remain idempotent.
     * A cancelled connection lifetime must not prevent that cleanup.
     */
    g_task_set_check_cancellable(task, FALSE);
    g_ptr_array_add(self->disconnect_tasks, task);

    if (self->session == NULL) {
        telegram_tdlib_connection_complete_disconnect_tasks(
            self, TELEGRAM_TDLIB_SESSION_CLOSE_CLOSED);
        return;
    }

    if (self->close_result_known) {
        telegram_tdlib_connection_complete_disconnect_tasks(
            self, self->close_result);
        return;
    }

    if (telegram_tdlib_session_get_close_result(
            self->session, &self->close_result))
    {
        telegram_tdlib_connection_complete_disconnect_tasks(
            self, self->close_result);
        return;
    }

    telegram_tdlib_session_close(self->session);
    if (!self->close_result_known &&
        telegram_tdlib_session_get_close_result(
            self->session, &self->close_result))
    {
        telegram_tdlib_connection_complete_disconnect_tasks(
            self, self->close_result);
    }
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
    TelegramTdlibConnection *connection)
{
    connection->disconnect_tasks =
        g_ptr_array_new_with_free_func(g_object_unref);
    connection->owner_context = g_main_context_ref_thread_default();
}

static void
telegram_tdlib_connection_dispose(GObject *object)
{
    TelegramTdlibConnection *self =
        TELEGRAM_TDLIB_CONNECTION(object);

    if (self->reauthorization_source != NULL) {
        g_source_destroy(self->reauthorization_source);
        g_clear_pointer(&self->reauthorization_source, g_source_unref);
    }

    if (self->session != NULL) {
        telegram_tdlib_session_cancel(self->session);
        telegram_tdlib_session_free(g_steal_pointer(&self->session));
    }

    g_clear_object(&self->connect_task);

    G_OBJECT_CLASS(telegram_tdlib_connection_parent_class)->dispose(object);
}

static void
telegram_tdlib_connection_finalize(GObject *object)
{
    TelegramTdlibConnection *self =
        TELEGRAM_TDLIB_CONNECTION(object);

    g_clear_pointer(&self->disconnect_tasks, g_ptr_array_unref);
    if (self->session_factory_destroy != NULL) {
        self->session_factory_destroy(self->session_factory_data);
    }
    self->session_factory = NULL;
    self->session_factory_data = NULL;
    self->session_factory_destroy = NULL;
    if (self->reauthorization_connect_destroy != NULL) {
        self->reauthorization_connect_destroy(
            self->reauthorization_connect_data);
    }
    self->reauthorization_connect = NULL;
    self->reauthorization_ready = NULL;
    self->reauthorization_connect_data = NULL;
    self->reauthorization_connect_destroy = NULL;
    g_clear_pointer(&self->owner_context, g_main_context_unref);

    G_OBJECT_CLASS(telegram_tdlib_connection_parent_class)->finalize(object);
}

static void
telegram_tdlib_connection_class_finalize(
    G_GNUC_UNUSED TelegramTdlibConnectionClass *klass)
{
}

static void
telegram_tdlib_connection_class_init(TelegramTdlibConnectionClass *klass)
{
    GObjectClass *object_class = G_OBJECT_CLASS(klass);
    PurpleConnectionClass *connection_class =
        PURPLE_CONNECTION_CLASS(klass);

    object_class->dispose = telegram_tdlib_connection_dispose;
    object_class->finalize = telegram_tdlib_connection_finalize;
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

gboolean
telegram_tdlib_connection_set_session_factory_for_test(
    TelegramTdlibConnection *connection,
    TelegramTdlibSessionFactory factory,
    gpointer data,
    GDestroyNotify destroy)
{
    g_return_val_if_fail(TELEGRAM_TDLIB_IS_CONNECTION(connection), FALSE);
    g_return_val_if_fail(factory != NULL, FALSE);

    if (connection->session != NULL || connection->connect_task != NULL) {
        return FALSE;
    }
    if (connection->session_factory_destroy != NULL) {
        connection->session_factory_destroy(
            connection->session_factory_data);
    }
    connection->session_factory = factory;
    connection->session_factory_data = data;
    connection->session_factory_destroy = destroy;
    return TRUE;
}

gboolean
telegram_tdlib_connection_set_reauthorization_connect_for_test(
    TelegramTdlibConnection *connection,
    TelegramTdlibReauthorizationConnect connect,
    gpointer data,
    GDestroyNotify destroy)
{
    g_return_val_if_fail(
        TELEGRAM_TDLIB_IS_CONNECTION(connection), FALSE);
    g_return_val_if_fail(connect != NULL, FALSE);

    if (connection->session != NULL || connection->connect_task != NULL ||
        connection->reauthorization_connect != NULL) {
        return FALSE;
    }

    connection->reauthorization_connect = connect;
    connection->reauthorization_connect_data = data;
    connection->reauthorization_connect_destroy = destroy;
    return TRUE;
}

gboolean
telegram_tdlib_connection_set_reauthorization_ready_for_test(
    TelegramTdlibConnection *connection,
    TelegramTdlibReauthorizationReady ready)
{
    g_return_val_if_fail(
        TELEGRAM_TDLIB_IS_CONNECTION(connection), FALSE);
    g_return_val_if_fail(ready != NULL, FALSE);

    if (connection->session != NULL || connection->connect_task != NULL ||
        connection->reauthorization_ready != NULL ||
        connection->reauthorization_connect_data == NULL) {
        return FALSE;
    }

    connection->reauthorization_ready = ready;
    return TRUE;
}

void
telegram_tdlib_connection_register_module_for_test(GTypeModule *module)
{
    g_return_if_fail(G_IS_TYPE_MODULE(module));

    telegram_tdlib_connection_register_type(module);
}
