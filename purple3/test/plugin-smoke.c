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

#include <glib.h>

#define G_SETTINGS_ENABLE_BACKEND
#include <gio/gsettingsbackend.h>

#include <gplugin.h>
#include <purple.h>

#define TELEGRAM_TDLIB_PLUGIN_ID "telegram-tdlib"

struct _Purple3SmokeUi {
    PurpleUi parent;
};

#define PURPLE3_SMOKE_TYPE_UI (purple3_smoke_ui_get_type())
G_DECLARE_FINAL_TYPE(Purple3SmokeUi, purple3_smoke_ui, PURPLE3_SMOKE, UI,
                     PurpleUi)

G_DEFINE_FINAL_TYPE(Purple3SmokeUi, purple3_smoke_ui, PURPLE_TYPE_UI)

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

static void
test_plugin_load_and_unload(void)
{
    GError *error = NULL;
    GPluginManager *plugin_manager = NULL;
    GPluginPlugin *plugin = NULL;
    GPluginPluginInfo *plugin_info = NULL;
    PurpleCore *core = NULL;
    PurpleProtocolManager *protocol_manager = NULL;
    PurpleProtocol *protocol = NULL;
    PurpleAccountSettings *settings = NULL;
    PurpleAccount *account = NULL;
    PurpleConnection *connection = NULL;
    GBytes *icon = NULL;

    g_assert_nonnull(g_getenv("PURPLE_PLUGIN_PATH"));

    core = purple3_smoke_core_start();
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
                    "Telegram (tdlib)");
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
    g_bytes_unref(icon);

    settings = purple_protocol_get_default_account_settings(protocol);
    g_assert_true(PURPLE_IS_ACCOUNT_SETTINGS(settings));
    g_clear_object(&settings);

    account = purple_account_new("bootstrap", TELEGRAM_TDLIB_PLUGIN_ID);
    connection = purple_protocol_create_connection(protocol, account, &error);
    g_assert_null(connection);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error(&error);
    g_clear_object(&account);

    g_assert_true(gplugin_manager_unload_plugin(plugin_manager, plugin,
                                                &error));
    g_assert_no_error(error);
    g_assert_cmpint(gplugin_plugin_get_state(plugin), ==,
                    GPLUGIN_PLUGIN_STATE_QUERIED);
    g_assert_null(purple_protocol_manager_find(protocol_manager,
                                               TELEGRAM_TDLIB_PLUGIN_ID));

    g_assert_true(gplugin_manager_load_plugin(plugin_manager, plugin, &error));
    g_assert_no_error(error);
    g_assert_true(PURPLE_IS_PROTOCOL(
        purple_protocol_manager_find(protocol_manager,
                                     TELEGRAM_TDLIB_PLUGIN_ID)));

    g_assert_true(gplugin_manager_unload_plugin(plugin_manager, plugin,
                                                &error));
    g_assert_no_error(error);
    g_assert_cmpint(gplugin_plugin_get_state(plugin), ==,
                    GPLUGIN_PLUGIN_STATE_QUERIED);
    g_assert_null(purple_protocol_manager_find(protocol_manager,
                                               TELEGRAM_TDLIB_PLUGIN_ID));

    g_clear_object(&plugin_info);
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
