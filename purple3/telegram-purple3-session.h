/*
 * tdlib-purple - Telegram client for libpurple using TDLib
 * Copyright (C) tdlib-purple contributors
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 3 of the License, or (at your option)
 * any later version.
 */

#ifndef TELEGRAM_PURPLE3_SESSION_H
#define TELEGRAM_PURPLE3_SESSION_H

#include <gio/gio.h>
#include <glib.h>

#include <purple.h>

#include "telegram-application-credentials.h"

G_BEGIN_DECLS

typedef struct TelegramTdlibSession TelegramTdlibSession;

typedef enum {
    TELEGRAM_TDLIB_SESSION_FAILURE_CANCELLED,
    TELEGRAM_TDLIB_SESSION_FAILURE_AUTHORIZATION,
    TELEGRAM_TDLIB_SESSION_FAILURE_BACKEND,
} TelegramTdlibSessionFailure;

typedef enum {
    TELEGRAM_TDLIB_SESSION_CLOSE_CLOSED,
    TELEGRAM_TDLIB_SESSION_CLOSE_TIMED_OUT,
    TELEGRAM_TDLIB_SESSION_CLOSE_FAILED,
} TelegramTdlibSessionCloseResult;

typedef struct {
    void (*ready)(PurpleConnection *connection);
    void (*connect_failed)(PurpleConnection *connection,
                           TelegramTdlibSessionFailure failure);
    void (*runtime_failed)(PurpleConnection *connection);
    void (*closed)(PurpleConnection *connection,
                   TelegramTdlibSessionCloseResult result);
} TelegramTdlibSessionCallbacks;

G_GNUC_INTERNAL TelegramTdlibSession *telegram_tdlib_session_new(
    PurpleConnection *connection,
    const TdlibPurpleApplicationCredentials *credentials,
    GCancellable *connection_cancellable,
    const TelegramTdlibSessionCallbacks *callbacks,
    GError **error);

G_GNUC_INTERNAL gboolean telegram_tdlib_session_start(
    TelegramTdlibSession *session,
    GError **error);

/* These operations are idempotent and never throw across the C boundary. */
G_GNUC_INTERNAL void telegram_tdlib_session_cancel(
    TelegramTdlibSession *session);
G_GNUC_INTERNAL void telegram_tdlib_session_close(
    TelegramTdlibSession *session);
G_GNUC_INTERNAL gboolean telegram_tdlib_session_get_close_result(
    TelegramTdlibSession *session,
    TelegramTdlibSessionCloseResult *result);
G_GNUC_INTERNAL void telegram_tdlib_session_free(
    TelegramTdlibSession *session);

/* Application-wide TDLib diagnostics are disabled before any client starts. */
G_GNUC_INTERNAL gboolean telegram_tdlib_session_initialize_runtime(
    GError **error);

/* Cancels authorization prompts without forcing already-ready accounts out. */
G_GNUC_INTERNAL void telegram_tdlib_session_prepare_unload(void);

/* True while unloading could release code still owned by a session/backend. */
G_GNUC_INTERNAL gboolean telegram_tdlib_session_module_busy(void);

G_END_DECLS

#endif /* TELEGRAM_PURPLE3_SESSION_H */
