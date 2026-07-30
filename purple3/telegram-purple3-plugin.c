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
#include <glib.h>
#include <glib/gi18n-lib.h>

#include <gplugin.h>
#include <gplugin-native.h>
#include <purple.h>

#include "telegram-purple3-connection.h"
#include "telegram-purple3-protocol.h"

#define TELEGRAM_TDLIB_PLUGIN_ID "telegram-tdlib"
#define TELEGRAM_TDLIB_LEGACY_SETTING_PHONE_NUMBER "phone-number"
#define TELEGRAM_TDLIB_LEGACY_SETTING_API_ID "api-id"
#define TELEGRAM_TDLIB_LEGACY_SETTING_API_HASH "api-hash"

static PurpleProtocol *telegram_tdlib_protocol = NULL;
static PurpleAccountManager *telegram_tdlib_account_manager = NULL;
static gulong telegram_tdlib_account_added_handler = 0;
static gulong telegram_tdlib_core_account_manager_handler = 0;

static gboolean
telegram_tdlib_remove_legacy_account_settings(PurpleAccount *account)
{
    static const char *const legacy_settings[] = {
        TELEGRAM_TDLIB_LEGACY_SETTING_PHONE_NUMBER,
        TELEGRAM_TDLIB_LEGACY_SETTING_API_ID,
        TELEGRAM_TDLIB_LEGACY_SETTING_API_HASH,
    };
    PurpleAccountSettings *empty_updates = NULL;
    PurpleAccountSettings *settings = NULL;
    gboolean removed = FALSE;

    g_return_val_if_fail(PURPLE_IS_ACCOUNT(account), FALSE);

    if (g_strcmp0(purple_account_get_protocol_id(account),
                  TELEGRAM_TDLIB_PLUGIN_ID) != 0)
    {
        return FALSE;
    }

    settings = purple_account_get_settings(account);
    for (guint index = 0; index < G_N_ELEMENTS(legacy_settings); index++) {
        if (purple_account_settings_remove_setting(
                settings, legacy_settings[index]))
        {
            removed = TRUE;
        }
    }

    if (removed) {
        /*
         * Removal changes the list model but does not emit settings-updated.
         * Emit it through the public update API so the account manager
         * persists the cleaned settings. This migration code never requests
         * the removed string values.
         */
        empty_updates = purple_account_settings_new();
        purple_account_settings_update_settings(settings, empty_updates);
        g_clear_object(&empty_updates);
    }

    return removed;
}

static void
telegram_tdlib_account_added_cb(
    G_GNUC_UNUSED PurpleAccountManager *manager,
    PurpleAccount *account,
    G_GNUC_UNUSED gpointer data)
{
    telegram_tdlib_remove_legacy_account_settings(account);
}

static void
telegram_tdlib_remove_existing_legacy_account_settings(
    PurpleAccountManager *manager)
{
    GListModel *accounts = NULL;
    guint n_items = 0;

    g_return_if_fail(PURPLE_IS_ACCOUNT_MANAGER(manager));

    accounts = G_LIST_MODEL(manager);
    n_items = g_list_model_get_n_items(accounts);
    for (guint index = 0; index < n_items; index++) {
        PurpleAccount *account = g_list_model_get_item(accounts, index);

        telegram_tdlib_remove_legacy_account_settings(account);
        g_clear_object(&account);
    }
}

static void
telegram_tdlib_stop_observing_account_manager(void)
{
    if (PURPLE_IS_ACCOUNT_MANAGER(telegram_tdlib_account_manager) &&
        telegram_tdlib_account_added_handler != 0)
    {
        g_signal_handler_disconnect(telegram_tdlib_account_manager,
                                    telegram_tdlib_account_added_handler);
    }

    telegram_tdlib_account_added_handler = 0;
    g_clear_object(&telegram_tdlib_account_manager);
}

static void
telegram_tdlib_observe_account_manager(PurpleCore *core)
{
    PurpleAccountManager *manager = NULL;

    g_return_if_fail(PURPLE_IS_CORE(core));

    manager = purple_core_get_account_manager(core);
    if (!PURPLE_IS_ACCOUNT_MANAGER(manager) ||
        manager == telegram_tdlib_account_manager)
    {
        return;
    }

    telegram_tdlib_stop_observing_account_manager();
    telegram_tdlib_account_manager = g_object_ref(manager);
    telegram_tdlib_account_added_handler = g_signal_connect(
        manager,
        "added",
        G_CALLBACK(telegram_tdlib_account_added_cb),
        NULL);
    telegram_tdlib_remove_existing_legacy_account_settings(manager);
}

static void
telegram_tdlib_core_account_manager_notify_cb(
    PurpleCore *core,
    G_GNUC_UNUSED GParamSpec *pspec,
    G_GNUC_UNUSED gpointer data)
{
    telegram_tdlib_observe_account_manager(core);
}

static GPluginPluginInfo *
telegram_tdlib_query(G_GNUC_UNUSED GError **error)
{
    const char *const authors[] = {
        "tdlib-purple contributors",
        NULL,
    };
    PurplePluginInfoFlags flags = PURPLE_PLUGIN_INFO_FLAGS_INTERNAL |
                                  PURPLE_PLUGIN_INFO_FLAGS_AUTO_LOAD;

    return purple_plugin_info_new(
        "id", TELEGRAM_TDLIB_PLUGIN_ID,
        "name", "Telegram (tdlib)",
        "authors", authors,
        "version", TDLIB_PURPLE_VERSION,
        "category", N_("Protocol"),
        "summary", N_("Telegram protocol plugin using TDLib"),
        "description", N_("Telegram protocol support using TDLib"),
        "website", "https://github.com/adrighem/tdlib-purple",
        "abi-version", PURPLE_ABI_VERSION,
        "flags", flags,
        NULL);
}

static gboolean
telegram_tdlib_load(GPluginPlugin *plugin, GError **error)
{
    PurpleCore *core = NULL;
    PurpleProtocolManager *manager = NULL;

    if (PURPLE_IS_PROTOCOL(telegram_tdlib_protocol) ||
        PURPLE_IS_ACCOUNT_MANAGER(telegram_tdlib_account_manager) ||
        telegram_tdlib_account_added_handler != 0 ||
        telegram_tdlib_core_account_manager_handler != 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "plugin was not cleaned up properly");
        return FALSE;
    }

    core = purple_core_get_default();
    if (!PURPLE_IS_CORE(core)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "libpurple core is not initialized");
        return FALSE;
    }

    manager = purple_core_get_protocol_manager(core);
    if (!PURPLE_IS_PROTOCOL_MANAGER(manager)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "libpurple protocol manager is not initialized");
        return FALSE;
    }

    telegram_tdlib_connection_register(GPLUGIN_NATIVE_PLUGIN(plugin));
    telegram_tdlib_protocol_register(GPLUGIN_NATIVE_PLUGIN(plugin));
    telegram_tdlib_protocol = telegram_tdlib_protocol_new();

    if (!purple_protocol_manager_add(manager, telegram_tdlib_protocol, error)) {
        g_clear_object(&telegram_tdlib_protocol);
        return FALSE;
    }

    telegram_tdlib_core_account_manager_handler = g_signal_connect(
        core,
        "notify::account-manager",
        G_CALLBACK(telegram_tdlib_core_account_manager_notify_cb),
        NULL);
    telegram_tdlib_observe_account_manager(core);

    return TRUE;
}

static gboolean
telegram_tdlib_unload(G_GNUC_UNUSED GPluginPlugin *plugin,
                      G_GNUC_UNUSED gboolean shutdown,
                      GError **error)
{
    PurpleCore *core = NULL;
    PurpleProtocolManager *manager = NULL;

    if (!PURPLE_IS_PROTOCOL(telegram_tdlib_protocol) ||
        telegram_tdlib_core_account_manager_handler == 0)
    {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "plugin was not set up properly");
        return FALSE;
    }

    core = purple_core_get_default();
    if (!PURPLE_IS_CORE(core)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "libpurple core is not initialized");
        return FALSE;
    }

    manager = purple_core_get_protocol_manager(core);
    if (!PURPLE_IS_PROTOCOL_MANAGER(manager)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "libpurple protocol manager is not initialized");
        return FALSE;
    }

    if (!purple_protocol_manager_remove(manager, telegram_tdlib_protocol,
                                        error)) {
        return FALSE;
    }

    g_clear_object(&telegram_tdlib_protocol);
    telegram_tdlib_stop_observing_account_manager();
    g_signal_handler_disconnect(core,
                                telegram_tdlib_core_account_manager_handler);
    telegram_tdlib_core_account_manager_handler = 0;

    return TRUE;
}

GPLUGIN_NATIVE_PLUGIN_DECLARE(telegram_tdlib)
