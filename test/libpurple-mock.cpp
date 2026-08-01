#include "libpurple-mock.h"
#include "purple-events.h"
#include <purple.h>
#include <stdarg.h>
#include <vector>
#include <algorithm>
#include <gtest/gtest.h>

struct AccountInfo {
    PurpleAccount                     *account;
    std::vector<PurpleBuddy *>         buddies;
    std::vector<PurpleChat *>          chats;
    std::vector<PurpleConversation *>  conversations;
    std::map<std::string, std::string> stringsOptions;
};

std::vector<AccountInfo>  g_accounts;
PurplePlugin             *g_plugin;
static GList             *g_chatConversations = nullptr;

struct SignalConnection {
    void *instance;
    std::string signal;
    void *handle;
    PurpleCallback callback;
    void *data;
    gulong id;
};

static std::vector<SignalConnection> g_signalConnections;
static gulong g_nextSignalId = 1;
static char g_conversationsHandle;
static char g_blistHandle;
static std::map<PurpleCmdId, std::string> g_registeredCommands;
static PurpleCmdId g_nextCommandId = 1;
static unsigned g_commandRegistrationFailureCountdown = 0;

std::size_t registeredPurpleCommandCount()
{
    return g_registeredCommands.size();
}

bool purpleCommandRegistered(const std::string &command)
{
    return g_purpleEvents.hasCommand(command.c_str());
}

void failPurpleCommandRegistrationAfter(
    unsigned callsUntilFailure)
{
    g_commandRegistrationFailureCountdown =
        callsUntilFailure;
}

extern "C" {

#define EVENT(type, ...) g_purpleEvents.addEvent(std::make_unique<type>(__VA_ARGS__))

void purple_debug_misc(const char *category, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    printf("%s: ", category);
    vprintf(format, va);
    va_end(va);
}

void purple_debug_info(const char *category, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    printf("Info: %s: ", category);
    vprintf(format, va);
    va_end(va);
}

void purple_debug_warning(const char *category, const char *format, ...)
{
    va_list va;
    va_start(va, format);
    printf("Warning: %s: ", category);
    vprintf(format, va);
    va_end(va);
}

const char *purple_account_get_username(const PurpleAccount *account)
{
    return account->username;
}

const char *purple_account_get_alias(const PurpleAccount *account)
{
    return account->alias;
}

const gchar *purple_account_get_name_for_display(const PurpleAccount *account)
{
    return purple_account_get_alias(account) ? : purple_account_get_username(account);
}

PurpleConnection *purple_account_get_connection(const PurpleAccount *account)
{
    return account->gc;
}

gboolean purple_account_is_connected(const PurpleAccount *account)
{
    return PURPLE_CONNECTION_IS_CONNECTED(account->gc);
}

void purple_account_set_alias(PurpleAccount *account, const char *alias)
{
    free(account->alias);
    account->alias = strdup(alias);
    EVENT(AccountSetAliasEvent, account, alias);
}

PurpleAccount *purple_account_new(const char *username, const char *protocol_id)
{
    PurpleAccount *account = new PurpleAccount;
    account->username = strdup(username);
    account->alias = nullptr;
    account->proxy_info = NULL;
    account->gc = NULL;

    g_accounts.emplace_back();
    g_accounts.back().account = account;

    return account;
}

PurpleBlistNode root = {
    .type = PURPLE_BLIST_OTHER_NODE,
    .prev = NULL,
    .next = NULL,
    .parent = NULL,
    .child = NULL,
    .settings = NULL,
    .ui_data = NULL,
    .flags = (PurpleBlistNodeFlags)0
};

void purple_account_destroy(PurpleAccount *account)
{
    free(account->username);
    free(account->alias);

    auto it = std::find_if(g_accounts.begin(), g_accounts.end(),
                           [account](const AccountInfo &info) { return info.account == account; });
    ASSERT_FALSE(it == g_accounts.end()) << "Destroying unknown account";
    while (!it->buddies.empty())
        purple_blist_remove_buddy(it->buddies.back());
    while (!it->conversations.empty())
        purple_conversation_destroy(it->conversations.back());
    while (!it->chats.empty())
        purple_blist_remove_chat(it->chats.back());
    g_accounts.erase(it);
    if (g_accounts.empty()) {
        g_list_free(g_chatConversations);
        g_chatConversations = nullptr;
        ASSERT_EQ(nullptr, root.child) << "Blist nodes remain";
    }

    delete account;
}

const char *purple_account_get_protocol_id(const PurpleAccount *account)
{
    return "";
}

PurpleAccount *purple_accounts_find(const char *name, const char *protocol)
{
    auto it = std::find_if(g_accounts.begin(), g_accounts.end(),
                           [name](const AccountInfo &account) {
                               return !strcmp(account.account->username, name);
                           });
    if (it != g_accounts.end())
        return it->account;
    return NULL;
}

void purple_blist_add_account(PurpleAccount *account)
{
    EVENT(ShowAccountEvent, account);
}

static void addNode(PurpleBlistNode &node)
{
    node.next = root.child;
    node.prev = NULL;
    if (root.child)
        root.child->prev = &node;
    root.child = &node;
}

static void removeNode(PurpleBlistNode &node)
{
    PurpleBlistNode *found;
    for (found = root.child; found; found = found->next)
        if (found == &node)
            break;
    ASSERT_TRUE(found != NULL) << "Removing unknown blist node";
    if (node.prev)
        node.prev->next = node.next;
    if (node.next)
        node.next->prev = node.prev;
    if (&node == root.child)
        root.child = node.next;
}

void purple_blist_add_buddy(PurpleBuddy *buddy, PurpleContact *contact, PurpleGroup *group, PurpleBlistNode *node)
{
    ASSERT_EQ(NULL, node) << "Not supported";
    auto pAccount = std::find_if(g_accounts.begin(), g_accounts.end(),
                                 [buddy](const AccountInfo &info) { return info.account == buddy->account; });
    ASSERT_FALSE(pAccount == g_accounts.end()) << "Adding buddy with unknown account";

    ASSERT_TRUE(std::find_if(pAccount->buddies.begin(), pAccount->buddies.end(), 
                             [buddy](const PurpleBuddy *existing) {
                                 return !strcmp(existing->name, buddy->name);
                             }) == pAccount->buddies.end())
        << "Buddy already exists in this account";

    buddy->node.parent = group ? &group->node : NULL;
    addNode(buddy->node);
    pAccount->buddies.push_back(buddy);

    EVENT(AddBuddyEvent, buddy->name, purple_buddy_get_alias(buddy), buddy->account, contact, group, node);
}

void purple_blist_remove_account(PurpleAccount *account)
{
    // TODO add event
}

void purple_blist_remove_buddy(PurpleBuddy *buddy)
{
    auto pAccount = std::find_if(g_accounts.begin(), g_accounts.end(),
                                 [buddy](const AccountInfo &info) { return info.account == buddy->account; });
    ASSERT_FALSE(pAccount == g_accounts.end()) << "Removing buddy with unknown account";

    auto it = std::find(pAccount->buddies.begin(), pAccount->buddies.end(), buddy);
    ASSERT_FALSE(it == pAccount->buddies.end()) << "Removing unkown buddy";
    pAccount->buddies.erase(it);

    EVENT(RemoveBuddyEvent, buddy->account, buddy->name);
    removeNode(buddy->node);
    purple_buddy_destroy(buddy);
}

static gboolean
purple_strings_are_different(const char *one, const char *two)
{
        return !((one && two && strcmp(one, two) == 0) ||
                        ((one == NULL || *one == '\0') && (two == NULL || *two == '\0')));
}

void purple_blist_alias_buddy(PurpleBuddy *buddy, const char *alias)
{
    ASSERT_NE(nullptr, buddy);

    // Similar to real libpurple
    if (purple_strings_are_different(buddy->alias, alias)) {
        free(buddy->alias);
        buddy->alias = strdup(alias);
        EVENT(AliasBuddyEvent, buddy->name, alias);
    }
}

void purple_blist_server_alias_buddy(PurpleBuddy *buddy, const char *alias)
{
    ASSERT_NE(nullptr, buddy);

    if (purple_strings_are_different(buddy->server_alias, alias)) {
        free(buddy->server_alias);
        buddy->server_alias = alias ? strdup(alias) : NULL;
    }
}

static char *getChatName(const PurpleChat *chat)
{
    auto        pluginInfo  = (PurplePluginProtocolInfo *)g_plugin->info->extra_info;
    GList      *chatInfo    = (pluginInfo)->chat_info(chat->account->gc);
    const char *componentId = ((proto_chat_entry *)chatInfo->data)->identifier;
    char       *name        = (char *)g_hash_table_lookup(chat->components, componentId);

    g_list_free_full(chatInfo, g_free);
    return name;
}

void purple_blist_remove_chat(PurpleChat *chat)
{
    auto pAccount = std::find_if(g_accounts.begin(), g_accounts.end(),
                                 [chat](const AccountInfo &info) { return info.account == chat->account; });
    ASSERT_FALSE(pAccount == g_accounts.end()) << "Removing buddy with unknown account";

    auto it = std::find(pAccount->chats.begin(), pAccount->chats.end(), chat);
    ASSERT_FALSE(it == pAccount->chats.end()) << "Removing unkown chat";
    pAccount->chats.erase(it);

    const char *inviteLink = (const char *)g_hash_table_lookup(chat->components, (char *)"link");
    EVENT(RemoveChatEvent, getChatName(chat), inviteLink ? inviteLink : "");

    free(chat->alias);
    g_hash_table_destroy(chat->components);
    removeNode(chat->node);
    g_hash_table_destroy(chat->node.settings);
    delete chat;
}

const char *purple_buddy_get_alias_only(PurpleBuddy *buddy)
{
    return buddy->alias;
}

const char *purple_buddy_get_server_alias(PurpleBuddy *buddy)
{
    return buddy->server_alias;
}

const char *purple_buddy_get_alias(PurpleBuddy *buddy)
{
    return buddy->alias ? buddy->alias : (buddy->server_alias ? buddy->server_alias : buddy->name);
}

PurpleGroup *purple_buddy_get_group(PurpleBuddy *buddy)
{
    return reinterpret_cast<PurpleGroup *>(buddy->node.parent);
}

const char *purple_buddy_get_name(const PurpleBuddy *buddy)
{
    return buddy->name;
}

PurpleAccount *purple_buddy_get_account(const PurpleBuddy *buddy)
{
    return buddy->account;
}

static void newNode(PurpleBlistNode &node, PurpleBlistNodeType type)
{
    node.child = NULL;
    node.next = NULL;
    node.parent = NULL;
    node.prev = NULL;
    node.type = type;
    node.settings = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, g_free);
}

PurpleBuddy *purple_buddy_new(PurpleAccount *account, const char *name, const char *alias)
{
    PurpleBuddy *buddy = new PurpleBuddy;

    buddy->account = account;
    buddy->name = strdup(name);
    buddy->alias = alias ? strdup(alias) : NULL;
    buddy->server_alias = NULL;
    buddy->node.parent = NULL;
    newNode(buddy->node, PURPLE_BLIST_BUDDY_NODE);

    return buddy;
}

void purple_buddy_destroy(PurpleBuddy *buddy)
{
    free(buddy->name);
    free(buddy->alias);
    free(buddy->server_alias);
    g_hash_table_destroy(buddy->node.settings);
    delete buddy;
}

void
purple_buddy_icons_set_for_user(PurpleAccount *account, const char *username,
                                void *icon_data, size_t icon_len,
                                const char *checksum)
{
    EVENT(SetUserPhotoEvent, account, username, icon_data, icon_len);
}

PurpleStoredImage *
purple_buddy_icons_node_set_custom_icon(PurpleBlistNode *node,
                                        guchar *icon_data, size_t icon_len)
{
    EXPECT_TRUE(PURPLE_BLIST_NODE_IS_CHAT(node));
    EVENT(SetUserPhotoEvent, PURPLE_CHAT(node)->account, getChatName(PURPLE_CHAT(node)), icon_data, icon_len);
    return NULL;
}

PurpleChat *purple_chat_new(PurpleAccount *account, const char *alias, GHashTable *components)
{
    PurpleChat *chat = new PurpleChat;
    chat->account = account;
    chat->alias = strdup(alias);
    chat->components = components;
    newNode(chat->node, PURPLE_BLIST_CHAT_NODE);
    return chat;
}

void purple_blist_add_chat(PurpleChat *chat, PurpleGroup *group, PurpleBlistNode *node)
{
    ASSERT_EQ(NULL, node) << "Not supported";
    char *name = getChatName(chat);

    auto pAccount = std::find_if(g_accounts.begin(), g_accounts.end(),
                                 [chat](const AccountInfo &info) { return info.account == chat->account; });
    ASSERT_FALSE(pAccount == g_accounts.end()) << "Adding chat with unknown account";

    ASSERT_TRUE(std::find_if(pAccount->chats.begin(), pAccount->chats.end(), 
                             [name](const PurpleChat *existing) {
                                 return !strcmp(getChatName(existing), name);
                             }) == pAccount->chats.end())
        << "Chat already exists in this account";

    chat->node.parent = group ? &group->node : NULL;
    addNode(chat->node);
    pAccount->chats.push_back(chat);

    EVENT(AddChatEvent, name, chat->alias, chat->account, group, node);
}

PurpleChat *purple_blist_find_chat(PurpleAccount *account, const char *name)
{
    // real purple_blist_find_chat does this
    if (!purple_account_is_connected(account))
        return NULL;

    auto pAccount = std::find_if(g_accounts.begin(), g_accounts.end(),
                                 [account](const AccountInfo &info) { return info.account == account; });
    EXPECT_FALSE(pAccount == g_accounts.end()) << "Searching chat with unknown account";

    if (pAccount != g_accounts.end()) {
        auto it = std::find_if(pAccount->chats.begin(), pAccount->chats.end(),
                               [name](const PurpleChat *existing) {
                                   return !strcmp(getChatName(existing), name);
                               });
        if (it != pAccount->chats.end())
            return *it;
    }

    return NULL;
}

const char *purple_chat_get_name(PurpleChat *chat)
{
    if (chat->alias)
        return chat->alias;
    return getChatName(chat);
}

PurpleAccount *purple_chat_get_account(PurpleChat *chat)
{
    return chat->account;
}

void purple_blist_alias_chat(PurpleChat *chat, const char *alias)
{
    free(chat->alias);
    chat->alias = strdup(alias);
    EVENT(AliasChatEvent, getChatName(chat), alias);
}

void purple_connection_error(PurpleConnection *gc, const char *reason)
{
    EVENT(ConnectionErrorEvent, gc, reason);
}

PurpleAccount *purple_connection_get_account(const PurpleConnection *gc)
{
    return gc->account;
}

void *purple_connection_get_protocol_data(const PurpleConnection *connection)
{
    return connection->proto_data;
}

PurpleConnectionState purple_connection_get_state(const PurpleConnection *gc)
{
    return gc->state;
}

void purple_connection_set_protocol_data(PurpleConnection *connection, void *proto_data)
{
    connection->proto_data = proto_data;
}

void purple_connection_set_state(PurpleConnection *gc, PurpleConnectionState state)
{
    gc->state = state;
    EVENT(ConnectionSetStateEvent, gc, state);
}

void purple_connection_update_progress(PurpleConnection *gc, const char *text,
									 size_t step, size_t count)
{
    EVENT(ConnectionUpdateProgressEvent, gc, step, count);
}

static PurpleConversation *purple_conversation_new_impl(PurpleConversationType type,
										PurpleAccount *account,
										const char *name)
{
    auto pAccount = std::find_if(g_accounts.begin(), g_accounts.end(),
                                 [account](const AccountInfo &info) { return info.account == account; });
    EXPECT_FALSE(pAccount == g_accounts.end()) << "Adding conversation with unknown account";

    if (pAccount != g_accounts.end()) {
        PurpleConversation *conv = purple_find_conversation_with_account(type, name, account);
        if (conv) {
            if ((type == PURPLE_CONV_TYPE_CHAT) && purple_conv_chat_has_left(purple_conversation_get_chat_data(conv))) {
                // Rejoin, like real libpurple does
                purple_conversation_get_chat_data(conv)->left = FALSE;
                return conv;
            }
            EXPECT_TRUE(false) << "Conversation with this name already exists to this account";
        }
    }

    PurpleConversation *conv = new PurpleConversation;
    conv->type = type;
    conv->account = account;
    conv->name = strdup(name);
    conv->title = NULL;
    conv->ui_ops = NULL;
    if (conv->type == PURPLE_CONV_TYPE_IM) {
        conv->u.im = new PurpleConvIm;
        conv->u.im->conv = conv;
    }
    if (conv->type == PURPLE_CONV_TYPE_CHAT) {
        conv->u.chat = new PurpleConvChat;
        conv->u.chat->conv = conv;
        conv->u.chat->in_room = NULL;
        conv->u.chat->who = NULL;
        conv->u.chat->topic = NULL;
        conv->u.chat->left = FALSE;
        conv->u.chat->id = 0;
        conv->u.chat->users = g_hash_table_new_full(
            g_str_hash, g_str_equal, g_free,
            reinterpret_cast<GDestroyNotify>(
                purple_conv_chat_cb_destroy));
    }

    if (pAccount != g_accounts.end())
        pAccount->conversations.push_back(conv);

    return conv;
}

PurpleConversation *purple_conversation_new(PurpleConversationType type,
										PurpleAccount *account,
										const char *name)
{
    EVENT(NewConversationEvent, type, account, name);
    return purple_conversation_new_impl(type, account, name);
}

void purple_conversation_destroy(PurpleConversation *conv)
{
    auto pAccount = std::find_if(g_accounts.begin(), g_accounts.end(),
                                 [conv](const AccountInfo &info) { return info.account == conv->account; });
    ASSERT_FALSE(pAccount == g_accounts.end()) << "Removing conversation with unknown account";

    const std::vector<SignalConnection> connections = g_signalConnections;
    for (const SignalConnection &connection: connections) {
        if ((connection.instance == purple_conversations_get_handle()) &&
            (connection.signal == "deleting-conversation")) {
            typedef void (*DeletingConversationCallback)(PurpleConversation *, gpointer);
            reinterpret_cast<DeletingConversationCallback>(connection.callback)(
                conv, connection.data);
        }
    }

    auto it = std::find(pAccount->conversations.begin(), pAccount->conversations.end(), conv);
    ASSERT_FALSE(it == pAccount->conversations.end()) << "Removing unkown conversation";
    pAccount->conversations.erase(it);

    free(conv->name);
    free(conv->title);
    if (conv->type == PURPLE_CONV_TYPE_IM)
        delete conv->u.im;
    if (conv->type == PURPLE_CONV_TYPE_CHAT)
    {
        g_list_free(conv->u.chat->in_room);
        g_hash_table_destroy(conv->u.chat->users);
        g_free(conv->u.chat->who);
        g_free(conv->u.chat->topic);
        delete conv->u.chat;
    }
    delete conv;
}

PurpleConversationType purple_conversation_get_type(const PurpleConversation *conv)
{
    return conv->type;
}

const char *purple_conversation_get_name(const PurpleConversation *conv)
{
    return conv->name;
}

PurpleConvIm *purple_conversation_get_im_data(const PurpleConversation *conv)
{
    if (conv->type == PURPLE_CONV_TYPE_IM)
        return conv->u.im;

    return NULL;
}

PurpleConvChat *purple_conversation_get_chat_data(const PurpleConversation *conv)
{
    if (conv->type == PURPLE_CONV_TYPE_CHAT)
        return conv->u.chat;

    return NULL;
}

PurpleAccount *purple_conversation_get_account(const PurpleConversation *conv)
{
    return conv->account;
}

const char *purple_conversation_get_title(const PurpleConversation *conv)
{
    return conv->title;
}

static void replace_conversation_title(
    PurpleConversation *conv, const char *title)
{
    char *replacement = title ? strdup(title) : nullptr;
    free(conv->title);
    conv->title = replacement;
}

void purple_conversation_set_title(PurpleConversation *conv, const char *title)
{
    PurpleAccount *account = conv->account;
    replace_conversation_title(conv, title);
    EVENT(
        ConvSetTitleEvent,
        conv->name,
        conv->title ? conv->title : "");

    const auto conversationStillExists =
        [account, conv]() {
            const auto accountInfo = std::find_if(
                g_accounts.begin(), g_accounts.end(),
                [account](const AccountInfo &info) {
                    return info.account == account;
                });
            return accountInfo != g_accounts.end() &&
                   std::find(
                       accountInfo->conversations.begin(),
                       accountInfo->conversations.end(), conv) !=
                       accountInfo->conversations.end();
        };
    if (!conversationStillExists())
        return;

    const std::vector<SignalConnection> connections =
        g_signalConnections;
    for (const SignalConnection &connection : connections) {
        if (!conversationStillExists())
            break;
        if (connection.instance != purple_conversations_get_handle() ||
            connection.signal != "conversation-updated") {
            continue;
        }
        typedef void (*ConversationUpdatedCallback)(
            PurpleConversation *, PurpleConvUpdateType, gpointer);
        reinterpret_cast<ConversationUpdatedCallback>(
            connection.callback)(
                conv, PURPLE_CONV_UPDATE_TITLE,
                connection.data);
    }
}

void purple_conversation_write(PurpleConversation *conv, const char *who,
		const char *message, PurpleMessageFlags flags,
		time_t mtime)
{
    EVENT(ConversationWriteEvent, conv->name, who ? who : "", message, flags, mtime);
}

PurpleConversation *purple_conv_im_get_conversation(const PurpleConvIm *im)
{
    return im->conv;
}

PurpleConversation *purple_conv_chat_get_conversation(const PurpleConvChat *chat)
{
    return chat->conv;
}

int purple_conv_chat_get_id(const PurpleConvChat *chat)
{
    return chat->id;
}

gboolean purple_conv_chat_has_left(PurpleConvChat *chat)
{
    return chat->left;
}

void purple_conv_chat_left(PurpleConvChat *chat)
{
    chat->left = TRUE;
}

void purple_conv_im_write(PurpleConvIm *im, const char *who,
						const char *message, PurpleMessageFlags flags,
						time_t mtime)
{
    purple_conversation_write(purple_conv_im_get_conversation(im), who, message, flags, mtime);
}

void purple_conv_chat_write(PurpleConvChat *chat, const char *who,
						  const char *message, PurpleMessageFlags flags,
						  time_t mtime)
{
    purple_conversation_write(purple_conv_chat_get_conversation(chat), who, message, flags, mtime);
}

void purple_conv_chat_set_topic(PurpleConvChat *chat, const char *who,
							  const char *topic)
{
    g_free(chat->who);
    g_free(chat->topic);
    chat->who = who ? g_strdup(who) : NULL;
    chat->topic = topic ? g_strdup(topic) : NULL;
    EVENT(ChatSetTopicEvent, chat->conv->name, topic ? topic : "", who ? who : "");
}

const char *purple_conv_chat_get_topic(
    const PurpleConvChat *chat)
{
    return chat ? chat->topic : NULL;
}

gboolean purple_debug_is_enabled(void)
{
    return true;
}

gboolean purple_debug_is_verbose(void)
{
    return true;
}

PurpleBuddy *purple_find_buddy(PurpleAccount *account, const char *name)
{
    // purple_blist_find_chat returns NULL if account is not connected, so just in case, assume
    // purple_find_buddy will do likewise
    if (!purple_account_is_connected(account))
        return NULL;

    auto pAccount = std::find_if(g_accounts.begin(), g_accounts.end(),
                                 [account](const AccountInfo &info) { return info.account == account; });
    EXPECT_FALSE(pAccount == g_accounts.end()) << "Looking for buddy with unknown account";

    if (pAccount != g_accounts.end()) {
        auto it = std::find_if(pAccount->buddies.begin(), pAccount->buddies.end(),
                               [name](const PurpleBuddy *buddy) {
                                   return !strcmp(purple_buddy_get_name(buddy), name);
                               });
        if (it != pAccount->buddies.end())
            return *it;
    }

    return NULL;
}

PurpleConversation *purple_find_chat(const PurpleConnection *gc, int id)
{
    auto pAccount = std::find_if(g_accounts.begin(), g_accounts.end(),
                                 [gc](const AccountInfo &info) { return info.account == gc->account; });
    EXPECT_FALSE(pAccount == g_accounts.end()) << "Looking for buddy with unknown account";

    if (pAccount != g_accounts.end()) {
        auto it = std::find_if(pAccount->conversations.begin(), pAccount->conversations.end(),
                               [id](const PurpleConversation *conv) {
                                   return (conv->type == PURPLE_CONV_TYPE_CHAT) &&
                                          (conv->u.chat->id == id);
                               });
        if (it != pAccount->conversations.end())
            return *it;
    }

    return NULL;
}

GList *purple_get_chats(void)
{
    g_list_free(g_chatConversations);
    g_chatConversations = nullptr;
    for (const AccountInfo &accountInfo : g_accounts) {
        for (PurpleConversation *conversation : accountInfo.conversations) {
            if (purple_conversation_get_type(conversation) ==
                PURPLE_CONV_TYPE_CHAT) {
                g_chatConversations =
                    g_list_append(g_chatConversations, conversation);
            }
        }
    }
    return g_chatConversations;
}

PurpleConversation *purple_find_conversation_with_account(
		PurpleConversationType type, const char *name,
		const PurpleAccount *account)
{
    auto pAccount = std::find_if(g_accounts.begin(), g_accounts.end(),
                                 [account](const AccountInfo &info) { return info.account == account; });
    EXPECT_FALSE(pAccount == g_accounts.end()) << "Adding conversation with unknown account";

    if (pAccount != g_accounts.end()) {
        auto it = std::find_if(pAccount->conversations.begin(), pAccount->conversations.end(),
                               [type, name](PurpleConversation *existing) {
                                   return !strcmp(existing->name, name) && (existing->type == type);
                               });
        if (it != pAccount->conversations.end())
            return *it;
    }
    return NULL;
}

void *purple_notify_message(void *handle, PurpleNotifyMsgType type,
						  const char *title, const char *primary,
						  const char *secondary, PurpleNotifyCloseCallback cb,
						  gpointer user_data)
{
    g_purpleEvents.addNotify(
        handle, type, title, primary, secondary);
    return NULL;
}

PurpleNotifyUserInfo *purple_notify_user_info_new(void)
{
    return NULL;
}

GList *purple_notify_user_info_get_entries(PurpleNotifyUserInfo *user_info)
{
    return NULL;
}

void purple_notify_user_info_add_section_break(PurpleNotifyUserInfo *user_info)
{
}

void purple_notify_user_info_add_pair(PurpleNotifyUserInfo *user_info, const char *label, const char *value)
{
}

void *purple_notify_userinfo(PurpleConnection *gc, const char *who,
						   PurpleNotifyUserInfo *user_info, PurpleNotifyCloseCallback cb,
						   gpointer user_data)
{
    return NULL;
}

gboolean purple_plugin_register(PurplePlugin *plugin)
{
    // TODO maybe event
    g_plugin = plugin;
    return TRUE;
}

const char *purple_primitive_get_id_from_type(PurpleStatusPrimitive type)
{
    return (const char *)type;
}

void purple_prpl_got_user_status(PurpleAccount *account, const char *name,
							   const char *status_id, ...)
{
    PurpleStatusPrimitive type = (PurpleStatusPrimitive)(unsigned long)status_id;
    EVENT(UserStatusEvent, account, name, type);
}

void *purple_request_action(void *handle, const char *title, const char *primary,
	const char *secondary, int default_action, PurpleAccount *account,
	const char *who, PurpleConversation *conv, void *user_data,
	size_t action_count, ...)
{
    std::vector<std::string> buttons;
    std::vector<PurpleRequestActionCb> callbacks;
    va_list ap;
    va_start(ap, action_count);
    for (size_t i = 0; i < action_count; i++) {
        buttons.emplace_back(va_arg(ap, char*));
        callbacks.push_back(va_arg(ap, PurpleRequestActionCb));
    }
    va_end(ap);

    EVENT(RequestActionEvent, handle, title, primary, secondary, account, who, conv, user_data, buttons, callbacks);
    return NULL;
}

void *purple_request_input(void *handle, const char *title, const char *primary,
	const char *secondary, const char *default_value, gboolean multiline,
	gboolean masked, gchar *hint,
	const char *ok_text, GCallback ok_cb,
	const char *cancel_text, GCallback cancel_cb,
	PurpleAccount *account, const char *who, PurpleConversation *conv,
	void *user_data)
{
    EVENT(RequestInputEvent, handle, title, primary, secondary, default_value, masked, ok_text, ok_cb,
          cancel_text, cancel_cb, account, who, conv, user_data);

    // Just return some non-NULL pointer
    return &g_accounts;
}

void purple_request_close_with_handle(void *handle)
{
    g_purpleEvents.closeInputRequests(handle);
}

PurpleRoomlist *purple_roomlist_new(PurpleAccount *account)
{
    PurpleRoomlist *roomlist = new PurpleRoomlist;
    roomlist->account = account;
    roomlist->fields = NULL;
    roomlist->rooms = NULL;
    roomlist->in_progress = FALSE;
    roomlist->ui_data = NULL;
    roomlist->proto_data = NULL;
    roomlist->ref = 1;
    return roomlist;
}

void purple_roomlist_set_in_progress(PurpleRoomlist *list, gboolean in_progress)
{
    list->in_progress = in_progress;
    EVENT(RoomlistInProgressEvent, list, in_progress);
}

gboolean purple_roomlist_get_in_progress(PurpleRoomlist *list)
{
    return list->in_progress;
}

void purple_roomlist_ref(PurpleRoomlist *list)
{
    list->ref++;
}

static void freeRoomlistField(gpointer data)
{
    PurpleRoomlistField *field = static_cast<PurpleRoomlistField *>(data);
    if (!field)
        return;

    g_free(field->label);
    g_free(field->name);
    delete field;
}

static void freeRoomlistRoom(PurpleRoomlist *list, PurpleRoomlistRoom *room)
{
    GList *fieldInfo = list->fields;
    GList *fieldValue = room->fields;
    while (fieldInfo && fieldValue) {
        const PurpleRoomlistField *field =
            static_cast<const PurpleRoomlistField *>(fieldInfo->data);
        if (field && field->type == PURPLE_ROOMLIST_FIELD_STRING)
            g_free(fieldValue->data);
        fieldInfo = fieldInfo->next;
        fieldValue = fieldValue->next;
    }

    g_list_free(room->fields);
    g_free(room->name);
    delete room;
}

void purple_roomlist_unref(PurpleRoomlist *list)
{
    ASSERT_NE(0u, list->ref);
    list->ref--;
    if (list->ref == 0) {
        for (GList *item = list->rooms; item; item = item->next)
            freeRoomlistRoom(list, static_cast<PurpleRoomlistRoom *>(item->data));
        g_list_free(list->rooms);
        g_list_free_full(list->fields, freeRoomlistField);
        delete list;
    }
}

PurpleRoomlistField *purple_roomlist_field_new(PurpleRoomlistFieldType type,
                                           const gchar *label, const gchar *name,
                                           gboolean hidden)
{
    PurpleRoomlistField *field = new PurpleRoomlistField;
    field->type = type;
    field->label = g_strdup(label);
    field->name = g_strdup(name);
    field->hidden = hidden;
    return field;
}

void purple_roomlist_set_fields(PurpleRoomlist *list, GList *fields)
{
    list->fields = fields;
}

GList *purple_roomlist_get_fields(PurpleRoomlist *list)
{
    return list->fields;
}

PurpleRoomlistRoom *purple_roomlist_room_new(PurpleRoomlistRoomType type, const gchar *name,
                                         PurpleRoomlistRoom *parent)
{
    PurpleRoomlistRoom *room = new PurpleRoomlistRoom;
    room->type = type;
    room->name = g_strdup(name);
    room->fields = NULL;
    room->parent = parent;
    room->expanded_once = FALSE;
    return room;
}

void purple_roomlist_room_add_field(PurpleRoomlist *list, PurpleRoomlistRoom *room, gconstpointer value)
{
    const guint index = g_list_length(room->fields);
    PurpleRoomlistField *field =
        static_cast<PurpleRoomlistField *>(g_list_nth_data(list->fields, index));
    ASSERT_NE(nullptr, field) << "Adding more room values than declared fields";

    gpointer storedValue = const_cast<gpointer>(value);
    switch (field->type) {
        case PURPLE_ROOMLIST_FIELD_BOOL:
            storedValue = GINT_TO_POINTER(value != NULL);
            break;
        case PURPLE_ROOMLIST_FIELD_INT:
            break;
        case PURPLE_ROOMLIST_FIELD_STRING:
            storedValue = g_strdup(static_cast<const char *>(value));
            break;
    }
    room->fields = g_list_append(room->fields, storedValue);
}

PurpleRoomlistRoomType purple_roomlist_room_get_type(PurpleRoomlistRoom *room)
{
    return room->type;
}

const char *purple_roomlist_room_get_name(PurpleRoomlistRoom *room)
{
    return room->name;
}

PurpleRoomlistRoom *purple_roomlist_room_get_parent(PurpleRoomlistRoom *room)
{
    return room->parent;
}

GList *purple_roomlist_room_get_fields(PurpleRoomlistRoom *room)
{
    return room->fields;
}

static std::vector<std::string> getRoomlistFieldValues(PurpleRoomlist *list,
                                                        PurpleRoomlistRoom *room)
{
    std::vector<std::string> values;
    GList *fieldInfo = list->fields;
    GList *fieldValue = room->fields;
    while (fieldInfo && fieldValue) {
        const PurpleRoomlistField *field =
            static_cast<const PurpleRoomlistField *>(fieldInfo->data);
        switch (field->type) {
            case PURPLE_ROOMLIST_FIELD_BOOL:
                values.push_back(GPOINTER_TO_INT(fieldValue->data) ? "true" : "false");
                break;
            case PURPLE_ROOMLIST_FIELD_INT:
                values.push_back(std::to_string(GPOINTER_TO_INT(fieldValue->data)));
                break;
            case PURPLE_ROOMLIST_FIELD_STRING:
                values.push_back(fieldValue->data
                                     ? static_cast<const char *>(fieldValue->data)
                                     : "");
                break;
        }
        fieldInfo = fieldInfo->next;
        fieldValue = fieldValue->next;
    }
    return values;
}

void purple_roomlist_room_add(PurpleRoomlist *list, PurpleRoomlistRoom *room)
{
    ASSERT_EQ(nullptr, g_list_find(list->rooms, room)) << "Adding a room twice";
    list->rooms = g_list_append(list->rooms, room);
    EVENT(RoomlistAddRoomEvent, list, room, getRoomlistFieldValues(list, room));
}

void purple_serv_got_join_chat_failed(PurpleConnection *gc, GHashTable *data)
{
    const char *chatName = data
        ? static_cast<const char *>(g_hash_table_lookup(data, "id"))
        : nullptr;
    EVENT(JoinChatFailedEvent, gc, chatName ? chatName : "");
}

PurpleStatusType *purple_status_type_new_full(PurpleStatusPrimitive primitive,
										  const char *id, const char *name,
										  gboolean saveable,
										  gboolean user_settable,
										  gboolean independent)
{
    return NULL;
}

gboolean purple_status_is_available(const PurpleStatus *status)
{
    (void)status;
    return TRUE;
}

gboolean purple_status_is_online(const PurpleStatus *status)
{
    (void)status;
    return TRUE;
}

const char *purple_user_dir(void)
{
    return "purple_user_dir";
}

PurpleXfer *purple_xfer_new(PurpleAccount *account,
									PurpleXferType type, const char *who)
{
    PurpleXfer *xfer = new PurpleXfer();
    xfer->account = account;
    xfer->type = type;
    xfer->who = strdup(who);
    xfer->ref = 1;
    xfer->status = PURPLE_XFER_STATUS_UNKNOWN;
    xfer->fd = -1;
    return xfer;
}

void purple_xfer_ref(PurpleXfer *xfer)
{
    xfer->ref++;
}

void purple_xfer_unref(PurpleXfer *xfer)
{
    if (--xfer->ref == 0) {
        EXPECT_EQ(nullptr, xfer->dest_fp)
            << "Destroying a transfer with an open destination file";
        if (xfer->dest_fp)
            fclose(xfer->dest_fp);
        free(xfer->who);
        free(xfer->filename);
        free(xfer->local_filename);
        delete xfer;
    }
}

void purple_xfer_request(PurpleXfer *xfer)
{
    EVENT(XferRequestEvent, purple_xfer_get_type(xfer), xfer->who, purple_xfer_get_filename(xfer), xfer);
}

std::map<std::string, size_t> fakeFiles;

void setFakeFileSize(const char *path, size_t size)
{
    fakeFiles[path] = size;
}

void clearFakeFiles()
{
    fakeFiles.clear();
}

void purple_xfer_request_accepted(PurpleXfer *xfer, const char *filename)
{
    EVENT(XferAcceptedEvent, xfer, filename);
    xfer->status = PURPLE_XFER_STATUS_ACCEPTED;
    xfer->local_filename = strdup(filename);
    if (xfer->type == PURPLE_XFER_SEND)
        xfer->size = fakeFiles.at(filename);
    if (xfer->ops.init)
        xfer->ops.init(xfer);
}

void purple_xfer_set_init_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *))
{
    xfer->ops.init = fnc;
}

void purple_xfer_set_start_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *))
{
    xfer->ops.start = fnc;
}

void purple_xfer_set_cancel_send_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *))
{
    xfer->ops.cancel_send = fnc;
}

void purple_xfer_set_cancel_recv_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *))
{
    xfer->ops.cancel_recv = fnc;
}

void purple_xfer_set_end_fnc(PurpleXfer *xfer, void (*fnc)(PurpleXfer *))
{
    xfer->ops.end = fnc;
}

const char *purple_xfer_get_remote_user(const PurpleXfer *xfer)
{
    return xfer->who;
}

const char *purple_xfer_get_filename(const PurpleXfer *xfer)
{
    return xfer->filename;
}

const char *purple_xfer_get_local_filename(const PurpleXfer *xfer)
{
    return xfer->local_filename;
}

void purple_xfer_set_filename(PurpleXfer *xfer, const char *filename)
{
    xfer->filename = strdup(filename);
}

PurpleAccount *purple_xfer_get_account(const PurpleXfer *xfer)
{
    return xfer->account;
}

void purple_xfer_set_size(PurpleXfer *xfer, size_t size)
{
    xfer->size = size;
}

void purple_xfer_start(PurpleXfer *xfer, int fd, const char *ip,
						 unsigned int port)
{
    xfer->status = PURPLE_XFER_STATUS_STARTED;
    xfer->fd = (fd == 0) ? -1 : fd;

    // In libpurple, setting STARTED can synchronously call the UI before
    // begin_transfer opens the destination file. Tests use this event to
    // model that reentrancy.
    EVENT(XferStartEvent, xfer->local_filename);

    // Inline progress transfers install a start hook specifically to clean
    // up a destination that libpurple opens after a synchronous cancellation.
    // Other mock transfers deliberately retain their historical no-I/O
    // behavior.
    if (xfer->type == PURPLE_XFER_RECEIVE &&
        xfer->ops.start &&
        xfer->local_filename) {
        xfer->dest_fp = fopen(xfer->local_filename, "wb");
        if (!xfer->dest_fp) {
            purple_xfer_cancel_local(xfer);
            return;
        }
        if (fseek(xfer->dest_fp, xfer->bytes_sent,
                  SEEK_SET) != 0) {
            purple_xfer_cancel_local(xfer);
            return;
        }
    }

    xfer->start_time = time(NULL);
    if (xfer->ops.start)
        xfer->ops.start(xfer);
}

void purple_xfer_cancel_local(PurpleXfer *xfer)
{
    xfer->status = PURPLE_XFER_STATUS_CANCEL_LOCAL;
    xfer->end_time = time(NULL);
    EVENT(XferLocalCancelEvent, xfer->local_filename ? xfer->local_filename : "");
    if ((xfer->type == PURPLE_XFER_SEND) && xfer->ops.cancel_send)
        xfer->ops.cancel_send(xfer);
    if ((xfer->type == PURPLE_XFER_RECEIVE) && xfer->ops.cancel_recv)
        xfer->ops.cancel_recv(xfer);

    if (xfer->fd != -1) {
        close(xfer->fd);
        xfer->fd = -1;
    }
    if (xfer->dest_fp) {
        fclose(xfer->dest_fp);
        xfer->dest_fp = NULL;
    }
    purple_xfer_unref(xfer);
}

gboolean purple_xfer_is_canceled(const PurpleXfer *xfer)
{
    return (xfer->status == PURPLE_XFER_STATUS_CANCEL_LOCAL) ||
           (xfer->status == PURPLE_XFER_STATUS_CANCEL_REMOTE);
}

void purple_xfer_cancel_remote(PurpleXfer *xfer)
{
    xfer->status = PURPLE_XFER_STATUS_CANCEL_REMOTE;
    xfer->end_time = time(NULL);
    EVENT(XferRemoteCancelEvent, xfer->local_filename);
    if ((xfer->type == PURPLE_XFER_SEND) && xfer->ops.cancel_send)
        xfer->ops.cancel_send(xfer);
    if ((xfer->type == PURPLE_XFER_RECEIVE) && xfer->ops.cancel_recv)
        xfer->ops.cancel_recv(xfer);
    if (xfer->fd != -1) {
        close(xfer->fd);
        xfer->fd = -1;
    }
    if (xfer->dest_fp) {
        fclose(xfer->dest_fp);
        xfer->dest_fp = NULL;
    }
    purple_xfer_unref(xfer);
}

void purple_xfer_error(PurpleXferType type, PurpleAccount *account, const char *who, const char *msg)
{
    purple_notify_error(account, "Xfer error", who, msg);
}

PurpleXferType purple_xfer_get_type(const PurpleXfer *xfer)
{
    return xfer->type;
}

void purple_xfer_set_bytes_sent(PurpleXfer *xfer, size_t bytes_sent)
{
    xfer->bytes_sent = bytes_sent;
}

size_t purple_xfer_get_bytes_sent(const PurpleXfer *xfer)
{
    return xfer->bytes_sent;
}

void purple_xfer_set_completed(PurpleXfer *xfer, gboolean completed)
{
    if (completed)
        xfer->status = PURPLE_XFER_STATUS_DONE;
    EVENT(XferCompletedEvent, xfer->local_filename, completed, xfer->bytes_sent);
}

void purple_xfer_update_progress(PurpleXfer *xfer)
{
    EVENT(XferProgressEvent, xfer->local_filename, xfer->bytes_sent);
}

void purple_xfer_end(PurpleXfer *xfer)
{
    if (xfer->status != PURPLE_XFER_STATUS_DONE) {
        purple_xfer_cancel_local(xfer);
        return;
    }

    xfer->end_time = time(NULL);
    EVENT(XferEndEvent, xfer->local_filename);
    if (xfer->ops.end)
        xfer->ops.end(xfer);
    if (xfer->fd != -1) {
        close(xfer->fd);
        xfer->fd = -1;
    }
    if (xfer->dest_fp) {
        fclose(xfer->dest_fp);
        xfer->dest_fp = NULL;
    }
    purple_xfer_unref(xfer);
}

time_t purple_xfer_get_end_time(const PurpleXfer *xfer)
{
    return xfer->end_time;
}

PurpleXferStatusType purple_xfer_get_status(const PurpleXfer *xfer)
{
    return xfer->status;
}

size_t purple_xfer_get_size(const PurpleXfer *xfer)
{
    return xfer->size;
}

gboolean
purple_xfer_write_file(PurpleXfer *xfer, const guchar *buffer, gsize size)
{
    if (xfer->status != PURPLE_XFER_STATUS_STARTED) {
        purple_debug_misc("purple_xfer_write_file", "write_file requires a transfer in progress\n");
        purple_xfer_cancel_local(xfer);
        return FALSE;
    }
    EXPECT_LE(xfer->bytes_sent + size, xfer->size);
    xfer->bytes_sent += size;
    EVENT(XferWriteFileEvent, xfer->local_filename, buffer, size);
    return TRUE;
}

void serv_got_chat_in(PurpleConnection *g, int id, const char *who,
					  PurpleMessageFlags flags, const char *message, time_t mtime)
{
    EVENT(ServGotChatEvent, g, id, who, message, flags, mtime);
}

void serv_got_im(PurpleConnection *gc, const char *who, const char *msg,
				 PurpleMessageFlags flags, time_t mtime)
{
    if (purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, who, gc->account) == NULL) {
        purple_conversation_new_impl(PURPLE_CONV_TYPE_IM, gc->account, who);
    }
    EVENT(ServGotImEvent, gc, who, msg, flags, mtime);
}

void serv_got_alias(PurpleConnection *gc, const char *who, const char *alias)
{
    PurpleBuddy *buddy = purple_find_buddy(gc->account, who);
    ASSERT_NE(nullptr, buddy);

    if (purple_strings_are_different(buddy->server_alias, alias)) {
        const char *oldDisplayAlias = purple_buddy_get_alias(buddy);
        std::string oldDisplayAliasCopy = oldDisplayAlias ? oldDisplayAlias : "";
        purple_blist_server_alias_buddy(buddy, alias);
        const char *newDisplayAlias = purple_buddy_get_alias(buddy);
        if (oldDisplayAliasCopy != (newDisplayAlias ? newDisplayAlias : ""))
            EVENT(AliasBuddyEvent, who, newDisplayAlias);
    }
}

PurpleConversation *serv_got_joined_chat(PurpleConnection *gc,
									   int id, const char *name)
{
    PurpleConversation *conv = purple_conversation_new_impl(PURPLE_CONV_TYPE_CHAT, gc->account, name);
    purple_conversation_get_chat_data(conv)->id = id;

    PurpleChat *chat = purple_blist_find_chat(gc->account, name);
    if (chat && chat->alias)
        replace_conversation_title(conv, chat->alias);

    EVENT(ServGotJoinedChatEvent, gc, id, name, conv->title ? conv->title : name);
    return conv;
}

void serv_got_chat_left(PurpleConnection *gc, int id)
{
    PurpleConversation *conversation = purple_find_chat(gc, id);
    if (conversation)
        purple_conv_chat_left(
            purple_conversation_get_chat_data(conversation));
}

void serv_got_typing(PurpleConnection *gc, const char *name, int timeout,
					 PurpleTypingState state)
{
    // TODO event
}

void serv_got_typing_stopped(PurpleConnection *gc, const char *name)
{
    // TODO event
}

void purple_conversation_present(PurpleConversation *conv)
{
    EVENT(PresentConversationEvent, conv->name);
}

void purple_conv_chat_add_user(PurpleConvChat *chat, const char *user,
							 const char *extra_msg, PurpleConvChatBuddyFlags flags,
							 gboolean new_arrival)
{
    PurpleConvChatBuddy *buddy =
        purple_conv_chat_cb_find(chat, user);
    if (buddy) {
        buddy->flags = flags;
    } else {
        buddy = purple_conv_chat_cb_new(user, NULL, flags);
        chat->in_room = g_list_append(chat->in_room, buddy);
        g_hash_table_insert(
            chat->users, g_strdup(user), buddy);
    }
    EVENT(ChatAddUserEvent, chat->conv->name, user, extra_msg ? extra_msg : "", flags, new_arrival);
}

void purple_conv_chat_add_users(PurpleConvChat *chat, GList *users, GList *extra_msgs,
							  GList *flags, gboolean new_arrivals)
{
    GList *user, *flag;
    for (user = users, flag = flags; user; user = user->next, flag = flag->next)
        purple_conv_chat_add_user(chat, (const char *)user->data, NULL,
                                  (PurpleConvChatBuddyFlags)GPOINTER_TO_INT(flag->data), new_arrivals);
}

void purple_conv_chat_clear_users(PurpleConvChat *chat)
{
    g_list_free(chat->in_room);
    chat->in_room = NULL;
    g_hash_table_remove_all(chat->users);
    EVENT(ChatClearUsersEvent, chat->conv->name);
}

void purple_conv_chat_remove_user(PurpleConvChat *chat, const char *user, const char *reason)
{
    (void)reason;
    PurpleConvChatBuddy *buddy =
        purple_conv_chat_cb_find(chat, user);
    if (buddy)
        chat->in_room =
            g_list_remove(chat->in_room, buddy);
    g_hash_table_remove(chat->users, user);
}

gboolean purple_conv_chat_find_user(PurpleConvChat *chat, const char *user)
{
    return g_hash_table_contains(chat->users, user);
}

void purple_conv_chat_user_set_flags(PurpleConvChat *chat, const char *user,
								   PurpleConvChatBuddyFlags flags)
{
    if (purple_conv_chat_find_user(chat, user))
        purple_conv_chat_cb_find(chat, user)->flags = flags;
}

PurpleConvChatBuddyFlags purple_conv_chat_user_get_flags(
    PurpleConvChat *chat, const char *user)
{
    PurpleConvChatBuddy *buddy =
        purple_conv_chat_cb_find(chat, user);
    return buddy ? buddy->flags : PURPLE_CBFLAGS_NONE;
}

GList *purple_conv_chat_get_users(const PurpleConvChat *chat)
{
    return chat ? chat->in_room : NULL;
}

PurpleConvChatBuddy *purple_conv_chat_cb_new(
    const char *name, const char *alias,
    PurpleConvChatBuddyFlags flags)
{
    PurpleConvChatBuddy *buddy =
        g_new0(PurpleConvChatBuddy, 1);
    buddy->name = g_strdup(name);
    buddy->alias = alias ? g_strdup(alias) : NULL;
    buddy->flags = flags;
    buddy->attributes = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, g_free);
    return buddy;
}

PurpleConvChatBuddy *purple_conv_chat_cb_find(
    PurpleConvChat *chat, const char *name)
{
    return chat
        ? static_cast<PurpleConvChatBuddy *>(
              g_hash_table_lookup(chat->users, name))
        : NULL;
}

const char *purple_conv_chat_cb_get_name(
    PurpleConvChatBuddy *buddy)
{
    return buddy ? buddy->name : NULL;
}

void purple_conv_chat_cb_destroy(
    PurpleConvChatBuddy *buddy)
{
    if (!buddy)
        return;
    g_free(buddy->name);
    g_free(buddy->alias);
    g_free(buddy->alias_key);
    if (buddy->attributes)
        g_hash_table_destroy(buddy->attributes);
    g_free(buddy);
}

PurpleBlistNode *purple_blist_get_root(void)
{
    return &root;
}

PurpleBlistNode *purple_blist_node_get_sibling_next(PurpleBlistNode *node)
{
    return node->next;
}

PurpleBlistNodeType purple_blist_node_get_type(PurpleBlistNode *node)
{
    return node->type;
}

GHashTable *purple_chat_get_components(PurpleChat *chat)
{
    return chat->components;
}

PurpleBlistNode *purple_blist_node_get_first_child(PurpleBlistNode *node)
{
    return node->child;
}

void purple_blist_node_set_string(PurpleBlistNode *node, const char *key,
		const char *value)
{
    g_hash_table_insert(node->settings, (void *)key, g_strdup(value));
}

const char *purple_blist_node_get_string(PurpleBlistNode *node, const char *key)
{
    return static_cast<const char *>(g_hash_table_lookup(node->settings, key));
}

void purple_blist_node_remove_setting(PurpleBlistNode *node, const char *key)
{
    g_hash_table_remove(node->settings, key);
}

static char groupName[] = "Group";

PurpleGroup standardPurpleGroup = {
    .node = PurpleBlistNode(),
	.name = groupName,
	.totalsize = 0,
	.currentsize = 0,
	.online = 0
};

PurpleGroup *purple_find_group(const char *name)
{
    if (!strcmp(name, standardPurpleGroup.name))
        return &standardPurpleGroup;
    return NULL;
}

const char *purple_group_get_name(PurpleGroup *group)
{
    return group->name;
}

struct _PurpleStoredImage {
    std::vector<uint8_t> data;
};

std::vector<std::unique_ptr<PurpleStoredImage>> imageStore;

guint8 *arrayDup(gpointer data, size_t size)
{
    guint8 *result = (guint8 *)g_malloc(size);
    memmove(result, data, size);
    return result;
}

int purple_imgstore_add_with_id(gpointer data, size_t size, const char *filename)
{
    imageStore.push_back(std::make_unique<PurpleStoredImage>());
    imageStore.back()->data = std::vector<uint8_t>(size);
    memmove(imageStore.back()->data.data(), data, size);
    g_free(data);
    return imageStore.size();
}

int getLastImgstoreId()
{
    return imageStore.size();
}

PurpleStoredImage *purple_imgstore_find_by_id(int id)
{
    if ((id >= 1) && ((unsigned)id <= imageStore.size()))
        return imageStore[id-1].get();
    else
        return NULL;
}

gconstpointer purple_imgstore_get_data(PurpleStoredImage *img)
{
    return img->data.data();
}

size_t purple_imgstore_get_size(PurpleStoredImage *img)
{
    return img->data.size();
}

gchar *purple_markup_escape_text(const gchar *text, gssize length)
{
    std::string s(text, length);
    size_t pos;
    while ((pos = s.find('<')) != std::string::npos)
        s.replace(pos, 1, "&lt;");
    while ((pos = s.find('>')) != std::string::npos)
        s.replace(pos, 1, "&gt;");
    return g_strdup(s.c_str());
}

char *purple_unescape_html(const char *html)
{
    std::string s(html);
    size_t pos;
    while ((pos = s.find("&lt;")) != std::string::npos)
        s.replace(pos, 4, "<");
    while ((pos = s.find("&gt;")) != std::string::npos)
        s.replace(pos, 4, ">");
    return g_strdup(s.c_str());
}

char *purple_markup_strip_html(const char *str)
{
    std::string s(str);
    size_t pos;
    while ((pos = s.find("<")) != std::string::npos) {
        size_t tagend = s.find(">", pos);
        if (tagend != std::string::npos)
            s.erase(pos, tagend-pos+1);
    }
    return g_strdup(s.c_str());
}

PurpleProxyInfo *purple_proxy_get_setup(PurpleAccount *account)
{
    return account->proxy_info;
}

PurpleProxyType purple_proxy_info_get_type(const PurpleProxyInfo *info)
{
    return info->type;
}

const char *purple_proxy_info_get_host(const PurpleProxyInfo *info)
{
    return info->host;
}

int purple_proxy_info_get_port(const PurpleProxyInfo *info)
{
    return info->port;
}

const char *purple_proxy_info_get_username(const PurpleProxyInfo *info)
{
    return info->username;
}

const char *purple_proxy_info_get_password(const PurpleProxyInfo *info)
{
    return info->password;
}

PurpleRequestFields *purple_request_fields_new(void)
{
    return NULL;
}

PurpleRequestFieldGroup *purple_request_field_group_new(const char *title)
{
    return NULL;
}

PurpleRequestField *purple_request_field_string_new(const char *id,
												const char *text,
												const char *default_value,
												gboolean multiline)
{
    return NULL;
}

void purple_request_field_set_type_hint(PurpleRequestField *field,
									  const char *type_hint)
{
}

void purple_request_field_string_set_masked(PurpleRequestField *field,
										  gboolean masked)
{
}

void purple_request_field_group_add_field(PurpleRequestFieldGroup *group,
										PurpleRequestField *field)
{
}

void purple_request_fields_add_group(PurpleRequestFields *fields,
								   PurpleRequestFieldGroup *group)
{
}

const char *purple_request_fields_get_string(const PurpleRequestFields *fields,
										   const char *id)
{
    return "";
}

void *purple_request_fields(void *handle, const char *title, const char *primary,
	const char *secondary, PurpleRequestFields *fields,
	const char *ok_text, GCallback ok_cb,
	const char *cancel_text, GCallback cancel_cb,
	PurpleAccount *account, const char *who, PurpleConversation *conv,
	void *user_data)
{
    return NULL;
}

PurpleMenuAction *purple_menu_action_new(const char *label, PurpleCallback callback,
                                     gpointer data, GList *children)
{
    PurpleMenuAction *action = new PurpleMenuAction;
    action->label = strdup(label);
    action->callback = callback;
    action->children = NULL;
    action->data = data;
    return action;
}

void purple_menu_action_free(PurpleMenuAction *act)
{
    free(act->label);
    delete act;
}

PurplePluginAction *purple_plugin_action_new(const char* label, void (*callback)(PurplePluginAction *))
{
    return NULL;
}

PurpleAccountOption *purple_account_option_string_new(const char *text,
	const char *pref_name, const char *default_value)
{
    return NULL;
}

PurpleAccountOption *purple_account_option_bool_new(const char *text,
	const char *pref_name, gboolean default_value)
{
    return NULL;
}

PurpleAccountOption *purple_account_option_list_new(const char *text,
	const char *pref_name, GList *list)
{
    for (GList *choice = list; choice; choice = g_list_next(choice)) {
        PurpleKeyValuePair *kvp = static_cast<PurpleKeyValuePair *>(choice->data);
        g_free(kvp->key);
        g_free(kvp->value);
        g_free(kvp);
    }
    g_list_free(list);
    return NULL;
}

const char *purple_account_get_string(const PurpleAccount *account,
									const char *name,
									const char *default_value)
{
    auto it = std::find_if(g_accounts.begin(), g_accounts.end(),
                           [account](const AccountInfo &info) { return info.account == account; });
    EXPECT_FALSE(it == g_accounts.end()) << "Unknown account";

    if (it != g_accounts.end()) {
        auto itOption = it->stringsOptions.find(name);
        if (itOption != it->stringsOptions.end())
            return itOption->second.c_str();
    }

    return default_value;
}

void purple_account_set_string(PurpleAccount *account, const char *name,
                                                         const char *value)
{
    auto it = std::find_if(g_accounts.begin(), g_accounts.end(),
                           [account](const AccountInfo &info) { return info.account == account; });
    EXPECT_FALSE(it == g_accounts.end()) << "Unknown account";

    if (it != g_accounts.end()) {
        it->stringsOptions[name] = value;
    }
}

gboolean purple_account_get_bool(const PurpleAccount *account, const char *name,
							   gboolean default_value)
{
    return *purple_account_get_string(account, name, default_value ? "true" : "");
}

void purple_account_set_bool(PurpleAccount *account, const char *name,
						   gboolean value)
{
    purple_account_set_string(account, name, value ? "true" : "");
}

void purple_account_remove_setting(PurpleAccount *account, const char *setting)
{
    auto it = std::find_if(g_accounts.begin(), g_accounts.end(),
                           [account](const AccountInfo &info) { return info.account == account; });
    EXPECT_FALSE(it == g_accounts.end()) << "Unknown account";

    if (it != g_accounts.end()) {
        it->stringsOptions.erase(setting);
    }
}

char *purple_str_size_to_units(size_t size)
{
    return g_strdup("purple_str_size_to_units");
}

PurpleCmdId purple_cmd_register(const gchar *cmd, const gchar *args, PurpleCmdPriority p, PurpleCmdFlag f,
                             const gchar *prpl_id, PurpleCmdFunc func, const gchar *helpstr, void *data)
{
    if (g_commandRegistrationFailureCountdown &&
        --g_commandRegistrationFailureCountdown == 0) {
        return 0;
    }

    const PurpleCmdId id = g_nextCommandId++;
    g_registeredCommands[id] = cmd;
    g_purpleEvents.addCommand(cmd, func, data);
    return id;
}

void purple_cmd_unregister(PurpleCmdId id)
{
    const auto command = g_registeredCommands.find(id);
    if (command == g_registeredCommands.end())
        return;

    g_purpleEvents.removeCommand(command->second.c_str());
    g_registeredCommands.erase(command);
}

PurpleMediaManager *purple_media_manager_get(void)
{
    return NULL;
}

PurpleMediaCaps purple_media_manager_get_ui_caps(PurpleMediaManager *manager)
{
    return PURPLE_MEDIA_CAPS_NONE;
}

GHashTable *uiInfo = NULL;

GHashTable *purple_core_get_ui_info()
{
    if (!uiInfo)
        uiInfo = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);

    return uiInfo;
}

void setUiName(const char *name)
{
    if (!uiInfo)
        uiInfo = g_hash_table_new_full(g_str_hash, g_str_equal, NULL, NULL);

    static char nameKey[] = "name";
    g_hash_table_insert(uiInfo, nameKey, const_cast<char *>(name));

}

void *purple_conversations_get_handle()
{
    return &g_conversationsHandle;
}

void *purple_blist_get_handle()
{
    return &g_blistHandle;
}

PurpleConversationUiOps *purple_conversation_get_ui_ops(const PurpleConversation *conv)
{
    return conv->ui_ops;
}

gboolean purple_conversation_has_focus(PurpleConversation *conv)
{
    if (!conv || !conv->ui_ops || !conv->ui_ops->has_focus)
        return FALSE;
    return conv->ui_ops->has_focus(conv);
}

gulong purple_signal_connect(void *instance, const char *signal,
	void *handle, PurpleCallback func, void *data)
{
    const gulong id = g_nextSignalId++;
    g_signalConnections.push_back(
        SignalConnection{instance, signal ? signal : "", handle, func, data, id});
    return id;
}

void purple_signals_disconnect_by_handle(void *handle)
{
    g_signalConnections.erase(
        std::remove_if(
            g_signalConnections.begin(), g_signalConnections.end(),
            [handle](const SignalConnection &connection) {
                return connection.handle == handle;
            }),
        g_signalConnections.end());
}

};
