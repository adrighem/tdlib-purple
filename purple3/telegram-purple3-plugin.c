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

#include "telegram-purple3-protocol.h"

#define TELEGRAM_TDLIB_PLUGIN_ID "telegram-tdlib"

static PurpleProtocol *telegram_tdlib_protocol = NULL;

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

    if (PURPLE_IS_PROTOCOL(telegram_tdlib_protocol)) {
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

    telegram_tdlib_protocol_register(GPLUGIN_NATIVE_PLUGIN(plugin));
    telegram_tdlib_protocol = telegram_tdlib_protocol_new();

    if (!purple_protocol_manager_add(manager, telegram_tdlib_protocol, error)) {
        g_clear_object(&telegram_tdlib_protocol);
        return FALSE;
    }

    return TRUE;
}

static gboolean
telegram_tdlib_unload(G_GNUC_UNUSED GPluginPlugin *plugin,
                      G_GNUC_UNUSED gboolean shutdown,
                      GError **error)
{
    PurpleCore *core = NULL;
    PurpleProtocolManager *manager = NULL;

    if (!PURPLE_IS_PROTOCOL(telegram_tdlib_protocol)) {
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
    return TRUE;
}

GPLUGIN_NATIVE_PLUGIN_DECLARE(telegram_tdlib)
