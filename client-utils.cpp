#include "client-utils.h"
#include "purple-info.h"
#include "config.h"
#include "format.h"
#include "receiving.h"
#include "file-transfer.h"
#include <server.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <algorithm>
#include <functional>
#include <iterator>
#include <ctime>
#include <limits>

enum {
    MAX_MESSAGE_PARTS = 10,
};

const char *errorCodeMessage()
{
    // TRANSLATOR: In-line error message, appears after a colon (':'), arguments will be a number and some error text from Telegram
    return _("code {0} ({1})");
}

static std::string messageTypeToString(const td::td_api::MessageContent &content)
{
#define C(type) case td::td_api::type::ID: return #type;
    switch (content.get_id()) {
    C(messageText)
    C(messageAnimation)
    C(messageAudio)
    C(messageDocument)
    C(messagePhoto)
    C(messageExpiredPhoto)
    C(messageSticker)
    C(messageVideo)
    C(messageExpiredVideo)
    C(messageVideoNote)
    C(messageVoiceNote)
    C(messageLocation)
    C(messageVenue)
    C(messageContact)
    C(messageAnimatedEmoji)
    C(messageDice)
    C(messageGame)
    C(messagePoll)
    C(messageInvoice)
    C(messageCall)
    C(messageVideoChatScheduled)
    C(messageVideoChatStarted)
    C(messageVideoChatEnded)
    C(messageInviteVideoChatParticipants)
    C(messageBasicGroupChatCreate)
    C(messageSupergroupChatCreate)
    C(messageChatChangeTitle)
    C(messageChatChangePhoto)
    C(messageChatDeletePhoto)
    C(messageChatAddMembers)
    C(messageChatJoinByLink)
    C(messageChatJoinByRequest)
    C(messageChatDeleteMember)
    C(messageChatUpgradeTo)
    C(messageChatUpgradeFrom)
    C(messagePinMessage)
    C(messageScreenshotTaken)
    C(messageChatSetTheme)
    C(messageCustomServiceAction)
    C(messageGameScore)
    C(messagePaymentSuccessful)
    C(messagePaymentSuccessfulBot)
    C(messageContactRegistered)
    C(botWriteAccessAllowReasonConnectedWebsite)
    C(messagePassportDataSent)
    C(messagePassportDataReceived)
    C(messageProximityAlertTriggered)
    C(messageUnsupported)
    }
#undef C
    return "id " + std::to_string(content.get_id());
}

std::string getUnsupportedMessageDescription(const td::td_api::MessageContent &content)
{
    // TRANSLSATOR: In-line placeholder when an unsupported message is being replied to.
    return formatMessage(_("Unsupported message type {}"), messageTypeToString(content));
}

std::string getDisplayedError(const td::td_api::object_ptr<td::td_api::Object> &object)
{
    if (!object) {
        // Dead code at the time of writing this - NULL response can happen if sendQueryWithTimeout
        // was used, but it isn't used in a way that can lead to this message
        return "No response received";
    }
    else if (object->get_id() == td::td_api::error::ID) {
        const td::td_api::error &error = static_cast<const td::td_api::error &>(*object);
        return formatMessage(errorCodeMessage(), {std::to_string(error.code_), error.message_});
    } else {
        // Should not be possible
        return "Unexpected response";
    }
}

std::string proxyTypeToString(PurpleProxyType proxyType)
{
    switch (proxyType) {
    case PURPLE_PROXY_NONE:
    case PURPLE_PROXY_USE_GLOBAL:
    case PURPLE_PROXY_USE_ENVVAR:
        return "unknown";
    case PURPLE_PROXY_HTTP:
        return "HTTP";
    case PURPLE_PROXY_SOCKS4:
        return "SOCKS4";
    case PURPLE_PROXY_SOCKS5:
        return "SOCKS5";
    case PURPLE_PROXY_TOR:
        return "TOR";
    }

    return "unknown";
}

const char *getPurpleStatusId(const td::td_api::UserStatus &tdStatus)
{
    if (tdStatus.get_id() == td::td_api::userStatusOnline::ID)
        return purple_primitive_get_id_from_type(PURPLE_STATUS_AVAILABLE);
    else
        return purple_primitive_get_id_from_type(PURPLE_STATUS_AWAY);
}

std::string getPurpleBuddyName(const td::td_api::user &user)
{
    // Prepend "id" so it's not accidentally equal to our phone number which is account name
    return "id" + std::to_string(user.id_);
}

std::string getSecretChatBuddyName(SecretChatId secretChatId)
{
    return "secret" + std::to_string(secretChatId.value());
}

std::vector<const td::td_api::user *> getUsersByPurpleName(const char *buddyName, TdAccountData &account,
                                                           const char *action)
{
    std::vector<const td::td_api::user *> result;

    UserId userId = purpleBuddyNameToUserId(buddyName);
    if (userId.valid()) {
        const td::td_api::user *tdUser = account.getUser(userId);
        if (tdUser != nullptr)
            result.push_back(tdUser);
        else if (action)
            purple_debug_warning(config::pluginId, "Cannot %s: no user with id %s\n", action, buddyName);
    } else {
        account.getUsersByDisplayName(buddyName, result);
        if (action) {
            if (result.empty())
                purple_debug_warning(config::pluginId, "Cannot %s: no user with display name '%s'\n",
                                    action, buddyName);
            else if (result.size() != 1)
                purple_debug_warning(config::pluginId, "Cannot %s: more than one user with display name '%s'\n",
                                    action, buddyName);
        }
    }

    return result;
}

PurpleConversation *getImConversation(PurpleAccount *account, const char *username)
{
    PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, username, account);
    if (conv == NULL)
        conv = purple_conversation_new(PURPLE_CONV_TYPE_IM, account, username);

    return conv;
}

PurpleConvChat *getChatConversation(TdAccountData &account, const td::td_api::chat &chat,
                                    int chatPurpleId)
{
    std::string chatName       = getPurpleChatName(chat);
    bool        newChatCreated = false;

    // If account logged off with chats open, these chats will be purple_conv_chat_left()'d but not
    // purple_conversation_destroy()'d by purple_connection_destroy. So when logging back in,
    // conversation will exist but not necessarily with correct libpurple id. Therefore, lookup by
    // libpurple id using purple_find_chat cannot be used.
    PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, chatName.c_str(),
                                                                     account.purpleAccount);

    // Such pre-open chats will (unless some other logic intervenes) be inactive (as in,
    // purple_conv_chat_has_left returns true) and if that's the case, serv_got_joined_chat must
    // still be called to make them active and thus able to send or receive messages, because that's
    // the kind of thing we were called for here.
    if ((conv == NULL) || purple_conv_chat_has_left(purple_conversation_get_chat_data(conv))) {
        if (chatPurpleId != 0) {
            purple_debug_misc(config::pluginId, "Creating conversation for chat %s (purple id %d)\n",
                              chat.title_.c_str(), chatPurpleId);
            serv_got_joined_chat(purple_account_get_connection(account.purpleAccount), chatPurpleId, chatName.c_str());
            conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT, chatName.c_str(),
                                                         account.purpleAccount);
            if (conv == NULL)
                purple_debug_warning(config::pluginId, "Did not create conversation for chat %s\n", chat.title_.c_str());
            else {
                // Sometimes when the group has just been created, or we left it and then got
                // messageChatDeleteMember, the chat will not be in buddy list. In that case,
                // libpurpleis going to use chatXXXXXXXXXXX as chat title. Set chat title explicitly
                // to prevent that.
                PurpleChat *purpleChat = purple_blist_find_chat(account.purpleAccount, chatName.c_str());
                if (!purpleChat) {
                    purple_debug_misc(config::pluginId, "Setting conversation title to '%s'\n", chat.title_.c_str());
                    purple_conversation_set_title(conv, chat.title_.c_str());
                }
                newChatCreated = true;
            }

        } else
            purple_debug_warning(config::pluginId, "No internal ID for chat %s\n", chat.title_.c_str());
    }

    if (conv) {
        PurpleConvChat *purpleChat = purple_conversation_get_chat_data(conv);

        if (purpleChat && newChatCreated) {
            BasicGroupId                          basicGroupId = getBasicGroupId(chat);
            const td::td_api::basicGroupFullInfo *groupInfo    = basicGroupId.valid() ? account.getBasicGroupInfo(basicGroupId) : nullptr;
            if (groupInfo)
                updateChatConversation(purpleChat, *groupInfo, account);

            SupergroupId supergroupId = getSupergroupId(chat);
            if (supergroupId.valid()) {
                const td::td_api::supergroupFullInfo *supergroupInfo = account.getSupergroupInfo(supergroupId);
                const td::td_api::chatMembers        *members        = account.getSupergroupMembers(supergroupId);
                if (supergroupInfo)
                    updateChatConversation(purpleChat, *supergroupInfo, account);
                if (members)
                    updateSupergroupChatMembers(purpleChat, *members, account);
            }
        }

        return purpleChat;
    }

    return NULL;
}

PurpleConvChat *findChatConversation(PurpleAccount *account, const td::td_api::chat &chat)
{
    std::string         name = getPurpleChatName(chat);
    PurpleConversation *conv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT,
                                                                     name.c_str(), account);
    if (conv)
        return purple_conversation_get_chat_data(conv);
    return NULL;
}

bool conversationHasFocus(PurpleConversation *conv)
{
    PurpleConversationUiOps *ops = purple_conversation_get_ui_ops(conv);

    // purple_conversation_has_focus return false if this callback is not set, as is the case with
    // bitlbee, but we want to default to sending read receipts
    if ((ops == NULL) || (ops->has_focus == NULL))
        return true;
    else
        return purple_conversation_has_focus(conv);
}

void setBuddyServerAlias(PurpleBuddy *buddy, const char *alias)
{
    purple_blist_server_alias_buddy(buddy, alias);
}

void gotBuddyServerAlias(PurpleAccount *account, const char *buddyName, const char *alias)
{
    PurpleConnection *connection = purple_account_get_connection(account);
    if (connection)
        serv_got_alias(connection, buddyName, alias);
    else {
        PurpleBuddy *buddy = purple_find_buddy(account, buddyName);
        if (buddy)
            setBuddyServerAlias(buddy, alias);
    }
}

void updatePrivateChat(TdAccountData &account, const td::td_api::chat *chat, const td::td_api::user &user)
{
    std::string purpleUserName = getPurpleBuddyName(user);
    std::string alias          = chat ? chat->title_ : makeBasicDisplayName(user);

    PurpleBuddy *buddy = purple_find_buddy(account.purpleAccount, purpleUserName.c_str());
    if (buddy == NULL) {
        purple_debug_misc(config::pluginId, "Adding new buddy %s for user %s\n",
                          alias.c_str(), purpleUserName.c_str());

        const ContactRequest *contactReq = account.findContactRequest(getId(user));
        PurpleGroup          *group      = (contactReq && !contactReq->groupName.empty()) ?
                                           purple_find_group(contactReq->groupName.c_str()) : NULL;
        if (group)
            purple_debug_misc(config::pluginId, "Adding into group %s\n", purple_group_get_name(group));

        buddy = purple_buddy_new(account.purpleAccount, purpleUserName.c_str(), NULL);
        setBuddyServerAlias(buddy, alias.c_str());
        purple_blist_add_buddy(buddy, NULL, group, NULL);
        // If a new buddy has been added here, it means that there was updateNewChat with the private
        // chat. This means either we added them to contacts or started messaging them, or they
        // messaged us. Either way, there is no need to for any extra notification about new contact
        // because the user will be aware anyway.

        // Now, in case this buddy resulted from sending a message to group chat member or from
        // someone new messaging us
        std::string displayName = account.getDisplayName(user);
        PurpleConversation *oldConv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_IM, displayName.c_str(),
                                                                            account.purpleAccount);
        if (oldConv) {
            purple_conv_im_write(purple_conversation_get_im_data(oldConv), "",
                                 // TRANSLATOR: In-chat status update
                                 _("Future messages in this conversation will be shown in a different tab"),
                                 PURPLE_MESSAGE_SYSTEM, time(NULL));
        }
    } else {
        gotBuddyServerAlias(account.purpleAccount, purpleUserName.c_str(), alias.c_str());

        const char *oldPhotoIdStr = purple_blist_node_get_string(PURPLE_BLIST_NODE(buddy), BuddyOptions::ProfilePhotoId);
        int64_t     oldPhotoId    = 0;
        if (oldPhotoIdStr)
            sscanf(oldPhotoIdStr, "%" G_GINT64_FORMAT, &oldPhotoId);
        if (user.profile_photo_ && user.profile_photo_->small_)
        {
            const td::td_api::file &photo = *user.profile_photo_->small_;
            if (photo.local_ && photo.local_->is_downloading_completed_ &&
                (user.profile_photo_->id_ != oldPhotoId))
            {
                gchar  *img = NULL;
                size_t  len;
                GError *err = NULL;
                g_file_get_contents(photo.local_->path_.c_str(), &img, &len, &err);
                if (err) {
                    purple_debug_warning(config::pluginId, "Failed to load profile photo %s for %s: %s\n",
                                         photo.local_->path_.c_str(), purpleUserName.c_str(),  err->message);
                    g_error_free(err);
                } else {
                    std::string newPhotoIdStr = std::to_string(user.profile_photo_->id_);
                    purple_blist_node_set_string(PURPLE_BLIST_NODE(buddy), BuddyOptions::ProfilePhotoId,
                                                 newPhotoIdStr.c_str());
                    purple_debug_info(config::pluginId, "Loaded new profile photo for %s (id %s)\n",
                                      purpleUserName.c_str(), newPhotoIdStr.c_str());
                    purple_buddy_icons_set_for_user(account.purpleAccount, purpleUserName.c_str(),
                                                    img, len, NULL);
                }
            }
        } else if (oldPhotoId) {
            purple_debug_info(config::pluginId, "Removing profile photo from %s\n", purpleUserName.c_str());
            purple_blist_node_remove_setting(PURPLE_BLIST_NODE(buddy), BuddyOptions::ProfilePhotoId);
            purple_buddy_icons_set_for_user(account.purpleAccount, purpleUserName.c_str(), NULL, 0, NULL);
        }
    }
}

static void updateGroupChat(TdAccountData &account, const td::td_api::chat &chat,
                            const td::td_api::object_ptr<td::td_api::ChatMemberStatus> &groupStatus,
                            const char *groupType, const std::string &groupId)
{
    if (!isGroupMember(groupStatus)) {
        purpleDebug("Skipping {} {} because we are not a member", {std::string(groupType), groupId});
        return;
    }

    std::string  chatName   = getPurpleChatName(chat);
    PurpleChat  *purpleChat = purple_blist_find_chat(account.purpleAccount, chatName.c_str());
    if (!purpleChat) {
        purpleDebug("Adding new chat for {} {} ({})", {std::string(groupType), groupId, chat.title_});
        purpleChat = purple_chat_new(account.purpleAccount, chat.title_.c_str(), getChatComponents(chat));
        purple_blist_add_chat(purpleChat, NULL, NULL);
    } else {
        const char *oldName = purple_chat_get_name(purpleChat);
        if (chat.title_ != oldName) {
            purple_debug_misc(config::pluginId, "Renaming chat '%s' to '%s'\n", oldName, chat.title_.c_str());
            purple_blist_alias_chat(purpleChat, chat.title_.c_str());
        }
    }

    if (account.isExpectedChat(getId(chat))) {
        PurpleConversation *baseConv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT,
                                                                            chatName.c_str(), account.purpleAccount);
        if (baseConv && purple_conv_chat_has_left(purple_conversation_get_chat_data(baseConv))) {
            purple_debug_misc(config::pluginId, "Rejoining chat %s as previously requested\n", chatName.c_str());
            serv_got_joined_chat(purple_account_get_connection(account.purpleAccount),
                                 account.getPurpleChatId(getId(chat)), chatName.c_str());
        }
        account.removeExpectedChat(getId(chat));
    }

    const char *oldPhotoId = purple_blist_node_get_string(PURPLE_BLIST_NODE(purpleChat), BuddyOptions::ProfilePhotoId);
    if (chat.photo_ && chat.photo_->small_)
    {
        const td::td_api::file &photo = *chat.photo_->small_;
        if (photo.local_ && photo.local_->is_downloading_completed_ && photo.remote_ &&
            !photo.remote_->unique_id_.empty() && (!oldPhotoId || (photo.remote_->unique_id_ != oldPhotoId)))
        {
            gchar  *img = NULL;
            size_t  len;
            GError *err = NULL;
            g_file_get_contents(photo.local_->path_.c_str(), &img, &len, &err);
            if (err) {
                purple_debug_warning(config::pluginId, "Failed to load chat photo %s for %s: %s\n",
                                        photo.local_->path_.c_str(), chat.title_.c_str(),  err->message);
                g_error_free(err);
            } else {
                purple_blist_node_set_string(PURPLE_BLIST_NODE(purpleChat), BuddyOptions::ProfilePhotoId,
                                             photo.remote_->unique_id_.c_str());
                purple_debug_info(config::pluginId, "Loaded new chat photo for %s (id %s)\n",
                                  chat.title_.c_str(), photo.remote_->unique_id_.c_str());
                purple_buddy_icons_node_set_custom_icon(PURPLE_BLIST_NODE(purpleChat),
                                                        reinterpret_cast<guchar *>(img), len);
            }
        }
    } else if (oldPhotoId) {
        purple_debug_info(config::pluginId, "Removing chat photo from %s\n", chat.title_.c_str());
        purple_blist_node_remove_setting(PURPLE_BLIST_NODE(purpleChat), BuddyOptions::ProfilePhotoId);
        purple_buddy_icons_node_set_custom_icon(PURPLE_BLIST_NODE(purpleChat), NULL, 0);
    }
}

void updateBasicGroupChat(TdAccountData &account, BasicGroupId groupId)
{
    const td::td_api::basicGroup *group = account.getBasicGroup(groupId);
    const td::td_api::chat       *chat  = account.getBasicGroupChatByGroup(groupId);

    if (!group)
        purpleDebug("Basic group {} does not exist yet\n", groupId.value());
    else if (!chat)
        purpleDebug("Chat for basic group {} does not exist yet\n", groupId.value());
    else
        updateGroupChat(account, *chat, group->status_, "basic group", std::to_string(groupId.value()));
}

void updateSupergroupChat(TdAccountData &account, SupergroupId groupId)
{
    const td::td_api::supergroup *group = account.getSupergroup(groupId);
    const td::td_api::chat       *chat  = account.getSupergroupChatByGroup(groupId);

    if (!group)
        purpleDebug("Supergroup {} does not exist yet\n", groupId.value());
    else if (!chat)
        purpleDebug("Chat for supergroup {} does not exist yet\n", groupId.value());
    else
        updateGroupChat(account, *chat, group->status_, "supergroup", std::to_string(groupId.value()));
}

bool isInviteLinkActive(const td::td_api::chatInviteLink &linkInfo)
{
    return !linkInfo.is_revoked_ &&
        ((linkInfo.member_limit_ == 0) || (linkInfo.member_count_ < linkInfo.member_limit_)) &&
        ((linkInfo.expiration_date_ == 0) || (std::time(NULL) < static_cast<time_t>(linkInfo.expiration_date_)));
}

static std::string lastMessageSetting(ChatId chatId)
{
    return "last-message-chat" + std::to_string(chatId.value());
}

void removeGroupChat(PurpleAccount *purpleAccount, const td::td_api::chat &chat)
{
    std::string  chatName   = getPurpleChatName(chat);
    PurpleChat  *purpleChat = purple_blist_find_chat(purpleAccount, chatName.c_str());

    if (purpleChat)
        purple_blist_remove_chat(purpleChat);
    // TODO: uncomment when updateNewChat(chat_list=NULL) + updateChatChatList(non-NULL) at login
    // no longer removes chat
    //std::string setting = lastMessageSetting(getId(chat));
    //purple_account_remove_setting(purpleAccount, setting.c_str());
}

void removePrivateChat(TdAccountData &account, const td::td_api::chat &chat)
{
    // TODO: uncomment when updateNewChat(chat_list=NULL) + updateChatChatList(non-NULL) at login
    // no longer removes chat
    //std::string setting = lastMessageSetting(getId(chat));
    //purple_account_remove_setting(account.purpleAccount, setting.c_str());
}

void saveChatLastMessage(TdAccountData &account, ChatId chatId, MessageId messageId)
{
    std::string setting = lastMessageSetting(chatId);
    std::string value = std::to_string(messageId.value());
    purple_account_set_string(account.purpleAccount, setting.c_str(), value.c_str());
}

MessageId getChatLastMessage(TdAccountData &account, ChatId chatId)
{
    std::string setting = lastMessageSetting(chatId);
    const char *value = purple_account_get_string(account.purpleAccount, setting.c_str(), NULL);

    return value ? MessageId::fromString(value) : MessageId();
}

std::string makeBasicDisplayName(const td::td_api::user &user)
{
    std::string result = user.first_name_;
    if (!result.empty() && !user.last_name_.empty())
        result += ' ';
    result += user.last_name_;

    return result;
}

std::string getIncomingGroupchatSenderPurpleName(const td::td_api::chat &chat, const td::td_api::message &message,
                                                 const TdAccountData &account)
{
    if (!message.is_outgoing_ && (getBasicGroupId(chat).valid() || getSupergroupId(chat).valid())) {
        UserId senderId = getSenderUserId(message);
        if (senderId.valid())
            return account.getDisplayName(senderId);
        else if (!message.author_signature_.empty())
            return message.author_signature_;
        else if (message.is_channel_post_) {
            // TRANSLATOR: The "sender" of a message that was posted to a channel. Will be used like a username.
            return _("Channel post");
        } else if (message.forward_info_ && message.forward_info_->origin_)
            switch (message.forward_info_->origin_->get_id()) {
            case td::td_api::messageOriginUser::ID:
                return account.getDisplayName(getSenderUserId(static_cast<const td::td_api::messageOriginUser &>(*message.forward_info_->origin_)));
            case td::td_api::messageOriginHiddenUser::ID:
                return static_cast<const td::td_api::messageOriginHiddenUser &>(*message.forward_info_->origin_).sender_name_;
            case td::td_api::messageOriginChannel::ID:
                return static_cast<const td::td_api::messageOriginChannel&>(*message.forward_info_->origin_).author_signature_;
            }
    }

    // For outgoing messages, our name will be used instead
    // For private and secret chats, sender name will be determined from the chat instead

    return "";
}

std::string getForwardSource(const td::td_api::messageForwardInfo &forwardInfo,
                             const TdAccountData &account)
{
    if (!forwardInfo.origin_)
        return "";

    switch (forwardInfo.origin_->get_id()) {
        case td::td_api::messageOriginUser::ID:
            return account.getDisplayName(getSenderUserId(static_cast<const td::td_api::messageOriginUser &>(*forwardInfo.origin_)));
        case td::td_api::messageOriginHiddenUser::ID:
            return static_cast<const td::td_api::messageOriginHiddenUser &>(*forwardInfo.origin_).sender_name_;
        case td::td_api::messageOriginChannel::ID: {
            const td::td_api::chat *chat = account.getChat(getChatId(static_cast<const td::td_api::messageOriginChannel&>(*forwardInfo.origin_)));
            if (chat)
                return chat->title_;
        }
    }

    return "";
}

void getNamesFromAlias(const char *alias, std::string &firstName, std::string &lastName)
{
    if (!alias) alias = "";

    const char *name1end = alias;
    while (*name1end && isspace(static_cast<unsigned char>(*name1end))) name1end++;
    while (*name1end && !isspace(static_cast<unsigned char>(*name1end))) name1end++;
    firstName = std::string(alias, name1end-alias);

    const char *name2start = name1end;
    while (*name2start && isspace(static_cast<unsigned char>(*name2start))) name2start++;
    lastName = name2start;
}

static void findChatsByComponents(PurpleBlistNode *node,
                                  const char *joinString, const char *groupName, int groupType,
                                  std::vector<PurpleChat *> &result)
{
    PurpleBlistNodeType nodeType = purple_blist_node_get_type(node);

    if (nodeType == PURPLE_BLIST_CHAT_NODE) {
        PurpleChat *chat           = PURPLE_CHAT(node);
        GHashTable *components     = purple_chat_get_components(chat);
        const char *nodeName       = getChatName(components);
        const char *nodeJoinString = getChatJoinString(components);
        const char *nodeGroupName  = getChatGroupName(components);
        int         nodeGroupType  = getChatGroupType(components);

        if (!nodeName) nodeName = "";
        if (!nodeJoinString) nodeJoinString = "";
        if (!nodeGroupName) nodeGroupName = "";

        if (!strcmp(nodeName, "") && !strcmp(nodeJoinString, joinString)) {
            if ((*joinString != '\0') ||
                (!strcmp(nodeGroupName, groupName) && (nodeGroupType == groupType)))
            {
                result.push_back(chat);
            }
        }
    }

    for (PurpleBlistNode *child = purple_blist_node_get_first_child(node); child;
         child = purple_blist_node_get_sibling_next(child))
    {
        findChatsByComponents(child, joinString, groupName, groupType, result);
    }
}

std::vector<PurpleChat *>findChatsByJoinString(const std::string &joinString)
{
    std::vector<PurpleChat *> result;

    for (PurpleBlistNode *root = purple_blist_get_root(); root;
         root = purple_blist_node_get_sibling_next(root)) // LOL
    {
        findChatsByComponents(root, joinString.c_str(), "", 0, result);
    }

    return result;
}

std::vector<PurpleChat *> findChatsByNewGroup(const char *name, int type)
{
    std::vector<PurpleChat *> result;

    for (PurpleBlistNode *root = purple_blist_get_root(); root;
         root = purple_blist_node_get_sibling_next(root)) // LOL
    {
        findChatsByComponents(root, "", name, type, result);
    }

    return result;
}

std::string getChatMemberPurpleName(UserId userId, const TdAccountData &account)
{
    const td::td_api::user *user = account.getUser(userId);
    if (!user || (user->type_ && (user->type_->get_id() == td::td_api::userTypeDeleted::ID)))
        return "";

    std::string userName    = getPurpleBuddyName(*user);
    const char *phoneNumber = getCanonicalPhoneNumber(user->phone_number_.c_str());
    if (purple_find_buddy(account.purpleAccount, userName.c_str()))
        // libpurple will be able to map user name to alias because there is a buddy
        return userName;
    else if (!strcmp(getCanonicalPhoneNumber(purple_account_get_username(account.purpleAccount)), phoneNumber))
        // This is us, so again libpurple will map phone number to alias
        return purple_account_get_username(account.purpleAccount);
    else
        // Use first and last name instead
        return account.getDisplayName(*user);
}

PurpleConvChatBuddyFlags getChatMemberFlags(const td::td_api::object_ptr<td::td_api::ChatMemberStatus> &status)
{
    if (!status)
        return PURPLE_CBFLAGS_NONE;
    if (status->get_id() == td::td_api::chatMemberStatusCreator::ID)
        return PURPLE_CBFLAGS_FOUNDER;
    if (status->get_id() == td::td_api::chatMemberStatusAdministrator::ID)
        return PURPLE_CBFLAGS_OP;
    return PURPLE_CBFLAGS_NONE;
}

void addChatMember(PurpleConvChat *purpleChat, UserId userId,
                   const td::td_api::object_ptr<td::td_api::ChatMemberStatus> &status,
                   const TdAccountData &account, bool newArrival)
{
    if (!purpleChat || !isGroupMember(status))
        return;

    std::string memberName = getChatMemberPurpleName(userId, account);
    if (memberName.empty())
        return;

    PurpleConvChatBuddyFlags flags = getChatMemberFlags(status);
    if (purple_conv_chat_find_user(purpleChat, memberName.c_str()))
        purple_conv_chat_user_set_flags(purpleChat, memberName.c_str(), flags);
    else
        purple_conv_chat_add_user(purpleChat, memberName.c_str(), NULL, flags, newArrival);
}

void removeChatMember(PurpleConvChat *purpleChat, UserId userId, const TdAccountData &account,
                      const char *reason)
{
    if (!purpleChat)
        return;

    std::string memberName = getChatMemberPurpleName(userId, account);
    if (!memberName.empty() && purple_conv_chat_find_user(purpleChat, memberName.c_str()))
        purple_conv_chat_remove_user(purpleChat, memberName.c_str(), reason);
}

void updateChatMember(PurpleConvChat *purpleChat, const td::td_api::chatMember *oldMember,
                      const td::td_api::chatMember *newMember, const TdAccountData &account)
{
    UserId oldUserId = oldMember ? getUserId(*oldMember) : UserId::invalid;
    UserId newUserId = newMember ? getUserId(*newMember) : UserId::invalid;
    bool   oldMemberActive = oldMember && isGroupMember(oldMember->status_);
    bool   newMemberActive = newMember && isGroupMember(newMember->status_);

    if (oldMemberActive && (!newMemberActive || (oldUserId != newUserId)))
        removeChatMember(purpleChat, oldUserId, account);
    if (newMemberActive)
        addChatMember(purpleChat, newUserId, newMember->status_, account, !oldMemberActive);
}

static void setChatMembers(PurpleConvChat *purpleChat,
                           const std::vector<td::td_api::object_ptr<td::td_api::chatMember>> &members,
                           const TdAccountData &account)
{
    GList *flags = NULL;
    std::vector<std::string> nameData;

    for (const auto &member: members) {
        if (!member || !isGroupMember(member->status_))
            continue;

        std::string memberName = getChatMemberPurpleName(getUserId(*member), account);
        if (memberName.empty())
            continue;

        nameData.emplace_back(memberName);
        flags = g_list_append(flags, GINT_TO_POINTER(getChatMemberFlags(member->status_)));
    }

    GList *names = NULL;
    for (const std::string &name: nameData)
        names = g_list_append(names, const_cast<char *>(name.c_str()));

    purple_conv_chat_clear_users(purpleChat);
    purple_conv_chat_add_users(purpleChat, names, NULL, flags, false);
    g_list_free(names);
    g_list_free(flags);
}

void updateChatConversation(PurpleConvChat *purpleChat, const td::td_api::basicGroupFullInfo &groupInfo,
                    const TdAccountData &account)
{
    purple_conv_chat_set_topic(purpleChat, NULL, groupInfo.description_.c_str());
    setChatMembers(purpleChat, groupInfo.members_, account);
}

void updateChatConversation(PurpleConvChat *purpleChat, const td::td_api::supergroupFullInfo &groupInfo,
                    const TdAccountData &account)
{
    purple_conv_chat_set_topic(purpleChat, NULL, groupInfo.description_.c_str());
}

void updateSupergroupChatMembers(PurpleConvChat* purpleChat, const td::td_api::chatMembers& members,
                                 const TdAccountData& account)
{
    setChatMembers(purpleChat, members.members_, account);
}

struct MessagePart {
    bool        isImage = false;
    int         imageId = 0;
    std::string text;

    enum class EntityKind: uint8_t {
        Bold,
        Italic,
        Underline,
        Strikethrough,
        Code,
        Pre,
        PreCode,
        BlockQuote,
        Spoiler,
        TextUrl,
        EmailAddress,
        MentionName,
    };

    struct Entity {
        int32_t     offset = 0;
        int32_t     length = 0;
        EntityKind  kind;
        std::string argument;
    };
    std::vector<Entity> entities;
};

static size_t splitTextChunk(MessagePart &part, const char *text, size_t length, TdAccountData &account)
{
    enum {MIN_LENGTH_LIMIT = 8};
    unsigned lengthLimit = part.isImage ? account.options.maxCaptionLength : account.options.maxMessageLength;
    if (lengthLimit == 0)
        purple_debug_warning(config::pluginId, "No %s length limit\n", part.isImage ? "caption" : "message");
    else if (lengthLimit <= MIN_LENGTH_LIMIT)
        purple_debug_warning(config::pluginId, "%u is a ridiculous %s length limit\n",
                             lengthLimit, part.isImage ? "caption" : "message");
    if ((lengthLimit <= MIN_LENGTH_LIMIT) || (length <= lengthLimit)) {
        part.text = std::string(text, length);
        return length;
    }

    // Try to truncate at a line break, with no lower length limit in case of image caption
    unsigned newlineSplitLowerLimit = part.isImage ? 1 : lengthLimit/2;
    for (unsigned chunkLength = lengthLimit; chunkLength >= newlineSplitLowerLimit; chunkLength--)
        if (text[chunkLength-1] == '\n') {
            part.text = std::string(text, chunkLength-1);
            return chunkLength;
        }

    // Try to truncate to a whole number of utf-8 characters
    size_t      chunkLen;
    const char *pos = g_utf8_find_prev_char(text, text+lengthLimit);
    if (pos != NULL) {
        const char *next = g_utf8_find_next_char(pos, NULL);
        if (next == text+lengthLimit)
            chunkLen = lengthLimit;
        else
            chunkLen = pos-text;
    } else
        chunkLen = lengthLimit;
    part.text = std::string(text, chunkLen);
    return chunkLen;
}

static int32_t utf16Length(const char *text, size_t length)
{
    int32_t     result = 0;
    const char *p      = text;
    const char *end    = text + length;
    while (p < end) {
        gunichar ch = g_utf8_get_char_validated(p, end - p);
        if ((ch == (gunichar)-1) || (ch == (gunichar)-2)) {
            p++;
            result++;
        } else {
            result += (ch > 0xffff) ? 2 : 1;
            p = g_utf8_next_char(p);
        }
    }
    return result;
}

static std::string asciiLower(std::string s)
{
    for (char &c: s)
        c = tolower(static_cast<unsigned char>(c));
    return s;
}

static std::string unescapeHtml(const char *text, size_t len)
{
    std::string source(text, len);
    char *unescaped = purple_unescape_html(source.c_str());
    std::string result = unescaped ? unescaped : "";
    g_free(unescaped);
    return result;
}

static std::string getTagName(const std::string &tag)
{
    size_t pos = 0;
    while ((pos < tag.size()) && isspace(static_cast<unsigned char>(tag[pos])))
        pos++;
    if ((pos < tag.size()) && (tag[pos] == '/'))
        pos++;
    while ((pos < tag.size()) && isspace(static_cast<unsigned char>(tag[pos])))
        pos++;

    size_t nameStart = pos;
    while ((pos < tag.size()) &&
           (isalnum(static_cast<unsigned char>(tag[pos])) || (tag[pos] == '-')))
        pos++;
    return asciiLower(tag.substr(nameStart, pos - nameStart));
}

static bool isClosingTag(const std::string &tag)
{
    size_t pos = 0;
    while ((pos < tag.size()) && isspace(static_cast<unsigned char>(tag[pos])))
        pos++;
    return (pos < tag.size()) && (tag[pos] == '/');
}

static std::string getTagAttribute(const std::string &tag, const char *name)
{
    std::string lowerName = asciiLower(name);
    size_t pos = 0;
    while ((pos < tag.size()) && isspace(static_cast<unsigned char>(tag[pos])))
        pos++;
    if ((pos < tag.size()) && (tag[pos] == '/'))
        pos++;
    while ((pos < tag.size()) && isspace(static_cast<unsigned char>(tag[pos])))
        pos++;

    while ((pos < tag.size()) &&
           (isalnum(static_cast<unsigned char>(tag[pos])) || (tag[pos] == '-')))
        pos++;

    while (pos < tag.size()) {
        while ((pos < tag.size()) && isspace(static_cast<unsigned char>(tag[pos])))
            pos++;
        if ((pos >= tag.size()) || (tag[pos] == '/'))
            break;

        size_t attrStart = pos;
        while ((pos < tag.size()) &&
               (isalnum(static_cast<unsigned char>(tag[pos])) ||
                (tag[pos] == '-') || (tag[pos] == '_') || (tag[pos] == ':')))
            pos++;
        if (pos == attrStart) {
            pos++;
            continue;
        }

        std::string attrName = asciiLower(tag.substr(attrStart, pos - attrStart));
        while ((pos < tag.size()) && isspace(static_cast<unsigned char>(tag[pos])))
            pos++;
        if ((pos >= tag.size()) || (tag[pos] != '='))
            continue;
        pos++;
        while ((pos < tag.size()) && isspace(static_cast<unsigned char>(tag[pos])))
            pos++;

        size_t valueStart = pos;
        size_t valueEnd = pos;
        if ((pos < tag.size()) && ((tag[pos] == '"') || (tag[pos] == '\''))) {
            char quote = tag[pos++];
            valueStart = pos;
            valueEnd = tag.find(quote, pos);
            if (valueEnd == std::string::npos) {
                valueEnd = tag.size();
                pos = tag.size();
            } else {
                pos = valueEnd + 1;
            }
        } else {
            while ((pos < tag.size()) && !isspace(static_cast<unsigned char>(tag[pos])))
                pos++;
            valueEnd = pos;
        }

        if (attrName == lowerName) {
            std::string value = tag.substr(valueStart, valueEnd - valueStart);
            return unescapeHtml(value.c_str(), value.size());
        }
    }
    return "";
}

static bool isSupportedTextUrl(const std::string &url)
{
    std::string lowerUrl = asciiLower(url);
    return !lowerUrl.compare(0, 7, "http://") ||
           !lowerUrl.compare(0, 8, "https://") ||
           !lowerUrl.compare(0, 5, "tg://");
}

static bool isSimpleEmailAddress(const std::string &email)
{
    size_t at = email.find('@');
    if ((at == std::string::npos) || (at == 0) || (at == email.size() - 1))
        return false;
    if (email.find('@', at + 1) != std::string::npos)
        return false;

    for (char c: email) {
        unsigned char ch = static_cast<unsigned char>(c);
        if ((ch <= ' ') || (c == '<') || (c == '>') || (c == '"'))
            return false;
    }
    return true;
}

static bool getMailtoAddress(const std::string &url, std::string &email)
{
    if (asciiLower(url).compare(0, 7, "mailto:"))
        return false;
    email = url.substr(7);
    return isSimpleEmailAddress(email);
}

static bool parsePositiveInt64(const std::string &s, int64_t &value)
{
    if (s.empty())
        return false;

    int64_t result = 0;
    for (char c: s) {
        if (!isdigit(static_cast<unsigned char>(c)))
            return false;
        int digit = c - '0';
        if (result > (std::numeric_limits<int64_t>::max() - digit) / 10)
            return false;
        result = result * 10 + digit;
    }
    if (result <= 0)
        return false;

    value = result;
    return true;
}

static bool getMentionNameUserId(const std::string &url, int64_t &userId)
{
    const char prefix[] = "tg://user?id=";
    std::string lowerUrl = asciiLower(url);
    if (lowerUrl.compare(0, sizeof(prefix) - 1, prefix))
        return false;
    return parsePositiveInt64(url.substr(sizeof(prefix) - 1), userId);
}

static bool isGeneratedSpoilerStyle(const std::string &style)
{
    return asciiLower(style) == "background-color:#000000;color:#000000";
}

static bool tagToEntityKind(const std::string &name, const std::string &tag, bool closing,
                            MessagePart::EntityKind &kind, std::string &argument)
{
    argument.clear();
    if ((name == "b") || (name == "strong"))
        kind = MessagePart::EntityKind::Bold;
    else if ((name == "i") || (name == "em"))
        kind = MessagePart::EntityKind::Italic;
    else if (name == "u")
        kind = MessagePart::EntityKind::Underline;
    else if ((name == "s") || (name == "strike") || (name == "del"))
        kind = MessagePart::EntityKind::Strikethrough;
    else if (name == "code")
        kind = MessagePart::EntityKind::Code;
    else if (name == "pre")
        kind = MessagePart::EntityKind::Pre;
    else if (name == "blockquote")
        kind = MessagePart::EntityKind::BlockQuote;
    else if (name == "span") {
        if (!closing && !isGeneratedSpoilerStyle(getTagAttribute(tag, "style")))
            return false;
        kind = MessagePart::EntityKind::Spoiler;
    }
    else if (name == "a") {
        argument = closing ? "" : getTagAttribute(tag, "href");
        if (!closing) {
            std::string email;
            int64_t userId = 0;
            if (getMailtoAddress(argument, email)) {
                kind = MessagePart::EntityKind::EmailAddress;
                argument = email;
            } else if (getMentionNameUserId(argument, userId)) {
                kind = MessagePart::EntityKind::MentionName;
                argument = std::to_string(userId);
            } else if (isSupportedTextUrl(argument)) {
                kind = MessagePart::EntityKind::TextUrl;
            } else {
                return false;
            }
        } else {
            kind = MessagePart::EntityKind::TextUrl;
        }
    } else
        return false;

    return true;
}

static td::td_api::object_ptr<td::td_api::TextEntityType>
makeTextEntityType(MessagePart::EntityKind kind, const std::string &argument)
{
    switch (kind) {
        case MessagePart::EntityKind::Bold:
            return td::td_api::make_object<td::td_api::textEntityTypeBold>();
        case MessagePart::EntityKind::Italic:
            return td::td_api::make_object<td::td_api::textEntityTypeItalic>();
        case MessagePart::EntityKind::Underline:
            return td::td_api::make_object<td::td_api::textEntityTypeUnderline>();
        case MessagePart::EntityKind::Strikethrough:
            return td::td_api::make_object<td::td_api::textEntityTypeStrikethrough>();
        case MessagePart::EntityKind::Code:
            return td::td_api::make_object<td::td_api::textEntityTypeCode>();
        case MessagePart::EntityKind::Pre:
            return td::td_api::make_object<td::td_api::textEntityTypePre>();
        case MessagePart::EntityKind::PreCode:
            return td::td_api::make_object<td::td_api::textEntityTypePreCode>("");
        case MessagePart::EntityKind::BlockQuote:
            return td::td_api::make_object<td::td_api::textEntityTypeBlockQuote>();
        case MessagePart::EntityKind::Spoiler:
            return td::td_api::make_object<td::td_api::textEntityTypeSpoiler>();
        case MessagePart::EntityKind::TextUrl:
            return td::td_api::make_object<td::td_api::textEntityTypeTextUrl>(argument);
        case MessagePart::EntityKind::EmailAddress:
            return td::td_api::make_object<td::td_api::textEntityTypeEmailAddress>();
        case MessagePart::EntityKind::MentionName: {
            int64_t userId = 0;
            if (!parsePositiveInt64(argument, userId))
                return nullptr;
            return td::td_api::make_object<td::td_api::textEntityTypeMentionName>(userId);
        }
    }

    return nullptr;
}

struct ParsedText {
    struct OpenEntity {
        MessagePart::EntityKind kind;
        std::string             argument;
        int32_t                 startOffset;
        size_t                  startByteOffset;
    };

    std::string text;
    int32_t     utf16Length = 0;
    std::vector<MessagePart::Entity> entities;
    std::vector<OpenEntity>          openEntities;
};

static void appendParsedText(ParsedText &parsed, const char *text, size_t len)
{
    if (len == 0)
        return;

    std::string decoded = unescapeHtml(text, len);
    parsed.text += decoded;
    parsed.utf16Length += utf16Length(decoded.c_str(), decoded.size());
}

static void addParsedEntity(ParsedText &parsed, MessagePart::EntityKind kind,
                            const std::string &argument, int32_t startOffset,
                            size_t startByteOffset)
{
    int32_t length = parsed.utf16Length - startOffset;
    if (length <= 0)
        return;

    if ((kind == MessagePart::EntityKind::EmailAddress) &&
        (parsed.text.substr(startByteOffset) != argument))
        return;

    MessagePart::Entity entity;
    entity.offset   = startOffset;
    entity.length   = length;
    entity.kind     = kind;
    entity.argument = argument;
    parsed.entities.push_back(entity);
}

static bool entityKindMatchesClosing(MessagePart::EntityKind openKind,
                                     MessagePart::EntityKind closingKind)
{
    if (closingKind == MessagePart::EntityKind::TextUrl)
        return (openKind == MessagePart::EntityKind::TextUrl) ||
               (openKind == MessagePart::EntityKind::EmailAddress) ||
               (openKind == MessagePart::EntityKind::MentionName);
    return openKind == closingKind;
}

static void combinePreCodeEntities(ParsedText &parsed)
{
    std::vector<bool> remove(parsed.entities.size(), false);
    for (size_t pre = 0; pre < parsed.entities.size(); pre++) {
        if (parsed.entities[pre].kind != MessagePart::EntityKind::Pre)
            continue;

        for (size_t code = 0; code < parsed.entities.size(); code++) {
            const MessagePart::Entity &entity = parsed.entities[code];
            bool matchingCode = !remove[code] &&
                                (entity.kind == MessagePart::EntityKind::Code) &&
                                (entity.offset == parsed.entities[pre].offset) &&
                                (entity.length == parsed.entities[pre].length);
            if (matchingCode) {
                parsed.entities[pre].kind = MessagePart::EntityKind::PreCode;
                remove[code] = true;
                break;
            }
        }
    }

    if (std::find(remove.begin(), remove.end(), true) == remove.end())
        return;

    std::vector<MessagePart::Entity> kept;
    kept.reserve(parsed.entities.size());
    for (size_t i = 0; i < parsed.entities.size(); i++)
        if (!remove[i])
            kept.push_back(std::move(parsed.entities[i]));
    parsed.entities = std::move(kept);
}

static ParsedText parseRichText(const char *text, size_t len)
{
    ParsedText  parsed;
    const char *s         = text;
    const char *end       = text + len;
    const char *textStart = text;

    while (s < end) {
        if (*s != '<') {
            s++;
            continue;
        }

        const char *tagEnd = static_cast<const char *>(memchr(s, '>', end - s));
        if (!tagEnd)
            break;

        appendParsedText(parsed, textStart, s - textStart);

        std::string tag(s + 1, tagEnd - s - 1);
        std::string name = getTagName(tag);
        if ((name == "br") || (name == "br/")) {
            parsed.text += '\n';
            parsed.utf16Length++;
        } else {
            MessagePart::EntityKind kind;
            std::string argument;
            bool closing = isClosingTag(tag);
            if (tagToEntityKind(name, tag, closing, kind, argument)) {
                if (closing) {
                    auto it = std::find_if(parsed.openEntities.rbegin(), parsed.openEntities.rend(),
                        [kind](const ParsedText::OpenEntity &entity) {
                            return entityKindMatchesClosing(entity.kind, kind);
                        });
                    if (it != parsed.openEntities.rend()) {
                        addParsedEntity(parsed, it->kind, it->argument,
                                        it->startOffset, it->startByteOffset);
                        parsed.openEntities.erase(std::next(it).base());
                    }
                } else {
                    ParsedText::OpenEntity entity;
                    entity.kind            = kind;
                    entity.argument        = argument;
                    entity.startOffset     = parsed.utf16Length;
                    entity.startByteOffset = parsed.text.size();
                    parsed.openEntities.push_back(entity);
                }
            }
        }

        s = tagEnd + 1;
        textStart = s;
    }

    appendParsedText(parsed, textStart, end - textStart);
    for (auto it = parsed.openEntities.rbegin(); it != parsed.openEntities.rend(); ++it)
        addParsedEntity(parsed, it->kind, it->argument, it->startOffset, it->startByteOffset);
    combinePreCodeEntities(parsed);
    return parsed;
}

static void setChunkEntities(MessagePart &part, const std::vector<MessagePart::Entity> &entities,
                             int32_t chunkOffset, int32_t chunkLength)
{
    int32_t chunkEnd = chunkOffset + chunkLength;
    part.entities.clear();
    for (const MessagePart::Entity &entity: entities) {
        int32_t entityEnd = entity.offset + entity.length;
        int32_t start     = std::max(entity.offset, chunkOffset);
        int32_t end       = std::min(entityEnd, chunkEnd);
        if (end <= start)
            continue;

        MessagePart::Entity newEntity = entity;
        newEntity.offset = start - chunkOffset;
        newEntity.length = end - start;
        part.entities.push_back(newEntity);
    }
}

static void appendText(std::vector<MessagePart> &parts, const char *s, size_t len, TdAccountData &account)
{
    if (len != 0) {
        ParsedText parsed = parseRichText(s, len);
        if (parsed.text.empty())
            return;

        if (parts.empty())
            parts.emplace_back();

        const char *remaining    = parsed.text.c_str();
        size_t      lenRemaining = parsed.text.size();
        int32_t     chunkOffset  = 0;
        while (lenRemaining) {
            size_t chunkLength = splitTextChunk(parts.back(), remaining, lenRemaining, account);
            int32_t chunkUtf16Length = utf16Length(remaining, chunkLength);
            setChunkEntities(parts.back(), parsed.entities, chunkOffset, chunkUtf16Length);
            lenRemaining -= chunkLength;
            remaining += chunkLength;
            chunkOffset += chunkUtf16Length;
            if (lenRemaining)
                parts.emplace_back();
        }
    }
}

static void parseMessage(const char *message, std::vector<MessagePart> &parts, TdAccountData &account)
{
    parts.clear();
    if (!message)
        return;

    const char *s         = message;
    const char *textStart = message;

    while (*s) {
        bool  isImage = false;
        long  imageId;
        char *pastImage = NULL;

        if (!strncasecmp(s, "<img id=\"", 9)) {
            const char *idString = s+9;

            imageId = strtol(idString, &pastImage, 10);
            if ((pastImage != idString) && !strncmp(pastImage, "\">", 2) && (imageId <= INT32_MAX) &&
                (imageId >= INT32_MIN))
            {
                isImage = true;
                pastImage += 2;
                if (*pastImage == '\n')
                    pastImage++;
            }
        }

        if (isImage) {
            appendText(parts, textStart, s-textStart, account);
            parts.emplace_back();
            parts.back().isImage = true;
            parts.back().imageId = imageId;
            s = pastImage;
            textStart = pastImage;
        } else
            s++;
    }

    appendText(parts, textStart, s-textStart, account);
}

static td::td_api::object_ptr<td::td_api::formattedText> makeFormattedText(const MessagePart &input)
{
    auto result = td::td_api::make_object<td::td_api::formattedText>();
    result->text_ = input.text;
    for (const MessagePart::Entity &entity: input.entities) {
        auto type = makeTextEntityType(entity.kind, entity.argument);
        if (type && (entity.length > 0))
            result->entities_.push_back(td::td_api::make_object<td::td_api::textEntity>(
                entity.offset, entity.length, std::move(type)));
    }
    return result;
}

int transmitMessage(ChatId chatId, const char *message, TdTransceiver &transceiver,
                    TdAccountData &account, TdTransceiver::ResponseCb response)
{
    std::vector<MessagePart> parts;
    parseMessage(message, parts, account);
    if (parts.size() > MAX_MESSAGE_PARTS)
        return -E2BIG;

    for (const MessagePart &input: parts) {
        td::td_api::object_ptr<td::td_api::sendMessage> sendMessageRequest = td::td_api::make_object<td::td_api::sendMessage>();
        sendMessageRequest->chat_id_ = chatId.value();
        char *tempFileName = NULL;
        bool  hasImage     = false;

        if (input.isImage)
            hasImage = saveImage(input.imageId, &tempFileName);

        if (hasImage) {
            td::td_api::object_ptr<td::td_api::inputMessagePhoto> content = td::td_api::make_object<td::td_api::inputMessagePhoto>();
            content->photo_ = td::td_api::make_object<td::td_api::inputPhoto>(
                td::td_api::make_object<td::td_api::inputFileLocal>(tempFileName),
                nullptr, nullptr, std::vector<int32_t>(), 0, 0);
            content->caption_ = makeFormattedText(input);

            sendMessageRequest->input_message_content_ = std::move(content);
            purple_debug_misc(config::pluginId, "Sending photo %s\n", tempFileName);
        } else {
            td::td_api::object_ptr<td::td_api::inputMessageText> content = td::td_api::make_object<td::td_api::inputMessageText>();
            content->text_ = makeFormattedText(input);
            sendMessageRequest->input_message_content_ = std::move(content);
        }

        uint64_t requestId = transceiver.sendQuery(std::move(sendMessageRequest), response);
        account.addPendingRequest<SendMessageRequest>(requestId, chatId, tempFileName);
        if (tempFileName)
            g_free(tempFileName);
    }

    return 0;
}

std::string getSenderDisplayName(const td::td_api::chat &chat, const TgMessageInfo &message,
                                 PurpleAccount *account)
{
    if (message.outgoing)
        return purple_account_get_name_for_display(account);
    else if (isPrivateChat(chat) || getSecretChatId(chat).valid())
        return chat.title_;
    else
        return message.incomingGroupchatSender;
}

std::string getDownloadXferPeerName(ChatId chatId,
                                    const TgMessageInfo &message,
                                    TdAccountData &account)
{
    const td::td_api::chat *chat = account.getChat(chatId);
    if (chat) {
        const td::td_api::user *privateUser = account.getUserByPrivateChat(*chat);
        if (privateUser)
            return getPurpleBuddyName(*privateUser);
        auto secretChatId = getSecretChatId(*chat);
        if (secretChatId.valid())
            return getSecretChatBuddyName(secretChatId);
    }
    return message.incomingGroupchatSender;
}

void notifySendFailed(const td::td_api::updateMessageSendFailed &sendFailed, TdAccountData &account)
{
    if (sendFailed.message_ && sendFailed.error_) {
        const td::td_api::chat *chat = account.getChat(getChatId(*sendFailed.message_));
        if (chat) {
            std::string errorMessage = formatMessage(errorCodeMessage(), {std::to_string(sendFailed.error_->code_),
                                                     sendFailed.error_->message_});
            // TRANSLATOR: In-chat error message, argument will be text.
            errorMessage = formatMessage(_("Failed to send message: {}"), errorMessage);
            showChatNotification(account, *chat, errorMessage.c_str(), sendFailed.message_->date_,
                                 (PurpleMessageFlags)0);
        }
    }
}

template<typename T>
T zeroIfNegative(T value)
{
    return (value >= 0) ? value : 0;
}

void updateOption(const td::td_api::updateOption &option, TdAccountData &account)
{
    if ((option.name_ == "version") && option.value_ &&
        (option.value_->get_id() == td::td_api::optionValueString::ID))
    {
        purple_debug_misc(config::pluginId, "tdlib version: %s\n",
                            static_cast<const td::td_api::optionValueString &>(*option.value_).value_.c_str());
    } else if ((option.name_ == "message_caption_length_max") && option.value_ &&
        (option.value_->get_id() == td::td_api::optionValueInteger::ID))
    {
        account.options.maxCaptionLength = zeroIfNegative(static_cast<const td::td_api::optionValueInteger &>(*option.value_).value_);
    } else if ((option.name_ == "message_text_length_max") && option.value_ &&
        (option.value_->get_id() == td::td_api::optionValueInteger::ID))
    {
        account.options.maxMessageLength = zeroIfNegative(static_cast<const td::td_api::optionValueInteger &>(*option.value_).value_);
    } else
        purple_debug_misc(config::pluginId, "Option update %s\n", option.name_.c_str());
}

void populateGroupChatList(PurpleRoomlist *roomlist, const std::vector<const td::td_api::chat *> &chats,
                           const TdAccountData &account)
{
    for (const td::td_api::chat *chat: chats)
        if (account.isGroupChatWithMembership(*chat)) {
            PurpleRoomlistRoom *room = purple_roomlist_room_new(PURPLE_ROOMLIST_ROOMTYPE_ROOM,
                                                                chat->title_.c_str(), NULL);
            purple_roomlist_room_add_field (roomlist, room, getPurpleChatName(*chat).c_str());
            BasicGroupId groupId = getBasicGroupId(*chat);
            if (groupId.valid()) {
                const td::td_api::basicGroupFullInfo *fullInfo = account.getBasicGroupInfo(groupId);
                if (fullInfo && !fullInfo->description_.empty())
                    purple_roomlist_room_add_field(roomlist, room, fullInfo->description_.c_str());
            }
            SupergroupId supergroupId = getSupergroupId(*chat);
            if (supergroupId.valid()) {
                const td::td_api::supergroupFullInfo *fullInfo = account.getSupergroupInfo(supergroupId);
                if (fullInfo && !fullInfo->description_.empty())
                    purple_roomlist_room_add_field(roomlist, room, fullInfo->description_.c_str());
            }
            purple_roomlist_room_add(roomlist, room);
        }
    purple_roomlist_set_in_progress(roomlist, FALSE);
}

AccountThread::AccountThread(PurpleAccount* purpleAccount)
{
    m_accountUserName   = purple_account_get_username(purpleAccount);
    m_accountProtocolId = purple_account_get_protocol_id(purpleAccount);
}

void AccountThread::threadFunc()
{
    run();
    g_idle_add(&AccountThread::mainThreadCallback, this);
}

static bool g_singleThread = false;

void AccountThread::setSingleThread()
{
    g_singleThread = true;
}

bool AccountThread::isSingleThread()
{
    return g_singleThread;
}

void AccountThread::startThread()
{
    if (!g_singleThread) {
        if (!m_thread.joinable())
            m_thread = std::thread(std::bind(&AccountThread::threadFunc, this));
    } else {
        run();
        mainThreadCallback(this);
    }
}

gboolean AccountThread::mainThreadCallback(gpointer data)
{
    AccountThread  *self     = static_cast<AccountThread *>(data);
    PurpleAccount  *account  = purple_accounts_find(self->m_accountUserName.c_str(),
                                                    self->m_accountProtocolId.c_str());
    PurpleTdClient *tdClient = account ? getTdClient(account) : nullptr;
    if (self->m_thread.joinable())
        self->m_thread.join();

    if (tdClient)
        self->callback(tdClient);

    return FALSE; // this idle callback will not be called again
}
