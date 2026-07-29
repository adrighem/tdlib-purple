#include "purple-info.h"
#include "config.h"
#include "format.h"
#include <algorithm>
#include <cerrno>
#include <cmath>
#include <limits>

static char chatNameComponent[] = "id";
static char joinStringKey[]     = "link";
static char nameKey[]           = "name";
static char typeKey[]           = "type";

const char *getChatNameComponent()
{
    return chatNameComponent;
}

GList *getChatJoinInfo()
{
    // First entry is the internal chat name used to look up conversations
    struct proto_chat_entry *pce;
    pce = g_new0 (struct proto_chat_entry, 1);
    // TRANSLATOR: Info item *and* dialog item.
    pce->label = _("Chat ID (don't change):");
    pce->identifier = chatNameComponent;
    pce->required = FALSE;
    GList *info = g_list_append (NULL, pce);

    pce = g_new0 (struct proto_chat_entry, 1);
    // TRANSLATOR: Info item *and* dialog item.
    pce->label = _("Join URL or name (empty if creating new):");
    pce->identifier = joinStringKey;
    pce->required = FALSE;
    info = g_list_append (info, pce);

    pce = g_new0 (struct proto_chat_entry, 1);
    // TRANSLATOR: Info item *and* dialog item.
    pce->label = _("Group name (if creating a group):");
    pce->identifier = nameKey;
    pce->required = FALSE;
    info = g_list_append (info, pce);

    pce = g_new0 (struct proto_chat_entry, 1);
    // TRANSLATOR: Info item *and* dialog item.
    pce->label = _("Group to create: 1=small 2=big 3=channel");
    pce->identifier = typeKey;
    pce->required = FALSE;
    pce->is_int = TRUE;
    static_assert((GROUP_TYPE_BASIC > 0) && (GROUP_TYPE_SUPER > 0) && (GROUP_TYPE_CHANNEL > 0), "positive please");
    pce->min = std::min({GROUP_TYPE_BASIC, GROUP_TYPE_SUPER, GROUP_TYPE_CHANNEL});
    pce->max = std::max({GROUP_TYPE_BASIC, GROUP_TYPE_SUPER, GROUP_TYPE_CHANNEL});
    info = g_list_append (info, pce);

    return info;
}

std::string getPurpleChatName(const td::td_api::chat &chat)
{
    return getPurpleChatName(ChatTarget::chat(getId(chat)));
}

std::string getPurpleChatName(ChatTarget target)
{
    if (!target.valid())
        return "";

    if (target.isForumTopic() && target.forumTopicId() != ForumTopicId::general()) {
        return "forum" + std::to_string(target.chatId().value()) +
               "-topic" + std::to_string(target.forumTopicId().value());
    }

    return "chat" + std::to_string(target.chatId().value());
}

GHashTable *getChatComponents(const td::td_api::chat &chat)
{
    return getChatComponents(ChatTarget::chat(getId(chat)));
}

GHashTable *getChatComponents(ChatTarget target)
{
    const std::string name = getPurpleChatName(target);

    GHashTable *table = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
    g_hash_table_insert(table, chatNameComponent, g_strdup(name.c_str()));
    return table;
}

const char *getChatName(GHashTable *components)
{
    return (const char *)g_hash_table_lookup(components, chatNameComponent);
}

const char *getChatJoinString(GHashTable *components)
{
    return (const char *)g_hash_table_lookup(components, joinStringKey);
}

const char *getChatGroupName(GHashTable *components)
{
    return (const char *)g_hash_table_lookup(components, nameKey);
}

int getChatGroupType(GHashTable *components)
{
    const char *s = static_cast<const char *>(g_hash_table_lookup(components, typeKey));
    return s ? atoi(s) : 0;
}

static bool parseCanonicalInt64(const std::string &text, int64_t &value)
{
    if (text.empty() || text[0] == '+')
        return false;

    errno = 0;
    char *end = NULL;
    const long long parsed = strtoll(text.c_str(), &end, 10);
    static_assert(sizeof(parsed) >= sizeof(value), "long long must hold int64_t");
    if (errno || end != text.c_str() + text.size() ||
        parsed < std::numeric_limits<int64_t>::min() ||
        parsed > std::numeric_limits<int64_t>::max() ||
        std::to_string(parsed) != text) {
        return false;
    }

    value = static_cast<int64_t>(parsed);
    return value != 0;
}

ChatTarget parsePurpleChatName(const char *chatName)
{
    if (!chatName)
        return ChatTarget();

    const std::string name(chatName);
    if (name.compare(0, 4, "chat") == 0) {
        int64_t chatIdValue = 0;
        if (!parseCanonicalInt64(name.substr(4), chatIdValue))
            return ChatTarget();

        const ChatId chatId = ChatId::fromString(name.c_str() + 4);
        if (chatId.value() != chatIdValue)
            return ChatTarget();
        return ChatTarget::chat(chatId);
    }

    if (name.compare(0, 5, "forum") == 0) {
        const size_t separator = name.find("-topic", 5);
        if (separator == std::string::npos)
            return ChatTarget();

        int64_t chatIdValue = 0;
        int64_t topicIdValue = 0;
        if (!parseCanonicalInt64(name.substr(5, separator - 5), chatIdValue) ||
            !parseCanonicalInt64(name.substr(separator + 6), topicIdValue) ||
            topicIdValue <= ForumTopicId::general().value() ||
            topicIdValue > std::numeric_limits<int32_t>::max()) {
            return ChatTarget();
        }

        const std::string chatIdText = name.substr(5, separator - 5);
        const ChatId chatId = ChatId::fromString(chatIdText.c_str());
        if (chatId.value() != chatIdValue)
            return ChatTarget();
        return ChatTarget::forumTopic(
            chatId,
            ForumTopicId::fromValue(static_cast<int32_t>(topicIdValue)));
    }

    return ChatTarget();
}

ChatId getTdlibChatId(const char *chatName)
{
    const ChatTarget target = parsePurpleChatName(chatName);
    if (target.valid() && !target.isForumTopic())
        return target.chatId();

    return ChatId::invalid;
}

unsigned getAutoDownloadLimitKb(PurpleAccount *account)
{
    std::string dlLimitStr = purple_account_get_string(account, AccountOptions::AutoDownloadLimit,
                                                       AccountOptions::AutoDownloadLimitDefault);
    for (size_t i = 0; i < dlLimitStr.size(); i++)
        if (dlLimitStr[i] == ',')
            dlLimitStr[i] = '.';
    char *endptr;
    float dlLimit = strtof(dlLimitStr.c_str(), &endptr);

    if (*endptr != '\0') {
        // TRANSLATOR: Buddy-window error message, argument will be a "number".
        std::string message = formatMessage(_("Invalid auto-download limit '{}', resetting to default"), dlLimitStr);
        // TRANSLATOR: Title of a buddy-window error message
        purple_notify_warning(account, _("Download limit"), message.c_str(), NULL);
        purple_account_set_string(account, AccountOptions::AutoDownloadLimit,
                                  AccountOptions::AutoDownloadLimitDefault);
        dlLimit = atof(AccountOptions::AutoDownloadLimitDefault);
    } else if (!std::isfinite(dlLimit) || (dlLimit >= UINT_MAX/1024-1)) {
        purple_account_set_string(account, AccountOptions::AutoDownloadLimit, "0");
        dlLimit = 0;
    }

    return floorf(dlLimit*1024);
}

bool isSizeWithinLimit(unsigned size, unsigned limit)
{
    return (limit == 0) || (size <= limit);
}

bool ignoreBigDownloads(PurpleAccount *account)
{
    return !strcmp(purple_account_get_string(account, AccountOptions::BigDownloadHandling,
                                             AccountOptions::BigDownloadHandlingDefault),
                   AccountOptions::BigDownloadHandlingDiscard);
}

PurpleTdClient *getTdClient(PurpleAccount *account)
{
    PurpleConnection *connection = purple_account_get_connection(account);
    if (connection)
        return static_cast<PurpleTdClient *>(purple_connection_get_protocol_data(connection));
    else
        return NULL;
}

const char *getUiName()
{
    GHashTable *ui_info = purple_core_get_ui_info();
    const char *name = static_cast<char *>(g_hash_table_lookup(ui_info, "name"));
    return name ? name : "";
}

const char *AccountOptions::DownloadBehaviourDefault()
{
    if (!strcasecmp(getUiName(), "pidgin"))
        return AccountOptions::DownloadBehaviourHyperlink;
    else
        return AccountOptions::DownloadBehaviourStandard;
}

bool canDisableReadReceipts()
{
    const char *uiName = getUiName();
    return (!strcasecmp(uiName, "bitlbee") || !strcasecmp(uiName, "spectrum"));
}

bool isReadReceiptsEnabled(PurpleAccount *account)
{
    if (canDisableReadReceipts())
        return (purple_account_get_bool(account, AccountOptions::ReadReceipts,
                                        AccountOptions::ReadReceiptsDefault) != FALSE);
    else
        return true;
}
