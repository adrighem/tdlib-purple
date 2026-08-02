/*
 * tdlib-purple - Unofficial Telegram protocol plugin for libpurple
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

#include <glib.h>

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include <gplugin.h>
#include <purple.h>

#include "telegram-application-credentials-state.h"

#define TELEGRAM_TDLIB_PLUGIN_ID "telegram-tdlib"
#define TELEGRAM_TDLIB_LEGACY_SETTING_PHONE_NUMBER "phone-number"
#define TELEGRAM_TDLIB_LEGACY_SETTING_API_ID "api-id"
#define TELEGRAM_TDLIB_LEGACY_SETTING_API_HASH "api-hash"
#define TELEGRAM_TDLIB_SETTING_ENABLE_SECRET_CHATS "enable-secret-chats"

typedef struct {
    GMainLoop *loop;
    GObject *source;
    GAsyncResult *result;
    guint callback_count;
    guint timeout_id;
    gboolean completed;
    gboolean timed_out;
} Purple3SmokeAsyncWait;

#if !TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE
typedef struct {
    GMainLoop *loop;
    gpointer connection_weak;
    guint timeout_id;
    guint connection_notify_count;
    gboolean saw_connection;
    gboolean saw_connecting;
    gboolean timed_out;
} Purple3SmokeAccountConnectWait;
#endif

struct _Purple3SmokeUi {
    PurpleUi parent;
};

#define PURPLE3_SMOKE_TYPE_UI (purple3_smoke_ui_get_type())
G_DECLARE_FINAL_TYPE(Purple3SmokeUi, purple3_smoke_ui, PURPLE3_SMOKE, UI,
                     PurpleUi)

G_DEFINE_FINAL_TYPE(Purple3SmokeUi, purple3_smoke_ui, PURPLE_TYPE_UI)

static gboolean
purple3_smoke_async_timeout_cb(gpointer data)
{
    Purple3SmokeAsyncWait *wait = data;

    wait->timeout_id = 0;
    wait->timed_out = TRUE;
    g_main_loop_quit(wait->loop);

    return G_SOURCE_REMOVE;
}

static gboolean
purple3_smoke_async_quiescence_cb(gpointer data)
{
    Purple3SmokeAsyncWait *wait = data;

    wait->timeout_id = 0;
    g_main_loop_quit(wait->loop);

    return G_SOURCE_REMOVE;
}

#if !TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE
static gboolean
purple3_smoke_account_connect_timeout_cb(gpointer data)
{
    Purple3SmokeAccountConnectWait *wait = data;

    wait->timeout_id = 0;
    wait->timed_out = TRUE;
    g_main_loop_quit(wait->loop);

    return G_SOURCE_REMOVE;
}

static gboolean
purple3_smoke_account_connect_quiescence_cb(gpointer data)
{
    Purple3SmokeAccountConnectWait *wait = data;

    wait->timeout_id = 0;
    g_main_loop_quit(wait->loop);

    return G_SOURCE_REMOVE;
}
#endif

static void
purple3_smoke_async_cb(GObject *source, GAsyncResult *result, gpointer data)
{
    Purple3SmokeAsyncWait *wait = data;

    wait->callback_count++;
    if (!wait->completed) {
        wait->source = g_object_ref(source);
        wait->result = g_object_ref(result);
        wait->completed = TRUE;
    }

    if (g_main_loop_is_running(wait->loop)) {
        g_main_loop_quit(wait->loop);
    }
}

static Purple3SmokeAsyncWait *
purple3_smoke_async_wait_new(void)
{
    Purple3SmokeAsyncWait *wait = g_new0(Purple3SmokeAsyncWait, 1);

    wait->loop = g_main_loop_new(NULL, FALSE);

    return wait;
}

static void
purple3_smoke_async_wait_run(Purple3SmokeAsyncWait *wait)
{
    if (!wait->completed) {
        wait->timeout_id = g_timeout_add_seconds(
            2, purple3_smoke_async_timeout_cb, wait);
        g_main_loop_run(wait->loop);
    }

    if (wait->timeout_id != 0) {
        g_source_remove(wait->timeout_id);
        wait->timeout_id = 0;
    }

    g_assert_false(wait->timed_out);
    g_assert_true(wait->completed);

    /*
     * Keep the heap-backed callback data alive during a bounded quiescence
     * window to catch prompt duplicate callbacks from this immediate GTask
     * stub. Future long-lived operations need lifecycle-owned callback state.
     */
    wait->timeout_id = g_timeout_add(
        25, purple3_smoke_async_quiescence_cb, wait);
    g_main_loop_run(wait->loop);
    if (wait->timeout_id != 0) {
        g_source_remove(wait->timeout_id);
        wait->timeout_id = 0;
    }
    g_assert_cmpuint(wait->callback_count, ==, 1);
}

static void
purple3_smoke_async_wait_clear(Purple3SmokeAsyncWait *wait)
{
    g_assert_cmpuint(wait->timeout_id, ==, 0);

    g_clear_object(&wait->result);
    g_clear_object(&wait->source);
    g_clear_pointer(&wait->loop, g_main_loop_unref);
    g_free(wait);
}

static gboolean
purple3_smoke_ui_start(G_GNUC_UNUSED PurpleUi *ui,
                       G_GNUC_UNUSED GError **error)
{
    return TRUE;
}

static PurpleAccountManagerBackend *
purple3_smoke_ui_get_account_manager_backend(G_GNUC_UNUSED PurpleUi *ui,
                                              GError **error)
{
    return purple_account_manager_seagull_backend_new(":memory:", error);
}

static PurpleContactManagerBackend *
purple3_smoke_ui_get_contact_manager_backend(G_GNUC_UNUSED PurpleUi *ui,
                                              GError **error)
{
    return purple_contact_manager_seagull_backend_new(":memory:", error);
}

static PurpleConversationManagerBackend *
purple3_smoke_ui_get_conversation_manager_backend(G_GNUC_UNUSED PurpleUi *ui,
                                                   GError **error)
{
    return purple_conversation_manager_seagull_backend_new(":memory:", error);
}

static PurplePresenceManagerBackend *
purple3_smoke_ui_get_presence_manager_backend(G_GNUC_UNUSED PurpleUi *ui,
                                              GError **error)
{
    return purple_presence_manager_seagull_backend_new(":memory:", error);
}

static gpointer
purple3_smoke_ui_get_settings_backend(G_GNUC_UNUSED PurpleUi *ui)
{
    return g_memory_settings_backend_new();
}

static void
purple3_smoke_ui_init(G_GNUC_UNUSED Purple3SmokeUi *ui)
{
}

static void
purple3_smoke_ui_class_init(Purple3SmokeUiClass *klass)
{
    PurpleUiClass *ui_class = PURPLE_UI_CLASS(klass);

    ui_class->start = purple3_smoke_ui_start;
    ui_class->get_account_manager_backend =
        purple3_smoke_ui_get_account_manager_backend;
    ui_class->get_contact_manager_backend =
        purple3_smoke_ui_get_contact_manager_backend;
    ui_class->get_conversation_manager_backend =
        purple3_smoke_ui_get_conversation_manager_backend;
    ui_class->get_presence_manager_backend =
        purple3_smoke_ui_get_presence_manager_backend;
    ui_class->get_settings_backend = purple3_smoke_ui_get_settings_backend;
}

static PurpleCore *
purple3_smoke_core_start(void)
{
    GError *error = NULL;
    PurpleCore *core = NULL;
    PurpleUi *ui = NULL;

    ui = g_object_new(
        PURPLE3_SMOKE_TYPE_UI,
        "id", "tdlib-purple-smoke",
        "name", "tdlib-purple smoke test",
        "version", "1",
        "website", "https://github.com/adrighem/tdlib-purple",
        "support-website", "https://github.com/adrighem/tdlib-purple/issues",
        "client-type", "test",
        NULL);

    core = purple_core_new(ui, &error);
    g_clear_object(&ui);
    g_assert_no_error(error);
    g_assert_true(PURPLE_IS_CORE(core));

    purple_core_set_default(core);
    g_assert_true(purple_core_start(core, &error));
    g_assert_no_error(error);

    return core;
}

static PurpleAccountSetting *
purple3_smoke_find_setting(PurpleAccountSettings *settings, const char *id)
{
    GListModel *model = G_LIST_MODEL(settings);
    guint n_items = g_list_model_get_n_items(model);

    for (guint index = 0; index < n_items; index++) {
        PurpleAccountSetting *setting = g_list_model_get_item(model, index);

        if (g_strcmp0(purple_account_setting_get_id(setting), id) == 0) {
            return setting;
        }

        g_clear_object(&setting);
    }

    return NULL;
}

static void
purple3_smoke_assert_default_account_settings(PurpleProtocol *protocol)
{
    static const char *const absent_settings[] = {
        TELEGRAM_TDLIB_LEGACY_SETTING_PHONE_NUMBER,
        TELEGRAM_TDLIB_LEGACY_SETTING_API_ID,
        TELEGRAM_TDLIB_LEGACY_SETTING_API_HASH,
    };
    PurpleAccountSettings *settings = NULL;
    PurpleAccountSetting *setting = NULL;
    GListModel *model = NULL;

    settings = purple_protocol_get_default_account_settings(protocol);
    g_assert_true(PURPLE_IS_ACCOUNT_SETTINGS(settings));

    model = G_LIST_MODEL(settings);
    g_assert_cmpuint(g_list_model_get_n_items(model), ==, 1);

    setting = g_list_model_get_item(model, 0);
    g_assert_true(PURPLE_IS_ACCOUNT_SETTING_BOOLEAN(setting));
    g_assert_cmpstr(purple_account_setting_get_id(setting), ==,
                    TELEGRAM_TDLIB_SETTING_ENABLE_SECRET_CHATS);
    g_assert_cmpstr(
        purple_account_setting_get_label(setting), ==,
        "Enable secret chats (takes effect at reconnect)");
    g_assert_cmpint(purple_account_setting_get_weight(setting), ==, 10);
    g_assert_true(purple_account_setting_get_advanced(setting));
    g_assert_false(purple_account_setting_get_developer_mode(setting));
    g_assert_null(purple_account_setting_get_hint(setting));
    g_assert_true(purple_account_setting_boolean_get_value(
        PURPLE_ACCOUNT_SETTING_BOOLEAN(setting)));
    g_clear_object(&setting);

    for (guint index = 0; index < G_N_ELEMENTS(absent_settings); index++) {
        setting = purple3_smoke_find_setting(settings,
                                             absent_settings[index]);
        g_assert_null(setting);
    }

    g_clear_object(&settings);
}

static PurpleAccount *
purple3_smoke_account_new(void)
{
    PurpleAccountSettings *settings = NULL;
    PurpleAccountSetting *setting = NULL;
    PurpleAccount *account = NULL;

    account = purple_account_new("Telegram test account",
                                 TELEGRAM_TDLIB_PLUGIN_ID);
    settings = purple_account_get_settings(account);

    setting = purple_account_setting_boolean_new(
        TELEGRAM_TDLIB_SETTING_ENABLE_SECRET_CHATS, NULL, TRUE);
    g_assert_true(purple_account_settings_add_setting(settings, setting));

    return account;
}

static void
purple3_smoke_assert_account_validation(PurpleProtocol *protocol)
{
    PurpleAccount *account = NULL;
    GError *error = NULL;

    account = purple3_smoke_account_new();
    g_assert_true(purple_protocol_validate_account(protocol, account,
                                                   &error));
    g_assert_no_error(error);

    g_clear_object(&account);
}

static void
purple3_smoke_add_legacy_settings(PurpleAccount *account)
{
    PurpleAccountSettings *settings = NULL;
    PurpleAccountSetting *setting = NULL;

    g_assert_true(PURPLE_IS_ACCOUNT(account));

    settings = purple_account_get_settings(account);

    setting = purple_account_setting_string_new(
        TELEGRAM_TDLIB_LEGACY_SETTING_PHONE_NUMBER, NULL,
        "+15550000000");
    g_assert_true(purple_account_settings_add_setting(settings, setting));
    setting = purple_account_setting_string_new(
        TELEGRAM_TDLIB_LEGACY_SETTING_API_ID, NULL, "1");
    g_assert_true(purple_account_settings_add_setting(settings, setting));
    setting = purple_account_setting_string_new(
        TELEGRAM_TDLIB_LEGACY_SETTING_API_HASH, NULL,
        "synthetic-old-value");
    g_assert_true(purple_account_settings_add_setting(settings, setting));
}

static PurpleAccount *
purple3_smoke_legacy_account_new(void)
{
    PurpleAccount *account = NULL;

    account = purple3_smoke_account_new();
    purple3_smoke_add_legacy_settings(account);

    return account;
}

static void
purple3_smoke_assert_legacy_settings_removed(PurpleAccount *account)
{
    static const char *const legacy_settings[] = {
        TELEGRAM_TDLIB_LEGACY_SETTING_PHONE_NUMBER,
        TELEGRAM_TDLIB_LEGACY_SETTING_API_ID,
        TELEGRAM_TDLIB_LEGACY_SETTING_API_HASH,
    };
    PurpleAccountSettings *settings = NULL;
    PurpleAccountSetting *setting = NULL;

    settings = purple_account_get_settings(account);
    for (guint index = 0; index < G_N_ELEMENTS(legacy_settings); index++) {
        setting = purple3_smoke_find_setting(settings,
                                             legacy_settings[index]);
        g_assert_null(setting);
    }
    g_assert_true(purple_account_settings_get_boolean(
        settings, TELEGRAM_TDLIB_SETTING_ENABLE_SECRET_CHATS, FALSE));
}

static void
purple3_smoke_assert_legacy_settings_present(PurpleAccount *account)
{
    static const char *const legacy_settings[] = {
        TELEGRAM_TDLIB_LEGACY_SETTING_PHONE_NUMBER,
        TELEGRAM_TDLIB_LEGACY_SETTING_API_ID,
        TELEGRAM_TDLIB_LEGACY_SETTING_API_HASH,
    };
    PurpleAccountSettings *settings = NULL;
    PurpleAccountSetting *setting = NULL;

    settings = purple_account_get_settings(account);
    for (guint index = 0; index < G_N_ELEMENTS(legacy_settings); index++) {
        setting = purple3_smoke_find_setting(settings,
                                             legacy_settings[index]);
        g_assert_nonnull(setting);
        g_clear_object(&setting);
    }
}

static GListModel *
purple3_smoke_load_persisted_accounts(PurpleAccountManager *manager)
{
    Purple3SmokeAsyncWait *wait = NULL;
    PurpleAccountManagerBackend *backend = NULL;
    GListModel *accounts = NULL;
    GError *error = NULL;

    backend = purple_account_manager_get_backend(manager);
    g_assert_true(PURPLE_IS_ACCOUNT_MANAGER_BACKEND(backend));

    wait = purple3_smoke_async_wait_new();
    purple_account_manager_backend_load_accounts_async(
        backend, NULL, purple3_smoke_async_cb, wait);
    purple3_smoke_async_wait_run(wait);

    g_assert_true(wait->source == G_OBJECT(backend));
    accounts = purple_account_manager_backend_load_accounts_finish(
        backend, wait->result, &error);
    g_assert_no_error(error);
    g_assert_true(G_IS_LIST_MODEL(accounts));

    purple3_smoke_async_wait_clear(wait);

    return accounts;
}

static void
purple3_smoke_assert_migration_persisted(PurpleAccountManager *manager,
                                         PurpleAccount *account)
{
    GListModel *accounts = NULL;
    PurpleAccount *loaded = NULL;
    const char *account_id = NULL;
    guint n_items = 0;

    account_id = purple_account_get_id(account);
    g_assert_nonnull(account_id);

    accounts = purple3_smoke_load_persisted_accounts(manager);
    n_items = g_list_model_get_n_items(accounts);
    for (guint index = 0; index < n_items; index++) {
        PurpleAccount *candidate = g_list_model_get_item(accounts, index);

        if (g_strcmp0(purple_account_get_id(candidate), account_id) == 0) {
            loaded = candidate;
            break;
        }

        g_clear_object(&candidate);
    }

    g_assert_true(PURPLE_IS_ACCOUNT(loaded));
    purple3_smoke_assert_legacy_settings_removed(loaded);

    g_clear_object(&loaded);
    g_clear_object(&accounts);
}

static void
purple3_smoke_account_settings_updated_cb(
    G_GNUC_UNUSED PurpleAccount *account,
    G_GNUC_UNUSED PurpleAccountSettings *settings,
    gpointer data)
{
    guint *update_count = data;

    (*update_count)++;
}

static void
purple3_smoke_assert_added_account_migrated(PurpleCore *core)
{
    PurpleAccountManager *manager = NULL;
    PurpleAccount *account = NULL;
    gulong update_handler = 0;
    guint update_count = 0;

    manager = purple_core_get_account_manager(core);
    g_assert_true(PURPLE_IS_ACCOUNT_MANAGER(manager));

    account = purple3_smoke_legacy_account_new();
    g_assert_nonnull(purple_account_get_id(account));
    update_handler = g_signal_connect(
        account,
        "settings-updated",
        G_CALLBACK(purple3_smoke_account_settings_updated_cb),
        &update_count);
    purple_account_manager_add(manager, account);

    g_assert_cmpuint(update_count, ==, 1);
    purple3_smoke_assert_legacy_settings_removed(account);
    purple3_smoke_assert_migration_persisted(manager, account);

    g_signal_handler_disconnect(account, update_handler);
    purple_account_manager_remove(manager, account);
    g_clear_object(&account);
}

static void
purple3_smoke_assert_unrelated_account_not_migrated(PurpleCore *core)
{
    PurpleAccountManager *manager = NULL;
    PurpleAccount *account = NULL;
    gulong update_handler = 0;
    guint update_count = 0;

    manager = purple_core_get_account_manager(core);
    g_assert_true(PURPLE_IS_ACCOUNT_MANAGER(manager));

    account = purple_account_new("Unrelated test account",
                                 "unrelated-protocol");
    purple3_smoke_add_legacy_settings(account);
    g_assert_nonnull(purple_account_get_id(account));
    update_handler = g_signal_connect(
        account,
        "settings-updated",
        G_CALLBACK(purple3_smoke_account_settings_updated_cb),
        &update_count);
    purple_account_manager_add(manager, account);

    g_assert_cmpuint(update_count, ==, 0);
    purple3_smoke_assert_legacy_settings_present(account);

    g_signal_handler_disconnect(account, update_handler);
    purple_account_manager_remove(manager, account);
    g_clear_object(&account);
}

#if !TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE
static void
purple3_smoke_assert_connect_error(PurpleConnection *connection,
                                   GCancellable *cancellable,
                                   int expected_error_code)
{
    Purple3SmokeAsyncWait *wait = NULL;
    GError *error = NULL;
    gboolean success = FALSE;

    wait = purple3_smoke_async_wait_new();
    purple_connection_connect_async(connection, cancellable,
                                    purple3_smoke_async_cb, wait);
    purple3_smoke_async_wait_run(wait);

    g_assert_true(wait->source == G_OBJECT(connection));
    success = purple_connection_connect_finish(connection, wait->result,
                                               &error);
    g_assert_false(success);
    g_assert_error(error, G_IO_ERROR, expected_error_code);

    g_clear_error(&error);
    purple3_smoke_async_wait_clear(wait);
}
#endif

static void
purple3_smoke_assert_disconnect_succeeds(PurpleConnection *connection,
                                         GCancellable *cancellable)
{
    Purple3SmokeAsyncWait *wait = NULL;
    GError *error = NULL;
    gboolean success = FALSE;

    wait = purple3_smoke_async_wait_new();
    purple_connection_disconnect_async(connection, NULL, cancellable,
                                       purple3_smoke_async_cb, wait);
    purple3_smoke_async_wait_run(wait);

    g_assert_true(wait->source == G_OBJECT(connection));
    success = purple_connection_disconnect_finish(connection, wait->result,
                                                  &error);
    g_assert_no_error(error);
    g_assert_true(success);

    purple3_smoke_async_wait_clear(wait);
}

#if !TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE
static void
purple3_smoke_assert_connect_cancelled_after_start(
    PurpleConnection *connection,
    GCancellable *cancellable)
{
    Purple3SmokeAsyncWait *wait = NULL;
    GError *error = NULL;
    gboolean success = FALSE;

    wait = purple3_smoke_async_wait_new();
    purple_connection_connect_async(connection, cancellable,
                                    purple3_smoke_async_cb, wait);
    g_cancellable_cancel(cancellable);
    purple3_smoke_async_wait_run(wait);

    g_assert_true(wait->source == G_OBJECT(connection));
    success = purple_connection_connect_finish(connection, wait->result,
                                               &error);
    g_assert_false(success);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);

    g_clear_error(&error);
    purple3_smoke_async_wait_clear(wait);
}
#endif

static void
purple3_smoke_assert_connection_lifecycle(PurpleProtocol *protocol)
{
    PurpleAccount *account = NULL;
    PurpleConnection *connection = NULL;
    GCancellable *cancellable = NULL;
    GError *error = NULL;
    gpointer account_weak = NULL;
    gpointer cancellable_weak = NULL;
    gpointer connection_weak = NULL;
    gpointer connection_cancellable_weak = NULL;

    account = purple3_smoke_account_new();
    connection = purple_protocol_create_connection(protocol, account, &error);
    g_assert_no_error(error);
    g_assert_true(PURPLE_IS_CONNECTION(connection));
    g_assert_cmpstr(G_OBJECT_TYPE_NAME(connection), ==,
                    "TelegramTdlibConnection");
    g_assert_true(G_OBJECT_TYPE(connection) != PURPLE_TYPE_CONNECTION);
    g_assert_true(purple_connection_get_account(connection) == account);
    g_assert_true(G_IS_CANCELLABLE(
        purple_connection_get_cancellable(connection)));

    account_weak = account;
    g_object_add_weak_pointer(G_OBJECT(account), &account_weak);
    connection_weak = connection;
    g_object_add_weak_pointer(G_OBJECT(connection), &connection_weak);
    connection_cancellable_weak =
        purple_connection_get_cancellable(connection);
    g_object_add_weak_pointer(
        G_OBJECT(connection_cancellable_weak),
        &connection_cancellable_weak);

#if !TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE
    /*
     * A credentialed production module can start a real TDLib session. Keep
     * its smoke coverage to construction and disconnected cleanup; synthetic
     * fake-session tests exercise successful connection paths separately.
     */
    purple3_smoke_assert_connect_error(
        connection, NULL, G_IO_ERROR_NOT_INITIALIZED);
#endif
    purple3_smoke_assert_disconnect_succeeds(
        connection, NULL);

#if !TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE
    purple3_smoke_assert_connect_cancelled_after_start(
        connection, purple_connection_get_cancellable(connection));
#endif

    cancellable = g_cancellable_new();
    cancellable_weak = cancellable;
    g_object_add_weak_pointer(G_OBJECT(cancellable), &cancellable_weak);
    g_cancellable_cancel(cancellable);

#if !TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE
    purple3_smoke_assert_connect_error(
        connection, cancellable, G_IO_ERROR_CANCELLED);
#endif
    purple3_smoke_assert_disconnect_succeeds(connection, cancellable);
    purple3_smoke_assert_disconnect_succeeds(connection, cancellable);

    g_clear_object(&cancellable);
    g_assert_null(cancellable_weak);
    g_clear_object(&connection);
    g_assert_null(connection_weak);
    g_assert_null(connection_cancellable_weak);
    g_clear_object(&account);
    g_assert_null(account_weak);
}

#if !TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE
static void
purple3_smoke_account_connection_notify_cb(
    PurpleAccount *account,
    G_GNUC_UNUSED GParamSpec *pspec,
    gpointer data)
{
    Purple3SmokeAccountConnectWait *wait = data;
    PurpleConnection *connection =
        purple_account_get_connection(account);

    wait->connection_notify_count++;
    if (connection != NULL) {
        g_assert_false(wait->saw_connection);
        wait->saw_connection = TRUE;
        wait->connection_weak = connection;
        g_object_add_weak_pointer(
            G_OBJECT(connection), &wait->connection_weak);
    }
}

static void
purple3_smoke_account_state_notify_cb(
    PurpleAccount *account,
    G_GNUC_UNUSED GParamSpec *pspec,
    gpointer data)
{
    Purple3SmokeAccountConnectWait *wait = data;
    PurpleConnectionState state =
        purple_account_get_connection_state(account);

    if (state == PURPLE_CONNECTION_STATE_CONNECTING) {
        wait->saw_connecting = TRUE;
    } else if (
        state == PURPLE_CONNECTION_STATE_DISCONNECTED &&
        purple_account_get_error(account) != NULL)
    {
        g_main_loop_quit(wait->loop);
    }
}

static void
purple3_smoke_assert_account_connect_failure_releases_connection(void)
{
    Purple3SmokeAccountConnectWait wait = {0};
    PurpleAccount *account = NULL;
    GError *error = NULL;
    gulong connection_handler = 0;
    gulong state_handler = 0;
    gpointer account_weak = NULL;

    account = purple3_smoke_account_new();
    account_weak = account;
    g_object_add_weak_pointer(G_OBJECT(account), &account_weak);

    wait.loop = g_main_loop_new(NULL, FALSE);
    connection_handler = g_signal_connect(
        account,
        "notify::connection",
        G_CALLBACK(purple3_smoke_account_connection_notify_cb),
        &wait);
    state_handler = g_signal_connect(
        account,
        "notify::connection-state",
        G_CALLBACK(purple3_smoke_account_state_notify_cb),
        &wait);

    purple_account_set_enabled(account, TRUE);
    purple_account_connect(account);

    wait.timeout_id = g_timeout_add_seconds(
        2, purple3_smoke_account_connect_timeout_cb, &wait);
    g_main_loop_run(wait.loop);
    if (wait.timeout_id != 0) {
        g_source_remove(wait.timeout_id);
        wait.timeout_id = 0;
    }

    g_assert_false(wait.timed_out);
    g_assert_true(wait.saw_connecting);
    g_assert_true(wait.saw_connection);
    g_assert_cmpuint(wait.connection_notify_count, ==, 2);
    g_assert_cmpint(
        purple_account_get_connection_state(account),
        ==,
        PURPLE_CONNECTION_STATE_DISCONNECTED);
    g_assert_null(purple_account_get_connection(account));

    error = purple_account_get_error(account);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_INITIALIZED);

    wait.timeout_id = g_timeout_add(
        25, purple3_smoke_account_connect_quiescence_cb, &wait);
    g_main_loop_run(wait.loop);
    if (wait.timeout_id != 0) {
        g_source_remove(wait.timeout_id);
        wait.timeout_id = 0;
    }
    g_assert_null(wait.connection_weak);

    g_signal_handler_disconnect(account, connection_handler);
    g_signal_handler_disconnect(account, state_handler);
    g_clear_pointer(&wait.loop, g_main_loop_unref);
    g_clear_object(&account);
    g_assert_null(account_weak);
}
#endif

static void
test_plugin_load_and_unload(void)
{
    GError *error = NULL;
    GPluginManager *plugin_manager = NULL;
    GPluginPlugin *plugin = NULL;
    GPluginPluginInfo *plugin_info = NULL;
    PurpleCore *core = NULL;
    PurpleAccount *legacy_account = NULL;
    PurpleAccountManager *account_manager = NULL;
    PurpleProtocolManager *protocol_manager = NULL;
    PurpleProtocol *protocol = NULL;
    GBytes *icon = NULL;
    gulong legacy_update_handler = 0;
    guint legacy_update_count = 0;

    g_assert_nonnull(g_getenv("PURPLE_PLUGIN_PATH"));

    core = purple3_smoke_core_start();
    account_manager = purple_core_get_account_manager(core);
    g_assert_true(PURPLE_IS_ACCOUNT_MANAGER(account_manager));
    plugin_manager = gplugin_manager_get_default();

    plugin = gplugin_manager_find_plugin(plugin_manager,
                                         TELEGRAM_TDLIB_PLUGIN_ID);
    g_assert_true(GPLUGIN_IS_PLUGIN(plugin));
    g_assert_cmpint(gplugin_plugin_get_state(plugin), ==,
                    GPLUGIN_PLUGIN_STATE_LOADED);

    plugin_info = gplugin_plugin_get_info(plugin);
    g_assert_true(PURPLE_IS_PLUGIN_INFO(plugin_info));
    g_assert_cmpstr(gplugin_plugin_info_get_id(plugin_info), ==,
                    TELEGRAM_TDLIB_PLUGIN_ID);
    g_assert_cmpuint(gplugin_plugin_info_get_abi_version(plugin_info), ==,
                     PURPLE_ABI_VERSION);
    g_assert_true(
        (purple_plugin_info_get_flags(PURPLE_PLUGIN_INFO(plugin_info)) &
         (PURPLE_PLUGIN_INFO_FLAGS_INTERNAL |
          PURPLE_PLUGIN_INFO_FLAGS_AUTO_LOAD)) ==
        (PURPLE_PLUGIN_INFO_FLAGS_INTERNAL |
         PURPLE_PLUGIN_INFO_FLAGS_AUTO_LOAD));

    protocol_manager = purple_core_get_protocol_manager(core);
    protocol = purple_protocol_manager_find(protocol_manager,
                                            TELEGRAM_TDLIB_PLUGIN_ID);
    g_assert_true(PURPLE_IS_PROTOCOL(protocol));
    g_assert_cmpstr(purple_protocol_get_name(protocol), ==,
                    "Unofficial Telegram");
    g_assert_cmpstr(purple_protocol_get_icon_name(protocol), ==,
                    "im-telegram");
    g_assert_cmpstr(purple_protocol_get_icon_resource_path(protocol), ==,
                    "/im/tdlib-purple/protocols/telegram/icons");

    icon = g_resources_lookup_data(
        "/im/tdlib-purple/protocols/telegram/icons/16x16/apps/"
        "im-telegram.png",
        G_RESOURCE_LOOKUP_FLAGS_NONE,
        &error);
    g_assert_no_error(error);
    g_assert_nonnull(icon);
    g_clear_pointer(&icon, g_bytes_unref);

    purple3_smoke_assert_default_account_settings(protocol);
    purple3_smoke_assert_account_validation(protocol);
    purple3_smoke_assert_connection_lifecycle(protocol);
#if !TDLIB_PURPLE_APPLICATION_CREDENTIALS_AVAILABLE
    purple3_smoke_assert_account_connect_failure_releases_connection();
#endif
    purple3_smoke_assert_added_account_migrated(core);
    purple3_smoke_assert_unrelated_account_not_migrated(core);

    /*
     * The protocol owns no test account, connection, setting, cancellable, or
     * asynchronous result at this point. Drop the remaining metadata object
     * before exercising dynamic-type unload.
     */
    g_clear_object(&plugin_info);

    g_assert_true(gplugin_manager_unload_plugin(plugin_manager, plugin,
                                                &error));
    g_assert_no_error(error);
    g_assert_cmpint(gplugin_plugin_get_state(plugin), ==,
                    GPLUGIN_PLUGIN_STATE_QUERIED);
    g_assert_null(purple_protocol_manager_find(protocol_manager,
                                               TELEGRAM_TDLIB_PLUGIN_ID));

    legacy_account = purple3_smoke_legacy_account_new();
    g_assert_nonnull(purple_account_get_id(legacy_account));
    legacy_update_handler = g_signal_connect(
        legacy_account,
        "settings-updated",
        G_CALLBACK(purple3_smoke_account_settings_updated_cb),
        &legacy_update_count);
    purple_account_manager_add(account_manager, legacy_account);
    g_assert_cmpuint(legacy_update_count, ==, 0);
    purple3_smoke_assert_legacy_settings_present(legacy_account);

    g_assert_true(gplugin_manager_load_plugin(plugin_manager, plugin, &error));
    g_assert_no_error(error);
    g_assert_cmpuint(legacy_update_count, ==, 1);
    purple3_smoke_assert_legacy_settings_removed(legacy_account);
    purple3_smoke_assert_migration_persisted(account_manager,
                                             legacy_account);
    g_signal_handler_disconnect(legacy_account, legacy_update_handler);
    purple_account_manager_remove(account_manager, legacy_account);
    g_clear_object(&legacy_account);

    protocol = purple_protocol_manager_find(protocol_manager,
                                            TELEGRAM_TDLIB_PLUGIN_ID);
    g_assert_true(PURPLE_IS_PROTOCOL(protocol));
    purple3_smoke_assert_connection_lifecycle(protocol);

    g_assert_true(gplugin_manager_unload_plugin(plugin_manager, plugin,
                                                &error));
    g_assert_no_error(error);
    g_assert_cmpint(gplugin_plugin_get_state(plugin), ==,
                    GPLUGIN_PLUGIN_STATE_QUERIED);
    g_assert_null(purple_protocol_manager_find(protocol_manager,
                                               TELEGRAM_TDLIB_PLUGIN_ID));

    g_clear_object(&plugin);

    /*
     * In this Pidgin build, GPlugin 0.44 manager finalization tries to dispose
     * a dynamic GTypeModule whose use count is still nonzero, which GLib
     * rejects with a critical. Keep the manager alive until this short-lived
     * process exits. The assertions above cover plugin unload and protocol
     * removal; this workaround does not claim to cover manager finalization.
     */
    g_object_ref(plugin_manager);
    purple_core_quit(core);
    g_object_unref(core);
}

int
main(int argc, char *argv[])
{
    /*
     * Pidgin's development environment may expose optional GPlugin language
     * loaders. They are unrelated to this native C plugin and can register
     * duplicate introspection repositories in an isolated test process.
     */
    g_unsetenv("GPLUGIN_PLUGIN_PATH");

    g_test_init(&argc, &argv, NULL);

    g_test_add_func("/purple3/plugin/load-and-unload",
                    test_plugin_load_and_unload);

    return g_test_run();
}
