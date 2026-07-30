#include "td-client.h"
#include "tdlib-schema.h"
#include "purple-info.h"
#include "config.h"
#include "format.h"
#include "receiving.h"
#include "file-transfer.h"
#include "call.h"
#include "secret-chat.h"
#include "sticker.h"
#include <unistd.h>
#include <stdlib.h>
#include <algorithm>

enum {
    // Typing notifications seems to be resent every 5-6 seconds, so 10s timeout hould be appropriate
    REMOTE_TYPING_NOTICE_TIMEOUT = 10,
    SUPERGROUP_MEMBER_LIMIT      = 200,
};

static ChatId chatIdFromTdInt(td::td_api::int53 id)
{
    std::string idString = std::to_string(id);
    return ChatId::fromString(idString.c_str());
}

static MessageId messageIdFromTdInt(td::td_api::int53 id)
{
    const std::string idString = std::to_string(id);
    return MessageId::fromString(idString.c_str());
}

static TdAccountData::PendingSendLookupResult
extractPendingSendForUpdate(
    TdAccountData &data, const td::td_api::message *message,
    MessageId oldMessageId,
    TdAccountData::PendingSendInfo &pending)
{
    if (message &&
        data.extractPendingSend(
            getChatId(*message), oldMessageId, pending)) {
        return TdAccountData::PendingSendLookupResult::Found;
    }

    return data.extractPendingSend(oldMessageId, pending);
}

static bool isChildForumTopic(ChatTarget target)
{
    return target.valid() && target.isForumTopic() &&
           target.forumTopicId() != ForumTopicId::general();
}

static PurpleConvChat *getActiveChatWithPurpleId(
    PurpleConversation *conversation, PurpleAccount *account,
    int32_t purpleChatId)
{
    if (!conversation ||
        purple_conversation_get_account(conversation) != account) {
        return nullptr;
    }

    PurpleConvChat *chat =
        purple_conversation_get_chat_data(conversation);
    // libpurple can retain left conversations across reconnects, while this
    // plugin's per-client numeric chat ID allocator starts over.
    if (!chat ||
        purple_conv_chat_get_id(chat) != purpleChatId ||
        purple_conv_chat_has_left(chat)) {
        return nullptr;
    }
    return chat;
}

static bool hasSafeConversationTargetForSend(
    PurpleAccount *account, int32_t purpleChatId,
    ChatTarget expectedTarget)
{
    bool hasActiveExactChild = false;

    for (GList *item = purple_get_chats(); item; item = item->next) {
        PurpleConversation *conversation =
            static_cast<PurpleConversation *>(item->data);
        PurpleConvChat *chat =
            getActiveChatWithPurpleId(
                conversation, account, purpleChatId);
        if (!chat)
            continue;

        const ChatTarget nameTarget = parsePurpleChatName(
            purple_conversation_get_name(conversation));
        if (!areEquivalentConversationTargets(
                nameTarget, expectedTarget)) {
            return false;
        }
        if (isChildForumTopic(expectedTarget)) {
            hasActiveExactChild = true;
        }
    }

    return !isChildForumTopic(expectedTarget) ||
           hasActiveExactChild;
}

static bool hasChildForumConversation(
    PurpleAccount *account, int32_t purpleChatId)
{
    for (GList *item = purple_get_chats(); item; item = item->next) {
        PurpleConversation *conversation =
            static_cast<PurpleConversation *>(item->data);
        PurpleConvChat *chat =
            getActiveChatWithPurpleId(
                conversation, account, purpleChatId);
        if (!chat)
            continue;

        if (isChildForumTopic(parsePurpleChatName(
                purple_conversation_get_name(conversation)))) {
            return true;
        }
    }

    return false;
}

static bool hasPersistentForumTopicJoin(
    PurpleAccount *account, ChatTarget target)
{
    if (!purple_account_is_connected(account))
        return true;

    const std::string purpleName = getPurpleChatName(target);
    return purple_blist_find_chat(
               account, purpleName.c_str()) != nullptr ||
           purple_find_conversation_with_account(
               PURPLE_CONV_TYPE_CHAT,
               purpleName.c_str(), account) != nullptr;
}

static std::string escapeForNotice(const std::string &text)
{
    char *escaped = purple_markup_escape_text(text.c_str(), text.size());
    std::string result = escaped ? escaped : "";
    g_free(escaped);
    return result;
}

static std::string messageIdsToString(const std::vector<td::td_api::int53> &ids)
{
    std::string result;
    size_t limit = std::min<size_t>(ids.size(), 5);
    for (size_t i = 0; i < limit; i++) {
        if (!result.empty())
            result += ", ";
        result += std::to_string(ids[i]);
    }
    if (ids.size() > limit)
        result += ", ...";
    return result;
}

static std::string deletedMessagesNotice(
    const std::vector<td::td_api::int53> &messageIds)
{
    // TRANSLATOR: {} is a comma-separated list of Telegram message IDs.
    return formatMessage(
        _("Deleted message(s): {}"),
        messageIdsToString(messageIds));
}

static std::string sendFailureNotice(
    const td::td_api::error &error)
{
    const std::string detail = formatMessage(
        errorCodeMessage(),
        {std::to_string(error.code_), error.message_});
    // TRANSLATOR: In-chat error message, argument is a Telegram error.
    return formatMessage(
        _("Failed to send message: {}"), detail);
}

static std::string reactionTypeToString(const td::td_api::ReactionType *reaction)
{
    if (!reaction)
        return "";

    switch (reaction->get_id()) {
        case td::td_api::reactionTypeEmoji::ID:
            return escapeForNotice(static_cast<const td::td_api::reactionTypeEmoji *>(reaction)->emoji_);
        case td::td_api::reactionTypeCustomEmoji::ID:
            return formatMessage("[custom emoji: {}]",
                                 std::to_string(static_cast<const td::td_api::reactionTypeCustomEmoji *>(reaction)->custom_emoji_id_));
        case td::td_api::reactionTypePaid::ID:
            return "[paid]";
    }
    return "[reaction]";
}

static std::string reactionTypesToString(const std::vector<td::td_api::object_ptr<td::td_api::ReactionType>> &reactions)
{
    std::string result;
    for (const auto &reaction: reactions) {
        if (!result.empty())
            result += ", ";
        result += reactionTypeToString(reaction.get());
    }
    return result.empty() ? _("none") : result;
}

static std::string messageReactionsToString(const std::vector<td::td_api::object_ptr<td::td_api::messageReaction>> &reactions)
{
    std::string result;
    for (const auto &reaction: reactions) {
        if (!reaction)
            continue;
        if (!result.empty())
            result += ", ";
        result += formatMessage("{0} x{1}",
                                {reactionTypeToString(reaction->type_.get()),
                                 std::to_string(reaction->total_count_)});
    }
    return result.empty() ? _("none") : result;
}

static std::string reactionSenderToString(
    const td::td_api::object_ptr<td::td_api::MessageSender> &sender,
    const TdAccountData &account)
{
    if (sender) {
        if (sender->get_id() == td::td_api::messageSenderUser::ID) {
            std::string displayName = account.getDisplayName(getUserId(sender));
            if (!displayName.empty())
                return displayName;
        } else if (sender->get_id() == td::td_api::messageSenderChat::ID) {
            ChatId chatId = chatIdFromTdInt(
                static_cast<const td::td_api::messageSenderChat *>(sender.get())->chat_id_);
            const td::td_api::chat *chat = account.getChat(chatId);
            if (chat && !chat->title_.empty())
                return chat->title_;
        }
    }

    // TRANSLATOR: Placeholder for an unknown message reaction sender. Will be used like a username.
    return _("Someone");
}

static void showChatUpdate(TdAccountData &account, ChatId chatId, const std::string &message,
                           PurpleMessageFlags extraFlags = (PurpleMessageFlags)0)
{
    const td::td_api::chat *chat = account.getChat(chatId);
    if (chat)
        showChatNotification(account, *chat, message.c_str(), extraFlags);
}

static PurpleConversation *findDisplayedConversation(
    PurpleAccount *account,
    const TdAccountData::DisplayedMessageConversation &identity)
{
    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            identity.type, identity.name.c_str(), account);
    if (!conversation)
        return nullptr;
    if (identity.type == PURPLE_CONV_TYPE_CHAT) {
        PurpleConvChat *chat =
            purple_conversation_get_chat_data(conversation);
        if (!chat || purple_conv_chat_has_left(chat))
            return nullptr;
    }
    return conversation;
}

bool PurpleTdClient::showMessageLinkedUpdate(
    ChatId chatId, MessageId messageId,
    const std::string &message,
    PurpleMessageFlags extraFlags)
{
    TdAccountData::DisplayedMessageConversation identity;
    const TdAccountData::DisplayedMessageLookupResult lookup =
        m_data.findDisplayedMessageConversation(
            chatId, messageId, identity);

    if (lookup ==
        TdAccountData::DisplayedMessageLookupResult::Available) {
        PurpleConversation *conversation =
            findDisplayedConversation(m_account, identity);
        if (!conversation)
            return true;

        const std::shared_ptr<LifetimeState> lifetime =
            m_lifetime;
        writeConversationNotification(
            conversation, message, extraFlags);
        return lifetime->alive;
    }

    if (lookup !=
            TdAccountData::DisplayedMessageLookupResult::
                Available &&
        m_data.shouldUseLegacyMessageUpdateFallback(
            chatId, messageId)) {
        const std::shared_ptr<LifetimeState> lifetime =
            m_lifetime;
        showChatUpdate(
            m_data, chatId, message, extraFlags);
        return lifetime->alive;
    }

    return true;
}

bool PurpleTdClient::showDeletedMessageUpdate(
    ChatId chatId,
    const std::vector<td::td_api::int53> &messageIds)
{
    if (!m_data.isForumSensitiveChat(chatId)) {
        const std::shared_ptr<LifetimeState> lifetime =
            m_lifetime;
        showChatUpdate(
            m_data, chatId,
            deletedMessagesNotice(messageIds));
        return lifetime->alive;
    }

    struct DestinationMessages {
        TdAccountData::DisplayedMessageConversation
            destination;
        std::vector<td::td_api::int53> messageIds;
    };

    std::vector<DestinationMessages> destinations;
    std::vector<td::td_api::int53> legacyMessageIds;
    for (td::td_api::int53 rawMessageId : messageIds) {
        TdAccountData::DisplayedMessageConversation
            identity;
        const TdAccountData::DisplayedMessageLookupResult
            lookup =
                m_data.findDisplayedMessageConversation(
                    chatId,
                    messageIdFromTdInt(rawMessageId),
                    identity);
        if (lookup ==
            TdAccountData::DisplayedMessageLookupResult::
                Available) {
            auto destination = std::find_if(
                destinations.begin(), destinations.end(),
                [&identity](
                    const DestinationMessages &item) {
                    return item.destination == identity;
                });
            if (destination == destinations.end()) {
                destinations.push_back(
                    DestinationMessages{
                        identity,
                        std::vector<td::td_api::int53>()});
                destination = destinations.end() - 1;
            }
            destination->messageIds.push_back(
                rawMessageId);
        } else if (
            m_data.shouldUseLegacyMessageUpdateFallback(
                chatId,
                messageIdFromTdInt(rawMessageId))) {
            legacyMessageIds.push_back(rawMessageId);
        }
    }

    for (const DestinationMessages &destination :
         destinations) {
        PurpleConversation *conversation =
            findDisplayedConversation(
                m_account, destination.destination);
        if (!conversation)
            continue;

        const std::shared_ptr<LifetimeState> lifetime =
            m_lifetime;
        writeConversationNotification(
            conversation,
            deletedMessagesNotice(destination.messageIds));
        if (!lifetime->alive)
            return false;
    }

    if (!legacyMessageIds.empty()) {
        const std::shared_ptr<LifetimeState> lifetime =
            m_lifetime;
        showChatUpdate(
            m_data, chatId,
            deletedMessagesNotice(legacyMessageIds));
        if (!lifetime->alive)
            return false;
    }

    return true;
}

bool PurpleTdClient::showTargetNotification(
    ChatTarget target, const std::string &message,
    time_t timestamp, PurpleMessageFlags extraFlags)
{
    if (!target.valid())
        return true;

    const std::shared_ptr<LifetimeState> lifetime =
        m_lifetime;
    if (isChildForumTopic(target)) {
        const std::string conversationName =
            getPurpleChatName(target);
        PurpleConversation *conversation =
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT,
                conversationName.c_str(), m_account);
        if (!conversation)
            return true;
        PurpleConvChat *chat =
            purple_conversation_get_chat_data(conversation);
        if (!chat)
            return true;

        // A send failure belongs to the exact room from which the send
        // originated. A left conversation is still a safe display target;
        // writing to it does not reactivate, rejoin, or present the room.
        writeConversationNotification(
            conversation, message, extraFlags, timestamp);
        return lifetime->alive;
    }

    const td::td_api::chat *chat =
        m_data.getChat(target.chatId());
    if (!chat)
        return true;
    if (timestamp >= 0) {
        showChatNotification(
            m_data, *chat, message.c_str(),
            timestamp, extraFlags);
    } else {
        showChatNotification(
            m_data, *chat, message.c_str(), extraFlags);
    }
    return lifetime->alive;
}

ChatTarget PurpleTdClient::replacePendingMessageId(
    const td::td_api::message &message,
    MessageId oldMessageId, ChatId oldChatId)
{
    const ChatId chatId = getChatId(message);
    const MessageId newMessageId = getId(message);
    ChatTarget target;
    const td::td_api::chat *chat = m_data.getChat(chatId);
    if (chat) {
        target = getMessageRoomTarget(*chat, message);
    }
    if (oldChatId.valid()) {
        m_data.replaceMessageId(
            oldChatId, oldMessageId,
            chatId, newMessageId, target);
    }
    if (target.valid()) {
        m_data.rememberMessageTarget(
            target, newMessageId);
    }
    return target;
}

PurpleTdClient::PurpleTdClient(
    PurpleAccount *acct,
    ITransceiverBackend *testBackend,
    const TdlibPurpleApplicationCredentials &applicationCredentials)
:   m_account(acct),
    m_applicationCredentials(applicationCredentials),
    m_transceiver(this, acct, &PurpleTdClient::processUpdate, testBackend),
    m_data(acct, m_transceiver),
    m_lifetime(std::make_shared<LifetimeState>()),
    m_forumTopics(std::make_unique<ForumTopicsAdapter>(
        m_transceiver, m_data,
        [this](ChatTarget target) {
            projectForumTopic(target);
        }))
{
    StickerConversionThread::setCallback(&PurpleTdClient::onAnimatedStickerConverted);
    setPurpleConnectionInProgress();
}

PurpleTdClient::~PurpleTdClient()
{
    m_lifetime->alive = false;
    m_forumTopics->shutdown();

    std::vector<PurpleXfer *> transfers;
    m_data.removeAllFileTransfers(transfers);
    for (PurpleXfer *xfer: transfers) {
        const bool isUpload =
            purple_xfer_get_type(xfer) == PURPLE_XFER_SEND;
        if (xfer->data && !purple_xfer_is_canceled(xfer))
            purple_xfer_cancel_local(xfer);
        // We keep uploads ref'd but not downloads.
        if (isUpload)
            purple_xfer_unref(xfer);
    }
    m_data.extractFileTransferRequests(transfers);
    for (PurpleXfer *xfer: transfers) {
        if (xfer->data && !purple_xfer_is_canceled(xfer))
            purple_xfer_cancel_local(xfer);
        purple_xfer_unref(xfer);
    }

    std::vector<IncomingMessage> messages;
    m_data.pendingMessages.flush(messages);

    // This avoids re-sending download request when displaying message. Doing this for messages
    // that don't involve inline downloads is fine.
    for (IncomingMessage &fullMessage: messages)
        fullMessage.inlineDownloadTimeout = true;

    showMessages(messages, m_data);
}

void PurpleTdClient::disableTdlibLogging()
{
    /*
     * Even TDLib warning and error messages can include phone numbers or
     * serialized request data. Keep only fatal handling and discard the
     * internal log stream; plugin-owned diagnostics remain available.
     */
    td::Client::execute(
        {0, td::td_api::make_object<td::td_api::setLogVerbosityLevel>(0)});
    td::Client::execute(
        {0,
         td::td_api::make_object<td::td_api::setLogStream>(
             td::td_api::make_object<td::td_api::logStreamEmpty>())});
}

void PurpleTdClient::setTdlibFatalErrorCallback(td::Log::FatalErrorCallbackPtr callback)
{
    td::Log::set_fatal_error_callback(callback);
}

void PurpleTdClient::processUpdate(td::td_api::Object &update)
{
    purple_debug_misc(config::pluginId, "Incoming update\n");

    switch (update.get_id()) {
    case td::td_api::updateAuthorizationState::ID: {
        auto &update_authorization_state = static_cast<td::td_api::updateAuthorizationState &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: authorization state\n");
        if (update_authorization_state.authorization_state_) {
            m_lastAuthState = update_authorization_state.authorization_state_->get_id();
            processAuthorizationState(*update_authorization_state.authorization_state_);
        }
        break;
    }

    case td::td_api::updateUser::ID: {
        auto &userUpdate = static_cast<td::td_api::updateUser &>(update);
        updateUser(std::move(userUpdate.user_));
        break;
    }

    case td::td_api::updateNewChat::ID: {
        auto &newChat = static_cast<td::td_api::updateNewChat &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: new chat\n");
        if (!newChat.chat_ || !newChat.chat_->type_) {
            purple_debug_warning(config::pluginId,
                                 "Received new chat without chat type\n");
            break;
        }

        const int32_t chatType = newChat.chat_->type_->get_id();
        const bool isBasicGroup =
            chatType == td::td_api::chatTypeBasicGroup::ID;
        const bool isSupergroup =
            chatType == td::td_api::chatTypeSupergroup::ID;
        const BasicGroupId basicGroupId = getBasicGroupId(*newChat.chat_);
        const SupergroupId supergroupId = getSupergroupId(*newChat.chat_);
        const bool groupMetadataKnown =
            (isBasicGroup && m_data.getBasicGroup(basicGroupId)) ||
            (isSupergroup && m_data.getSupergroup(supergroupId));

        if (chatType == td::td_api::chatTypePrivate::ID ||
            chatType == td::td_api::chatTypeSecret::ID ||
            m_data.isGroupChatWithMembership(*newChat.chat_)) {
            addChat(std::move(newChat.chat_));
        } else if ((isBasicGroup || isSupergroup) && !groupMetadataKnown) {
            const ChatId chatId = getId(*newChat.chat_);
            purple_debug_misc(
                config::pluginId,
                "Caching group chat %" G_GINT64_FORMAT
                " until membership metadata arrives\n",
                chatId.value());
            m_data.addChat(std::move(newChat.chat_));
            m_deferredGroupChats.insert(chatId);
            m_forumTopics->markRoomListsPending();
        } else {
            const ChatId chatId = getId(*newChat.chat_);
            const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
            failForumTopicJoins(chatId);
            if (!lifetime->alive)
                return;
            projectForumTopics(chatId);
            purple_debug_misc(config::pluginId,
                              "Incoming update: ignorig ID=%d\n",
                              update.get_id());
            purple_debug_misc(config::pluginId,
                              "Not adding a group that we are not a member of");
        }

        break;
    }

    case td::td_api::updateNewMessage::ID: {
        auto &newMessageUpdate = static_cast<td::td_api::updateNewMessage &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: new message\n");
        if (newMessageUpdate.message_)
            onIncomingMessage(std::move(newMessageUpdate.message_));
        else
            purple_debug_warning(config::pluginId, "Received null new message\n");
        break;
    }

    case td::td_api::updateMessageContent::ID: {
        auto &messageUpdate =
            static_cast<td::td_api::updateMessageContent &>(
                update);
        purple_debug_misc(config::pluginId, "Incoming update: message %" G_GINT64_FORMAT " content update\n",
                          messageUpdate.message_id_);
        if (messageUpdate.new_content_) {
            const ChatId chatId =
                chatIdFromTdInt(messageUpdate.chat_id_);
            const MessageId messageId =
                messageIdFromTdInt(
                    messageUpdate.message_id_);
            const std::string description =
                describeMessageContent(
                    *messageUpdate.new_content_, m_data);
            const std::shared_ptr<LifetimeState> lifetime =
                m_lifetime;
            std::vector<IncomingMessage> readyMessages;
            if (replaceQueuedMessageContent(
                    m_data, chatId, messageId,
                    std::move(messageUpdate.new_content_),
                    readyMessages)) {
                if (!lifetime->alive)
                    return;
                showMessages(readyMessages, m_data);
                if (!lifetime->alive)
                    return;
                break;
            }
            const TdAccountData::
                DisplayedMessageUpdateResult result =
                    m_data.showUpdatedMessage(
                        chatId, messageId,
                        description);
            if (!lifetime->alive)
                return;
            if (result !=
                    TdAccountData::
                        DisplayedMessageUpdateResult::
                            Written &&
                m_data.shouldUseLegacyMessageUpdateFallback(
                    chatId, messageId)) {
                // TRANSLATOR: In-chat status update. First argument is a Telegram message id, second is message content.
                showChatUpdate(
                    m_data, chatId,
                    formatMessage(
                        _("Message {0} updated: {1}"),
                        {std::to_string(
                             messageUpdate.message_id_),
                         description}));
                if (!lifetime->alive)
                    return;
            }
        }
        break;
    }

    case td::td_api::updateMessageEdited::ID: {
        const auto &messageUpdate = static_cast<const td::td_api::updateMessageEdited &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: message %" G_GINT64_FORMAT " edited\n",
                          messageUpdate.message_id_);
        break;
    }

    case td::td_api::updateDeleteMessages::ID: {
        const auto &messageUpdate = static_cast<const td::td_api::updateDeleteMessages &>(update);
        const ChatId chatId =
            chatIdFromTdInt(messageUpdate.chat_id_);
        purple_debug_misc(config::pluginId, "Incoming update: %zu deleted messages\n",
                          messageUpdate.message_ids_.size());
        std::vector<MessageId> deletedMessageIds;
        deletedMessageIds.reserve(
            messageUpdate.message_ids_.size());
        for (td::td_api::int53 rawMessageId :
             messageUpdate.message_ids_) {
            deletedMessageIds.push_back(
                messageIdFromTdInt(rawMessageId));
            TdAccountData::PendingSendInfo pending;
            if (m_data.extractPendingSend(
                    chatId,
                    messageIdFromTdInt(rawMessageId),
                    pending)) {
                removeTempFile(pending.tempFile);
            }
        }
        PendingMessageQueue::RemoveResult removedMessages;
        if (!messageUpdate.from_cache_) {
            removedMessages =
                m_data.pendingMessages.removeMessages(
                    chatId, deletedMessageIds);
        }
        m_data.discardPendingReadReceipts(
            chatId, deletedMessageIds);
        if (!messageUpdate.from_cache_) {
            const std::shared_ptr<LifetimeState> lifetime =
                m_lifetime;
            showMessages(
                removedMessages.readyMessages, m_data);
            if (!lifetime->alive)
                return;
            if (!showDeletedMessageUpdate(
                    chatId,
                    messageUpdate.message_ids_)) {
                return;
            }
        }
        break;
    }

    case td::td_api::updateMessageIsPinned::ID: {
        const auto &messageUpdate = static_cast<const td::td_api::updateMessageIsPinned &>(update);
        const char *format = messageUpdate.is_pinned_ ?
            // TRANSLATOR: In-chat status update, argument is a Telegram message id.
            _("Message {} was pinned") :
            // TRANSLATOR: In-chat status update, argument is a Telegram message id.
            _("Message {} was unpinned");
        if (!showMessageLinkedUpdate(
                chatIdFromTdInt(messageUpdate.chat_id_),
                messageIdFromTdInt(
                    messageUpdate.message_id_),
                formatMessage(
                    format,
                    std::to_string(
                        messageUpdate.message_id_)))) {
            return;
        }
        break;
    }

    case td::td_api::updateMessageInteractionInfo::ID: {
        const auto &messageUpdate = static_cast<const td::td_api::updateMessageInteractionInfo &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: message %" G_GINT64_FORMAT " interaction info\n",
                          messageUpdate.message_id_);
        break;
    }

    case td::td_api::updateMessageReaction::ID: {
        const auto &messageUpdate = static_cast<const td::td_api::updateMessageReaction &>(update);
        UserId actorId = getUserId(messageUpdate.actor_id_);
        std::string actor = actorId.valid() ? escapeForNotice(m_data.getDisplayName(actorId)) :
                                              // TRANSLATOR: Placeholder for an unknown message reaction sender.
                                              _("Someone");
        // TRANSLATOR: In-chat status update. Arguments are user name, message id, old reactions, new reactions.
        if (!showMessageLinkedUpdate(
                chatIdFromTdInt(messageUpdate.chat_id_),
                messageIdFromTdInt(
                    messageUpdate.message_id_),
                formatMessage(
                    _("{0} changed reactions on message {1}: {2} -> {3}"),
                    {actor,
                     std::to_string(
                         messageUpdate.message_id_),
                     reactionTypesToString(
                         messageUpdate.old_reaction_types_),
                     reactionTypesToString(
                         messageUpdate.new_reaction_types_)}),
                PURPLE_MESSAGE_NO_LOG)) {
            return;
        }
        break;
    }

    case td::td_api::updateMessageReactions::ID: {
        const auto &messageUpdate = static_cast<const td::td_api::updateMessageReactions &>(update);
        // TRANSLATOR: In-chat status update. First argument is a message id, second is a reaction summary.
        if (!showMessageLinkedUpdate(
                chatIdFromTdInt(messageUpdate.chat_id_),
                messageIdFromTdInt(
                    messageUpdate.message_id_),
                formatMessage(
                    _("Reactions on message {0}: {1}"),
                    {std::to_string(
                         messageUpdate.message_id_),
                     messageReactionsToString(
                         messageUpdate.reactions_)}),
                PURPLE_MESSAGE_NO_LOG)) {
            return;
        }
        break;
    }

    case td::td_api::updateMessageUnreadReactions::ID: {
        const auto &messageUpdate = static_cast<const td::td_api::updateMessageUnreadReactions &>(update);
        std::vector<UnreadReactionInfo> reactions;
        for (const auto &reaction: messageUpdate.unread_reactions_) {
            if (!reaction || !reaction->type_)
                continue;

            UnreadReactionInfo info;
            info.sender = reactionSenderToString(reaction->sender_id_, m_data);
            info.text = reactionTypeToString(reaction->type_.get());
            reactions.push_back(std::move(info));
        }

        if (reactions.empty())
            break;

        ChatId chatId = chatIdFromTdInt(messageUpdate.chat_id_);
        if (!m_data.getChat(chatId))
            break;

        MessageId messageId = MessageId::fromString(std::to_string(messageUpdate.message_id_).c_str());
        auto getMessageRequest = td::td_api::make_object<td::td_api::getMessage>(
            messageUpdate.chat_id_, messageUpdate.message_id_);
        uint64_t requestId = m_transceiver.sendQuery(
            std::move(getMessageRequest), &PurpleTdClient::unreadReactionsMessageResponse);
        m_data.addPendingRequest<UnreadReactionsRequest>(
            requestId,
            std::make_unique<UnreadReactionsRequest>(requestId, chatId, messageId,
                                                     std::move(reactions)));
        m_transceiver.setQueryTimer(requestId, &PurpleTdClient::unreadReactionsMessageResponse,
                                    1, true);
        break;
    }

    case td::td_api::updateMessageContainsUnreadPollVotes::ID: {
        const auto &messageUpdate = static_cast<const td::td_api::updateMessageContainsUnreadPollVotes &>(update);
        purple_debug_misc(config::pluginId,
                          "Incoming update: message %" G_GINT64_FORMAT " unread poll votes=%d\n",
                          messageUpdate.message_id_, (int)messageUpdate.contains_unread_poll_votes_);
        break;
    }

    case td::td_api::updateUserStatus::ID: {
        auto &updateStatus = static_cast<td::td_api::updateUserStatus &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: user status\n");
        if (updateStatus.status_)
            updateUserStatus(getUserId(updateStatus), std::move(updateStatus.status_));
        break;
    }

    case td::td_api::updateChatAction::ID: {
        auto &updateChatAction = static_cast<td::td_api::updateChatAction &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: chat action %d\n",
            updateChatAction.action_ ? updateChatAction.action_->get_id() : 0);
        handleUserChatAction(updateChatAction);
        break;
    }

    case td::td_api::updateBasicGroup::ID: {
        auto &groupUpdate = static_cast<td::td_api::updateBasicGroup &>(update);
        updateGroup(std::move(groupUpdate.basic_group_));
        break;
    }

    case td::td_api::updateSupergroup::ID: {
        auto &groupUpdate = static_cast<td::td_api::updateSupergroup &>(update);
        updateSupergroup(std::move(groupUpdate.supergroup_));
        break;
    }

    case td::td_api::updateForumTopicInfo::ID: {
        const auto &topicUpdate =
            static_cast<const td::td_api::updateForumTopicInfo &>(update);
        m_forumTopics->processUpdate(topicUpdate);
        break;
    }

    case td::td_api::updateChatMember::ID: {
        auto &memberUpdate = static_cast<td::td_api::updateChatMember &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: chat member for chat %" G_GINT64_FORMAT "\n",
                          memberUpdate.chat_id_);
        updateVisibleChatMemberList(memberUpdate);
        break;
    }

    case td::td_api::updateBasicGroupFullInfo::ID: {
        auto &groupUpdate = static_cast<td::td_api::updateBasicGroupFullInfo &>(update);
        updateGroupFull(getBasicGroupId(groupUpdate), std::move(groupUpdate.basic_group_full_info_));
        break;
    };

    case td::td_api::updateSupergroupFullInfo::ID: {
        auto &groupUpdate = static_cast<td::td_api::updateSupergroupFullInfo &>(update);
        updateSupergroupFull(getSupergroupId(groupUpdate), std::move(groupUpdate.supergroup_full_info_));
        break;
    };

    case td::td_api::updateMessageSendSucceeded::ID: {
        auto &sendSucceeded = static_cast<const td::td_api::updateMessageSendSucceeded &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: message %" G_GINT64_FORMAT " send succeeded\n",
                          sendSucceeded.old_message_id_);
        const MessageId oldMessageId =
            messageIdFromTdInt(
                sendSucceeded.old_message_id_);
        TdAccountData::PendingSendInfo pending;
        const TdAccountData::PendingSendLookupResult lookup =
            extractPendingSendForUpdate(
                m_data, sendSucceeded.message_.get(),
                oldMessageId, pending);
        if (sendSucceeded.message_) {
            ChatId oldChatId;
            if (lookup ==
                TdAccountData::PendingSendLookupResult::Found) {
                oldChatId = pending.target.chatId();
            } else if (
                lookup ==
                TdAccountData::PendingSendLookupResult::NotFound) {
                oldChatId =
                    getChatId(*sendSucceeded.message_);
            } else {
                purple_debug_warning(
                    config::pluginId,
                    "Ambiguous pending message id; final route was not rekeyed\n");
            }
            replacePendingMessageId(
                *sendSucceeded.message_,
                oldMessageId, oldChatId);
        }
        if (lookup ==
            TdAccountData::PendingSendLookupResult::Found) {
            removeTempFile(pending.tempFile);
        }
        break;
    }

    case td::td_api::updateMessageSendFailed::ID: {
        auto &sendFailed = static_cast<const td::td_api::updateMessageSendFailed &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: message %" G_GINT64_FORMAT " send failed\n",
                          sendFailed.old_message_id_);
        const MessageId oldMessageId =
            messageIdFromTdInt(
                sendFailed.old_message_id_);
        TdAccountData::PendingSendInfo pending;
        const TdAccountData::PendingSendLookupResult lookup =
            extractPendingSendForUpdate(
                m_data, sendFailed.message_.get(),
                oldMessageId, pending);
        ChatTarget target;
        if (sendFailed.message_) {
            ChatId oldChatId;
            if (lookup ==
                TdAccountData::PendingSendLookupResult::Found) {
                oldChatId = pending.target.chatId();
            } else if (
                lookup ==
                TdAccountData::PendingSendLookupResult::NotFound) {
                oldChatId =
                    getChatId(*sendFailed.message_);
            } else {
                purple_debug_warning(
                    config::pluginId,
                    "Ambiguous pending message id; failure notice suppressed\n");
            }
            const ChatTarget finalTarget =
                replacePendingMessageId(
                    *sendFailed.message_,
                    oldMessageId, oldChatId);
            if (lookup ==
                TdAccountData::PendingSendLookupResult::Found) {
                target = pending.target;
            } else if (
                lookup ==
                TdAccountData::PendingSendLookupResult::NotFound) {
                target = finalTarget;
            }
        } else if (
            lookup ==
            TdAccountData::PendingSendLookupResult::Found) {
            target = pending.target;
        }
        if (lookup ==
            TdAccountData::PendingSendLookupResult::Found) {
            removeTempFile(pending.tempFile);
        }
        if (sendFailed.message_ && sendFailed.error_ &&
            !showTargetNotification(
                target,
                sendFailureNotice(*sendFailed.error_),
                sendFailed.message_->date_)) {
            return;
        }
        break;
    }

    case td::td_api::updateChatPosition::ID: {
        auto &chatPositionUpdate = static_cast<td::td_api::updateChatPosition &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: update chat position for chat %" G_GINT64_FORMAT "\n",
                          chatPositionUpdate.chat_id_);
        if (chatPositionUpdate.position_)
            m_data.updateChatPosition(getChatId(chatPositionUpdate), std::move(chatPositionUpdate.position_));
        updateChat(m_data.getChat(getChatId(chatPositionUpdate)));
        break;
    }

    case td::td_api::updateChatTitle::ID: {
        auto &chatTitleUpdate = static_cast<td::td_api::updateChatTitle &>(update);
        const ChatId chatId = getChatId(chatTitleUpdate);
        const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
        purple_debug_misc(config::pluginId, "Incoming update: update chat title for chat %" G_GINT64_FORMAT "\n",
                          chatTitleUpdate.chat_id_);
        m_data.updateChatTitle(chatId, chatTitleUpdate.title_);
        updateChat(m_data.getChat(chatId));
        if (!lifetime->alive)
            return;
        projectForumTopics(chatId);
        break;
    }

    case td::td_api::updateChatLastMessage::ID: {
        auto &lastMessage = static_cast<td::td_api::updateChatLastMessage &>(update);
        updateChatLastMessage(lastMessage);
        break;
    }

    case td::td_api::updateOption::ID: {
        const td::td_api::updateOption &option = static_cast<const td::td_api::updateOption &>(update);
        updateOption(option, m_data);
        break;
    }

    case td::td_api::updateFile::ID: {
        auto &fileUpdate = static_cast<const td::td_api::updateFile &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: file update, id %d\n",
                          fileUpdate.file_ ? fileUpdate.file_->id_ : 0);
        if (fileUpdate.file_)
            updateFileTransferProgress(*fileUpdate.file_, m_transceiver, m_data,
                                       &PurpleTdClient::sendMessageResponse);
        break;
    };

    case td::td_api::updateSecretChat::ID: {
        auto &chatUpdate = static_cast<td::td_api::updateSecretChat &>(update);
        purple_debug_misc(config::pluginId, "Incoming update: secret chat, id %d\n",
                          chatUpdate.secret_chat_ ? chatUpdate.secret_chat_->id_ : 0);
        updateSecretChat(std::move(chatUpdate.secret_chat_), m_transceiver, m_data);
        break;
    };

    case td::td_api::updateCall::ID: {
        auto &callUpdate = static_cast<const td::td_api::updateCall &>(update);
        if (callUpdate.call_) {
            purpleDebug("Call update: id {}, outgoing={}, user id {}, state {}", {
                        std::to_string(callUpdate.call_->id_),
                        std::to_string(callUpdate.call_->user_id_),
                        std::to_string((int)callUpdate.call_->is_outgoing_),
                        std::to_string(callUpdate.call_->state_ ? callUpdate.call_->state_->get_id() : 0)});
            updateCall(*callUpdate.call_, m_data, m_transceiver);
        }
        break;
    };

    default:
        purple_debug_misc(config::pluginId, "Incoming update: ignorig ID=%d\n", update.get_id());
        break;
    }
}

void PurpleTdClient::setPurpleConnectionInProgress()
{
    purple_debug_misc(config::pluginId, "Connection in progress\n");
    PurpleConnection *gc = purple_account_get_connection(m_account);

    if (PURPLE_CONNECTION_IS_CONNECTED(gc))
        purple_blist_remove_account(m_account);
    purple_connection_set_state (gc, PURPLE_CONNECTING);
    purple_connection_update_progress(gc, "Connecting", 1, 2);
}

void PurpleTdClient::onLoggedIn()
{
    purple_connection_set_state (purple_account_get_connection(m_account), PURPLE_CONNECTED);

    // This query ensures an updateUser for every contact
    m_transceiver.sendQuery(td::td_api::make_object<td::td_api::getContacts>(),
                            &PurpleTdClient::getContactsResponse);
}

void PurpleTdClient::getContactsResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    purple_debug_misc(config::pluginId, "getContacts response to request %" G_GUINT64_FORMAT ", id %d\n", requestId, object ? object->get_id() : 0);
    if (object && (object->get_id() == td::td_api::users::ID)) {
        m_data.setContacts(*td::move_tl_object_as<td::td_api::users>(object));
        auto getChatsRequest = td::td_api::make_object<td::td_api::loadChats>();
        getChatsRequest->chat_list_ = td::td_api::make_object<td::td_api::chatListMain>();
        getChatsRequest->limit_ = 200;
        m_transceiver.sendQuery(std::move(getChatsRequest), &PurpleTdClient::getChatsResponse);
    } else
        notifyAuthError(object);
}

void PurpleTdClient::getChatsResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    purple_debug_misc(config::pluginId, "getChats response to request %" G_GUINT64_FORMAT "\n", requestId);
    if (object && (object->get_id() == td::td_api::ok::ID)) {
        auto getChatsRequest = td::td_api::make_object<td::td_api::loadChats>();
        getChatsRequest->chat_list_ = td::td_api::make_object<td::td_api::chatListMain>();
        getChatsRequest->limit_ = 200;
        m_transceiver.sendQuery(std::move(getChatsRequest), &PurpleTdClient::getChatsResponse);
    } else {
        std::string message = getDisplayedError(object);
        purple_debug_misc(config::pluginId, "Got no more chats: %s\n", message.c_str());
            m_data.getContactsWithNoChat(m_usersForNewPrivateChats);
            requestMissingPrivateChats();
    }
}

void PurpleTdClient::requestMissingPrivateChats()
{
    if (m_usersForNewPrivateChats.empty()) {
        purple_debug_misc(config::pluginId, "Login sequence complete\n");
        onChatListReady();
    } else {
        UserId userId = m_usersForNewPrivateChats.back();
        m_usersForNewPrivateChats.pop_back();
        purpleDebug("Requesting private chat for user id {}", userId.value());
        td::td_api::object_ptr<td::td_api::createPrivateChat> createChat =
            td::td_api::make_object<td::td_api::createPrivateChat>(userId.value(), false);
        m_transceiver.sendQuery(std::move(createChat), &PurpleTdClient::loginCreatePrivateChatResponse);
    }
}

void PurpleTdClient::loginCreatePrivateChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && (object->get_id() == td::td_api::chat::ID)) {
        td::td_api::object_ptr<td::td_api::chat> chat = td::move_tl_object_as<td::td_api::chat>(object);
        ChatId chatId = getId(*chat);
        purple_debug_misc(config::pluginId, "Requested private chat received: id %" G_GINT64_FORMAT "\n",
                          chat->id_);
        // Here the "new" chat already exists in AccountData because there has just been
        // updateNewChat about this same chat. But do addChat anyway, just in case.
        m_data.addChat(std::move(chat));
        updateChat(m_data.getChat(chatId));
    } else
        purple_debug_misc(config::pluginId, "Failed to get requested private chat\n");
    requestMissingPrivateChats();
}

void PurpleTdClient::requestBasicGroupFullInfo(BasicGroupId groupId)
{
    if (!m_data.isBasicGroupInfoRequested(groupId)) {
        m_data.setBasicGroupInfoRequested(groupId);
        uint64_t requestId = m_transceiver.sendQuery(td::td_api::make_object<td::td_api::getBasicGroupFullInfo>(groupId.value()),
                                                     &PurpleTdClient::groupInfoResponse);
        m_data.addPendingRequest<GroupInfoRequest>(requestId, groupId);
    }
}

void PurpleTdClient::requestSupergroupFullInfo(SupergroupId groupId)
{
    if (!m_data.isSupergroupInfoRequested(groupId)) {
        m_data.setSupergroupInfoRequested(groupId);
        uint64_t requestId = m_transceiver.sendQuery(td::td_api::make_object<td::td_api::getSupergroupFullInfo>(groupId.value()),
                                                     &PurpleTdClient::supergroupInfoResponse);
        m_data.addPendingRequest<SupergroupInfoRequest>(requestId, groupId);

        auto getMembersReq = td::td_api::make_object<td::td_api::getSupergroupMembers>();
        getMembersReq->supergroup_id_ = groupId.value();
        getMembersReq->filter_ = td::td_api::make_object<td::td_api::supergroupMembersFilterRecent>();
        getMembersReq->limit_ = SUPERGROUP_MEMBER_LIMIT;
        const uint64_t membersRevision =
            m_data.getSupergroupMembersRevision(groupId);
        requestId = m_transceiver.sendQuery(std::move(getMembersReq), &PurpleTdClient::supergroupMembersResponse);
        m_data.addPendingRequest<SupergroupMembersRequest>(
            requestId, groupId, membersRevision);
    }
}

void PurpleTdClient::groupInfoResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<GroupInfoRequest> request = m_data.getPendingRequest<GroupInfoRequest>(requestId);

    if (request && object && (object->get_id() == td::td_api::basicGroupFullInfo::ID)) {
        td::td_api::object_ptr<td::td_api::basicGroupFullInfo> groupInfo =
            td::move_tl_object_as<td::td_api::basicGroupFullInfo>(object);
        updateGroupFull(request->groupId, std::move(groupInfo));
    }
}

void PurpleTdClient::supergroupInfoResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<SupergroupInfoRequest> request = m_data.getPendingRequest<SupergroupInfoRequest>(requestId);

    if (request && object && (object->get_id() == td::td_api::supergroupFullInfo::ID)) {
        td::td_api::object_ptr<td::td_api::supergroupFullInfo> groupInfo =
            td::move_tl_object_as<td::td_api::supergroupFullInfo>(object);
        updateSupergroupFull(request->groupId, std::move(groupInfo));
    }
}

void PurpleTdClient::supergroupMembersResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<SupergroupMembersRequest> request =
        m_data.getPendingRequest<SupergroupMembersRequest>(
            requestId);

    if (request && object && (object->get_id() == td::td_api::chatMembers::ID)) {
        td::td_api::object_ptr<td::td_api::chatMembers> members =
            td::move_tl_object_as<td::td_api::chatMembers>(object);

        auto getMembersReq = td::td_api::make_object<td::td_api::getSupergroupMembers>();
        getMembersReq->supergroup_id_ = request->groupId.value();
        getMembersReq->filter_ = td::td_api::make_object<td::td_api::supergroupMembersFilterAdministrators>();
        getMembersReq->limit_ = SUPERGROUP_MEMBER_LIMIT;
        uint64_t newRequestId = m_transceiver.sendQuery(std::move(getMembersReq), &PurpleTdClient::supergroupAdministratorsResponse);
        m_data.addPendingRequest<GroupMembersRequestCont>(
            newRequestId, request->groupId,
            request->membersRevision, members.release());
    }
}

void PurpleTdClient::supergroupAdministratorsResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<GroupMembersRequestCont> request = m_data.getPendingRequest<GroupMembersRequestCont>(requestId);
    if (request) {
        auto members = std::move(request->members);

        if (object && (object->get_id() == td::td_api::chatMembers::ID)) {
            td::td_api::object_ptr<td::td_api::chatMembers> newMembers =
                td::move_tl_object_as<td::td_api::chatMembers>(object);
            for (auto &pNewMember: newMembers->members_) {
                if (! pNewMember || !pNewMember->member_id_) continue;
                const td::td_api::MessageSender *pNewMemberInfo = pNewMember->member_id_.get();
                if (std::find_if(members->members_.begin(), members->members_.end(),
                              [pNewMemberInfo](const td::td_api::object_ptr<td::td_api::chatMember> &pExistingMember) {
                                  return pExistingMember && pExistingMember->member_id_ &&
                                         isSameUser(*pExistingMember->member_id_, *pNewMemberInfo);
                              }) == members->members_.end())
                {
                    members->members_.push_back(std::move(pNewMember));
                }
            }
        }

        m_data.reconcileSupergroupMembers(
            request->groupId, std::move(members),
            request->membersRevision);
        const td::td_api::chat *chat = m_data.getSupergroupChatByGroup(request->groupId);
        const td::td_api::chatMembers *storedMembers =
            m_data.getSupergroupMembers(request->groupId);
        if (chat && storedMembers) {
            const std::shared_ptr<LifetimeState> lifetime =
                m_lifetime;
            const ContinuationGuard canContinue =
                [lifetime]() {
                    return lifetime->alive;
                };
            if (isEligibleForumParent(m_data, *chat)) {
                projectForumChatMembers(
                    m_data, *chat, *storedMembers,
                    canContinue);
                return;
            }

            PurpleConvChat *purpleChat = findChatConversation(m_account, *chat);
            if (purpleChat)
                updateSupergroupChatMembers(
                    purpleChat, *storedMembers, m_data);
        }
    }
}

void PurpleTdClient::updateGroupFull(BasicGroupId groupId, td::td_api::object_ptr<td::td_api::basicGroupFullInfo> groupInfo)
{
    if (!groupInfo)
        return;
    m_data.updateBasicGroupInfo(
        groupId, std::move(groupInfo));
    const td::td_api::chat *chat = m_data.getBasicGroupChatByGroup(groupId);
    const td::td_api::basicGroupFullInfo *storedInfo =
        m_data.getBasicGroupInfo(groupId);

    if (chat && storedInfo) {
        PurpleConvChat *purpleChat = findChatConversation(m_account, *chat);
        if (purpleChat)
            updateChatConversation(
                purpleChat, *storedInfo, m_data);
    }
}

void PurpleTdClient::updateSupergroupFull(SupergroupId groupId, td::td_api::object_ptr<td::td_api::supergroupFullInfo> groupInfo)
{
    if (!groupInfo)
        return;
    m_data.updateSupergroupInfo(
        groupId, std::move(groupInfo));
    const td::td_api::chat *chat = m_data.getSupergroupChatByGroup(groupId);
    const td::td_api::supergroupFullInfo *storedInfo =
        m_data.getSupergroupInfo(groupId);

    if (chat && storedInfo) {
        const std::shared_ptr<LifetimeState> lifetime =
            m_lifetime;
        const ContinuationGuard canContinue =
            [lifetime]() {
                return lifetime->alive;
            };
        if (isEligibleForumParent(m_data, *chat)) {
            projectForumChatDescription(
                m_data, *chat, storedInfo->description_,
                canContinue);
            return;
        }

        PurpleConvChat *purpleChat = findChatConversation(m_account, *chat);
        if (purpleChat)
            updateChatConversation(
                purpleChat, *storedInfo, m_data);
    }
}

void PurpleTdClient::onChatListReady()
{
    std::vector<const td::td_api::chat *> chats;
    m_data.getChats(chats);

    for (const td::td_api::chat *chat: chats) {
        const td::td_api::user *user = m_data.getUserByPrivateChat(*chat);
        if (user && isChatInContactList(*chat, user)) {
            std::string userName = getPurpleBuddyName(*user);
            purple_prpl_got_user_status(m_account, userName.c_str(),
                                        getPurpleStatusId(*user->status_), NULL);
        }
    }

    resolveDeferredGroupChats();
    m_chatListReady = true;
    markForumRoomListsReadyIfPossible();

    // Here we could remove buddies for which no private chat exists, meaning they have been remove
    // from the contact list perhaps in another client

    const td::td_api::user *selfInfo = m_data.getUserByPhone(purple_account_get_username(m_account));
    if (selfInfo != nullptr) {
        std::string alias = makeBasicDisplayName(*selfInfo);
        purple_debug_misc(config::pluginId, "Setting own alias to '%s'\n", alias.c_str());
        purple_account_set_alias(m_account, alias.c_str());
    } else
        purple_debug_warning(config::pluginId,
                             "Did not receive user information for self at login\n");

    purple_blist_add_account(m_account);
}

void PurpleTdClient::onAnimatedStickerConverted(AccountThread *arg)
{
    std::unique_ptr<AccountThread> baseThread(arg);
    StickerConversionThread *thread = dynamic_cast<StickerConversionThread *>(arg);
    if (!thread)
        return;

    const PendingContentHandle pendingContent =
        thread->pendingContent();
    const auto discardOutput = [thread]() {
        if (!thread->getOutputFileName().empty())
            remove(thread->getOutputFileName().c_str());
    };
    if (!pendingContent.currentAndNotCancelled()) {
        discardOutput();
        return;
    }

    const td::td_api::chat *chat =
        m_data.getChat(thread->chatId);
    if (!chat) {
        discardOutput();
        return;
    }

    IncomingMessage *pendingMessage =
        pendingContent.valid()
        ? m_data.pendingMessages.findPendingMessage(
              pendingContent)
        : nullptr;
    if (pendingContent.valid() && !pendingMessage &&
        pendingContent.message.disposition() !=
            PendingMessageState::Disposition::Released) {
        discardOutput();
        return;
    }

    std::string  errorMessage = thread->getErrorMessage();
    gchar       *imageData    =  NULL;
    gsize        imageSize    = 0;
    bool         success      = false;
    if (errorMessage.empty()) {
        GError *error = NULL;

        g_file_get_contents(thread->getOutputFileName().c_str(), &imageData, &imageSize, &error);
        if (error) {
            // unlikely error message not worth translating
            errorMessage = formatMessage("Could not read converted file {}: {}", {
                                            thread->getOutputFileName(), error->message});
            g_error_free(error);
        } else
            success = true;
    }
    discardOutput();

    if (success) {
        int id = purple_imgstore_add_with_id (imageData, imageSize, NULL);
        if (pendingMessage) {
            pendingMessage->animatedStickerConverted = true;
            pendingMessage->animatedStickerConvertSuccess = true;
            pendingMessage->animatedStickerImageId = id;
            checkMessageReady(
                pendingContent.message,
                m_transceiver, m_data);
            return;
        } else {
            std::string text = makeInlineImageText(id);
            showMessageText(m_data, *chat, thread->message(), text.c_str(), NULL, PURPLE_MESSAGE_IMAGES);
        }
    } else {
        if (pendingMessage) {
            pendingMessage->animatedStickerConverted = true;
            pendingMessage->animatedStickerConvertSuccess = false;
            const std::shared_ptr<LifetimeState> lifetime =
                m_lifetime;
            checkMessageReady(
                pendingContent.message,
                m_transceiver, m_data);
            pendingMessage = nullptr;
            if (!lifetime->alive)
                return;
            if (!pendingContent.currentAndNotCancelled())
                return;
            chat = m_data.getChat(thread->chatId);
            if (!chat)
                return;
        }
        // TRANSLATOR: In-chat error message, arguments will be a file name and a proper reason
        errorMessage = formatMessage(_("Could not read sticker file {0}: {1}"),
                                        {thread->inputFileName, errorMessage});
        errorMessage = makeNoticeWithSender(*chat, thread->message(), errorMessage.c_str(), m_account);
        showMessageText(m_data, *chat, thread->message(), NULL, errorMessage.c_str());
    }
}

void PurpleTdClient::sendReadReceipts(PurpleConversation *conversation)
{
    if (conversation != NULL) {
        sendConversationReadReceipts(m_data, conversation);
        return;
    }
}

void PurpleTdClient::setOnlineStatus(bool online)
{
    m_transceiver.sendQuery(td::td_api::make_object<td::td_api::setOption>(
        "online", td::td_api::make_object<td::td_api::optionValueBoolean>(online)), nullptr);
}

void PurpleTdClient::setBuddyIcon(PurpleStoredImage *img)
{
    if (!img) {
        const td::td_api::user *selfInfo = m_data.getUserByPhone(purple_account_get_username(m_account));
        if (selfInfo && selfInfo->profile_photo_) {
            uint64_t requestId = m_transceiver.sendQuery(
                td::td_api::make_object<td::td_api::deleteProfilePhoto>(selfInfo->profile_photo_->id_),
                &PurpleTdClient::setProfilePhotoResponse);
            m_data.addPendingRequest<ProfilePhotoRequest>(requestId, nullptr);
        }
        return;
    }

    char *tempFileName = NULL;
    int fd = g_file_open_tmp("tdlib_profile_photo_XXXXXX", &tempFileName, NULL);
    if (fd < 0) {
        purple_notify_error(m_account,
                            // TRANSLATOR: Failure notification title.
                            _("Failed to set profile photo"),
                            // TRANSLATOR: Failure notification content.
                            _("Could not create temporary file"), NULL);
        return;
    }

    size_t imageSize = purple_imgstore_get_size(img);
    const char *imageData = static_cast<const char *>(purple_imgstore_get_data(img));
    size_t remaining = imageSize;
    while (remaining > 0) {
        ssize_t written = write(fd, imageData, remaining);
        if (written <= 0)
            break;
        imageData += written;
        remaining -= written;
    }
    close(fd);
    if (remaining != 0) {
        remove(tempFileName);
        g_free(tempFileName);
        purple_notify_error(m_account,
                            // TRANSLATOR: Failure notification title.
                            _("Failed to set profile photo"),
                            // TRANSLATOR: Failure notification content.
                            _("Could not write temporary file"), NULL);
        return;
    }

    auto request = td::td_api::make_object<td::td_api::setProfilePhoto>(
        td::td_api::make_object<td::td_api::inputChatPhotoStatic>(
            td::td_api::make_object<td::td_api::inputFileLocal>(tempFileName)),
        false);
    uint64_t requestId = m_transceiver.sendQuery(std::move(request), &PurpleTdClient::setProfilePhotoResponse);
    m_data.addPendingRequest<ProfilePhotoRequest>(requestId, tempFileName);
    g_free(tempFileName);
}

void PurpleTdClient::setProfilePhotoResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<ProfilePhotoRequest> request = m_data.getPendingRequest<ProfilePhotoRequest>(requestId);
    if (request && !request->tempFile.empty())
        remove(request->tempFile.c_str());

    if (!object || (object->get_id() != td::td_api::ok::ID)) {
        std::string message = getDisplayedError(object);
        purple_notify_error(m_account,
                            // TRANSLATOR: Failure notification title.
                            _("Failed to set profile photo"),
                            message.c_str(), NULL);
    }
}

void PurpleTdClient::onIncomingMessage(td::td_api::object_ptr<td::td_api::message> message)
{
    if (!message)
        return;
    ChatId chatId = getChatId(*message);
    auto pGap = std::find_if(m_chatGaps.begin(), m_chatGaps.end(),
                             [chatId](const ChatGap &gap) { return (gap.chatId == chatId); });
    if (pGap != m_chatGaps.end()) {
        MessageId lastMessageId = pGap->lastMessage;
        m_chatGaps.erase(pGap);
        purple_debug_misc(config::pluginId,
            "Fetching skipped messages for chat %" G_GINT64_FORMAT
            " between %" G_GINT64_FORMAT " and %" G_GINT64_FORMAT "\n",
            chatId.value(), lastMessageId.value(), getId(*message).value());
        fetchHistory(m_data, chatId, getId(*message), lastMessageId);
    }

    const td::td_api::chat *chat = m_data.getChat(chatId);
    if (!chat) {
        purple_debug_warning(config::pluginId, "Received message with unknown chat id %" G_GINT64_FORMAT "\n",
                            message->chat_id_);
        return;
    }

    const ChatTarget target =
        getMessageRoomTarget(*chat, *message);
    if (!target.valid()) {
        purple_debug_warning(
            config::pluginId,
            "Received message with invalid room target\n");
        return;
    }

    if (isChildForumTopic(target)) {
        const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
        m_forumTopics->ensureForumTopicMetadata(target);
        if (!lifetime->alive)
            return;
        chat = m_data.getChat(chatId);
        if (!chat)
            return;
    }

    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    const bool accepted = handleIncomingMessage(
        m_data, *chat, std::move(message),
        PendingMessageQueue::Append,
        IncomingMessageSource::LiveUpdate);
    if (!lifetime->alive)
        return;
    if (accepted && isChildForumTopic(target))
        openPreparedForumTopicForPendingJoin(target);
}

void PurpleTdClient::unreadReactionsMessageResponse(
    uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<UnreadReactionsRequest> request =
        m_data.getPendingRequest<UnreadReactionsRequest>(requestId);
    if (!request)
        return;

    const td::td_api::chat *chat = m_data.getChat(request->chatId);
    if (!chat)
        return;

    const td::td_api::message *originalMessage = nullptr;
    if (object && (object->get_id() == td::td_api::message::ID))
        originalMessage = static_cast<const td::td_api::message *>(object.get());
    else
        purple_debug_misc(config::pluginId,
                          "Failed to fetch message %" G_GINT64_FORMAT " for unread reactions\n",
                          request->messageId.value());

    ChatTarget target = originalMessage
        ? getMessageRoomTarget(*chat, *originalMessage)
        : ChatTarget::chat(request->chatId);
    bool forumTopicDisplayAccepted = false;
    if (originalMessage &&
        target.chatId() != request->chatId) {
        return;
    }
    if (isChildForumTopic(target)) {
        if (!isEligibleForumParent(m_data, *chat))
            return;
        const TdAccountData::ForumTopicState *topic =
            m_data.findForumTopic(target);
        if (topic && topic->deleted)
            return;
        if (!topic)
            m_data.ensureForumTopicPlaceholder(target);
        if (m_data.activateForumTopic(target) == 0)
            return;
        forumTopicDisplayAccepted = true;

        const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
        m_forumTopics->ensureForumTopicMetadata(target);
        if (!lifetime->alive)
            return;
        chat = m_data.getChat(request->chatId);
        if (!chat)
            return;
    } else if (!originalMessage &&
               isEligibleForumParent(m_data, *chat)) {
        // The update has no topic field of its own. If fetching the
        // referenced message fails, guessing General could leak a
        // topic-specific reaction into the wrong room.
        return;
    } else if (!target.valid()) {
        return;
    }

    std::string quote = formatMessageQuote(originalMessage, m_data);
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    for (const UnreadReactionInfo &reaction: request->reactions) {
        TgMessageInfo messageInfo;
        messageInfo.target = target;
        messageInfo.forumTopicDisplayAccepted =
            forumTopicDisplayAccepted;
        messageInfo.incomingGroupchatSender = reaction.sender;

        std::string text = quote + "\n" + reaction.text;
        showMessageText(m_data, *chat, messageInfo, text.c_str(), nullptr,
                        PURPLE_MESSAGE_NO_LOG);
        if (!lifetime->alive)
            return;
    }
}

void PurpleTdClient::updateChatLastMessage(td::td_api::updateChatLastMessage &lastMessage)
{
    ChatId chatId = getChatId(lastMessage);
    for (auto &chatPosition: lastMessage.positions_)
        m_data.updateChatPosition(chatId, std::move(chatPosition));
    if (lastMessage.last_message_)
        saveChatLastMessage(m_data, chatId, getId(*lastMessage.last_message_));
    else {
        MessageId lastMessageId = getChatLastMessage(m_data, chatId);
        if (lastMessageId.valid()) {
            purple_debug_misc(config::pluginId,
                "Skipped messages detected for chat %" G_GINT64_FORMAT
                ", last seen message %" G_GINT64_FORMAT "\n",
                chatId.value(), lastMessageId.value());
            if (std::find_if(m_chatGaps.begin(), m_chatGaps.end(),
                             [chatId](const ChatGap &gap) {
                                 return (gap.chatId == chatId);
                             }) == m_chatGaps.end()) {
                m_chatGaps.emplace_back();
                m_chatGaps.back().chatId = chatId;
                m_chatGaps.back().lastMessage = lastMessageId;
            }
        }
    }
}

void PurpleTdClient::updateVisibleChatMemberList(td::td_api::updateChatMember &memberUpdate)
{
    const td::td_api::chat *chat = m_data.getChat(chatIdFromTdInt(memberUpdate.chat_id_));
    if (chat && isEligibleForumParent(m_data, *chat)) {
        // Forum rooms share one roster. Keep every projection quiet so a
        // Telegram service message remains the single user-facing notice.
        // The non-forum path below retains libpurple's arrival semantics.
        const td::td_api::chatMember *oldMember =
            memberUpdate.old_chat_member_.get();
        const SupergroupId groupId =
            getSupergroupId(*chat);
        const td::td_api::chatMember *newMember = nullptr;
        if (memberUpdate.new_chat_member_) {
            newMember = m_data.updateSupergroupMember(
                groupId,
                std::move(memberUpdate.new_chat_member_));
        } else if (oldMember) {
            m_data.removeSupergroupMember(
                groupId, getUserId(*oldMember));
        }

        const std::shared_ptr<LifetimeState> lifetime =
            m_lifetime;
        const ContinuationGuard canContinue =
            [lifetime]() {
                return lifetime->alive;
            };
        projectForumChatMemberUpdate(
            m_data, *chat, oldMember, newMember,
            canContinue);
        return;
    }

    PurpleConvChat *purpleChat = chat ? findChatConversation(m_account, *chat) : nullptr;
    if (purpleChat)
        ::updateChatMember(purpleChat, memberUpdate.old_chat_member_.get(),
                           memberUpdate.new_chat_member_.get(), m_data);
}

int PurpleTdClient::sendMessage(const char *buddyName, const char *message)
{
    SecretChatId secretChatId           = purpleBuddyNameToSecretChatId(buddyName);
    const td::td_api::user *privateUser = nullptr;
    const td::td_api::chat *chat        = nullptr;

    if (secretChatId.valid()) {
        chat = m_data.getChatBySecretChat(secretChatId);
        if (!chat) {
            showMessageTextIm(m_data, buddyName, NULL, "Secret chat not found", time(NULL), PURPLE_MESSAGE_ERROR);
            return -1;
        }
    } else {
        std::vector<const td::td_api::user *> users = getUsersByPurpleName(buddyName, m_data, "send message");
        if (users.size() != 1) {
            // Unlikely error messages not worth translating
            std::string errorMessage;
            if (users.empty())
                errorMessage = "User not found";
            else
                errorMessage = formatMessage("More than one user known with name '{}'", std::string(buddyName));
            showMessageTextIm(m_data, buddyName, NULL, errorMessage.c_str(), time(NULL), PURPLE_MESSAGE_ERROR);
            return -1;
        }
        privateUser = users[0];
        chat = m_data.getPrivateChatByUserId(getId(*privateUser));
    }

    if (chat) {
        int ret = transmitMessage(
            ChatTarget::chat(getId(*chat)), message,
            m_transceiver, m_data,
            &PurpleTdClient::sendMessageResponse);
        if (ret < 0)
            return ret;
        // Message shall not be echoed: tdlib will shortly present it as a new message and it will be displayed then
        return 0;
    } else if (privateUser) {
        purpleDebug("Requesting private chat for user id {}", privateUser->id_);
        td::td_api::object_ptr<td::td_api::createPrivateChat> createChat =
            td::td_api::make_object<td::td_api::createPrivateChat>(privateUser->id_, false);
        uint64_t requestId = m_transceiver.sendQuery(std::move(createChat), &PurpleTdClient::sendMessageCreatePrivateChatResponse);
        m_data.addPendingRequest<NewPrivateChatForMessage>(requestId, buddyName, message);
        return 0;
    }

    return -1;
}

void PurpleTdClient::sendMessageResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<SendMessageRequest> request = m_data.getPendingRequest<SendMessageRequest>(requestId);
    if (!request)
        return;
    if (object && (object->get_id() == td::td_api::message::ID)) {
        const td::td_api::message &message =
            static_cast<const td::td_api::message &>(*object);
        ChatTarget target = request->target;
        const td::td_api::chat *chat =
            m_data.getChat(getChatId(message));
        if (chat) {
            const ChatTarget responseTarget =
                getMessageRoomTarget(*chat, message);
            if (responseTarget.valid() &&
                responseTarget != target) {
                purple_debug_warning(
                    config::pluginId,
                    "Send response changed the requested message target\n");
            }
        }
        if (getChatId(message) == target.chatId()) {
            m_data.rememberMessageTarget(
                target, getId(message));
        }
        std::string tempFile;
        tempFile.swap(request->tempFile);
        if (getId(message).valid() &&
            message.sending_state_) {
            m_data.addPendingSend(
                target, getId(message),
                std::move(tempFile));
        } else {
            removeTempFile(tempFile);
        }
    } else {
        // TRANSLATOR: In-chat error message, argument will be a user-sent message
        std::string errorMessage = formatMessage(_("Failed to send message: {}"), getDisplayedError(object));
        if (!request->tempFile.empty()) {
            remove(request->tempFile.c_str());
            request->tempFile.clear();
        }
        showTargetNotification(
            request->target, errorMessage);
    }
}

void PurpleTdClient::sendTyping(const char *buddyName, bool isTyping)
{
    const td::td_api::chat *chat = nullptr;
    SecretChatId secretChatId = purpleBuddyNameToSecretChatId(buddyName);
    if (secretChatId.valid())
        chat = m_data.getChatBySecretChat(secretChatId);
    else {
        std::vector<const td::td_api::user *> users = getUsersByPurpleName(buddyName, m_data, "send typing notification");
        if (users.size() == 1)
            chat = m_data.getPrivateChatByUserId(getId(*users[0]));
    }

    if (chat) {
        auto sendAction = td::td_api::make_object<td::td_api::sendChatAction>();
        sendAction->chat_id_ = chat->id_;
        if (isTyping)
            sendAction->action_ = td::td_api::make_object<td::td_api::chatActionTyping>();
        else
            sendAction->action_ = td::td_api::make_object<td::td_api::chatActionCancel>();
        m_transceiver.sendQuery(std::move(sendAction), nullptr);
    }
}

void PurpleTdClient::updateUserStatus(UserId userId, td::td_api::object_ptr<td::td_api::UserStatus> status)
{
    const td::td_api::user *user = m_data.getUser(userId);
    if (user) {
        std::string userName = getPurpleBuddyName(*user);
        purple_prpl_got_user_status(m_account, userName.c_str(), getPurpleStatusId(*status), NULL);
        m_data.setUserStatus(userId, std::move(status));
    }
}

void PurpleTdClient::updateUser(td::td_api::object_ptr<td::td_api::user> userInfo)
{
    if (!userInfo) {
        purple_debug_warning(config::pluginId, "updateUser with null user info\n");
        return;
    }

    UserId userId = getId(*userInfo);
    m_data.updateUser(std::move(userInfo));

    // For chats, find_chat doesn't work if account is not yet connected, so just in case, don't
    // user find_buddy either.
    // Updates are only supposed to come after authorizationStateReady which sets account to connected.
    // But check purple_account_is_connected just in case.
    if (purple_account_is_connected(m_account)) {
        const td::td_api::user *user = m_data.getUser(userId);
        const td::td_api::chat *chat = m_data.getPrivateChatByUserId(userId);

        if (user)
            updateUserInfo(*user, chat);
    }
}

static bool shouldDownloadAvatar(const td::td_api::file &file)
{
    return (file.local_ && !file.local_->is_downloading_completed_ &&
            !file.local_->is_downloading_active_ && file.remote_ && file.remote_->is_uploading_completed_ &&
            file.local_->can_be_downloaded_);
}

void PurpleTdClient::downloadProfilePhoto(const td::td_api::user &user)
{
    if (user.profile_photo_ && user.profile_photo_->small_ &&
        shouldDownloadAvatar(*user.profile_photo_->small_))
    {
        auto downloadReq = td::td_api::make_object<td::td_api::downloadFile>();
        downloadReq->file_id_ = user.profile_photo_->small_->id_;
        downloadReq->priority_ = FILE_DOWNLOAD_PRIORITY;
        downloadReq->synchronous_ = true;
        uint64_t queryId = m_transceiver.sendQuery(std::move(downloadReq), &PurpleTdClient::avatarDownloadResponse);
        m_data.addPendingRequest<AvatarDownloadRequest>(queryId, &user);
    }
}

void PurpleTdClient::avatarDownloadResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<AvatarDownloadRequest> request = m_data.getPendingRequest<AvatarDownloadRequest>(requestId);
    if (request && object && (object->get_id() == td::td_api::file::ID)) {
        auto file = td::move_tl_object_as<td::td_api::file>(object);
        if (file->local_ && file->local_->is_downloading_completed_) {
            if (request->userId.valid()) {
                m_data.updateSmallProfilePhoto(request->userId, std::move(file));
                const td::td_api::user *user = m_data.getUser(request->userId);
                const td::td_api::chat *chat = m_data.getPrivateChatByUserId(request->userId);
                if (user && chat && isChatInContactList(*chat, user))
                    updatePrivateChat(m_data, chat, *user);
            } else if (request->chatId.valid()) {
                m_data.updateSmallChatPhoto(request->chatId, std::move(file));
                const td::td_api::chat *chat = m_data.getPrivateChatByUserId(request->userId);
                if (chat && isChatInContactList(*chat, nullptr)) {
                    BasicGroupId basicGroupId = getBasicGroupId(*chat);
                    SupergroupId supergroupId = getSupergroupId(*chat);
                    if (basicGroupId.valid())
                        updateBasicGroupChat(m_data, basicGroupId);
                    if (supergroupId.valid())
                        updateSupergroupChat(m_data, supergroupId);
                }
            }
        }
    }
}

void PurpleTdClient::updateGroup(td::td_api::object_ptr<td::td_api::basicGroup> group)
{
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    if (!group) {
        purple_debug_warning(config::pluginId, "updateBasicGroup with null group\n");
        return;
    }

    BasicGroupId id = getId(*group);
    m_data.updateBasicGroup(std::move(group));
    const td::td_api::chat *chat = m_data.getBasicGroupChatByGroup(id);
    const ChatId chatId = chat ? getId(*chat) : ChatId::invalid;
    if (chat && resolveDeferredGroupChat(chatId)) {
        if (!lifetime->alive)
            return;
        projectForumTopics(chatId);
        return;
    }

    // purple_blist_find_chat doesn't work if account is not connected.
    // Updates are only supposed to come after authorizationStateReady which sets account to connected.
    // But check purple_account_is_connected just in case.
    if (purple_account_is_connected(m_account))
        updateBasicGroupChat(
            m_data, id,
            [lifetime]() {
                return lifetime->alive;
            });
    if (!lifetime->alive)
        return;
    if (chatId.valid())
        projectForumTopics(chatId);
}

void PurpleTdClient::updateSupergroup(td::td_api::object_ptr<td::td_api::supergroup> group)
{
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    if (!group) {
        purple_debug_warning(config::pluginId, "updateSupergroup with null group\n");
        return;
    }

    SupergroupId id = getId(*group);
    m_data.updateSupergroup(std::move(group));
    const td::td_api::chat *chat = m_data.getSupergroupChatByGroup(id);
    const ChatId chatId = chat ? getId(*chat) : ChatId::invalid;
    if (chat && resolveDeferredGroupChat(chatId)) {
        if (!lifetime->alive)
            return;
        retryExpectedForumTopicJoins(chatId);
        if (!lifetime->alive)
            return;
        projectForumTopics(chatId);
        return;
    }

    // purple_blist_find_chat doesn't work if account is not connected.
    // Updates are only supposed to come after authorizationStateReady which sets account to connected.
    // But check purple_account_is_connected just in case.
    if (purple_account_is_connected(m_account))
        updateSupergroupChat(
            m_data, id,
            [lifetime]() {
                return lifetime->alive;
            });
    if (!lifetime->alive)
        return;
    if (chatId.valid())
        retryExpectedForumTopicJoins(chatId);
    if (!lifetime->alive)
        return;
    if (chatId.valid())
        projectForumTopics(chatId);
}

bool PurpleTdClient::resolveDeferredGroupChat(ChatId chatId)
{
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    auto deferred = m_deferredGroupChats.find(chatId);
    if (deferred == m_deferredGroupChats.end())
        return false;

    const td::td_api::chat *chat = m_data.getChat(chatId);
    if (!chat) {
        m_deferredGroupChats.erase(deferred);
        markForumRoomListsReadyIfPossible();
        return true;
    }

    const BasicGroupId basicGroupId = getBasicGroupId(*chat);
    const SupergroupId supergroupId = getSupergroupId(*chat);
    const bool metadataKnown =
        (basicGroupId.valid() && m_data.getBasicGroup(basicGroupId)) ||
        (supergroupId.valid() && m_data.getSupergroup(supergroupId));
    if (!metadataKnown)
        return true;

    if (!m_data.isGroupChatWithMembership(*chat)) {
        m_deferredGroupChats.erase(deferred);
        failForumTopicJoins(chatId);
        if (!lifetime->alive)
            return true;
        m_data.deleteChat(chatId);
        markForumRoomListsReadyIfPossible();
        return true;
    }

    if (!purple_account_is_connected(m_account))
        return true;

    m_deferredGroupChats.erase(deferred);
    updateChat(chat);
    if (!lifetime->alive)
        return true;
    markForumRoomListsReadyIfPossible();
    return true;
}

void PurpleTdClient::resolveDeferredGroupChats()
{
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    std::vector<ChatId> chatIds(
        m_deferredGroupChats.begin(), m_deferredGroupChats.end());
    for (ChatId chatId : chatIds) {
        resolveDeferredGroupChat(chatId);
        if (!lifetime->alive)
            return;
    }
}

void PurpleTdClient::markForumRoomListsReadyIfPossible()
{
    if (m_chatListReady && m_deferredGroupChats.empty())
        m_forumTopics->markRoomListsReady();
}

void PurpleTdClient::updateChat(const td::td_api::chat *chat)
{
    if (!chat) return;

    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    const ContinuationGuard canContinue =
        [lifetime]() {
            return lifetime->alive;
        };
    const td::td_api::user *privateChatUser = m_data.getUserByPrivateChat(*chat);
    BasicGroupId            basicGroupId    = getBasicGroupId(*chat);
    SupergroupId            supergroupId    = getSupergroupId(*chat);
    SecretChatId            secretChatId    = getSecretChatId(*chat);
    purpleDebug("Update chat: {} private user={} basic group={} supergroup={}", {
        std::to_string(chat->id_), std::to_string(privateChatUser ? privateChatUser->id_ : 0),
        std::to_string(basicGroupId.value()), std::to_string(supergroupId.value())
    });

    if ((basicGroupId.valid() && !m_data.getBasicGroup(basicGroupId)) ||
        (supergroupId.valid() && !m_data.getSupergroup(supergroupId))) {
        return;
    }

    // For secret chats, chat photo is same as user profile photo, so hopefully already downloaded.
    // But if not (such as when creating secret chat while downloading new photo for the user),
    // then don't bother.
    if (!privateChatUser && !secretChatId.valid())
        downloadChatPhoto(*chat);

    // For chats, find_chat doesn't work if account is not yet connected, so just in case, don't
    // user find_buddy either.
    // Updates are only supposed to come after authorizationStateReady which sets account to connected.
    // But check purple_account_is_connected just in case.
    if (!purple_account_is_connected(m_account))
        return;

    if (privateChatUser)
        updateUserInfo(*privateChatUser, chat);

    if (isChatInContactList(*chat, privateChatUser)) {
        // purple_blist_find_chat doesn't work if account is not connected
        if (basicGroupId.valid()) {
            requestBasicGroupFullInfo(basicGroupId);
            updateBasicGroupChat(
                m_data, basicGroupId, canContinue);
            if (!lifetime->alive)
                return;
        }
        if (supergroupId.valid()) {
            requestSupergroupFullInfo(supergroupId);
            updateSupergroupChat(
                m_data, supergroupId, canContinue);
            if (!lifetime->alive)
                return;
        }
    } else {
        if (basicGroupId.valid() || supergroupId.valid())
            removeGroupChat(m_account, *chat);
    }

    if (secretChatId.valid())
        updateKnownSecretChat(secretChatId, m_transceiver, m_data);
}

void PurpleTdClient::updateUserInfo(const td::td_api::user &user, const td::td_api::chat *privateChat)
{
    const UserId userId = getId(user);
    const std::shared_ptr<LifetimeState> lifetime =
        m_lifetime;
    const ContinuationGuard canContinue =
        [lifetime]() {
            return lifetime->alive;
        };
    if (privateChat) {
        if (isChatInContactList(*privateChat, &user)) {
            downloadProfilePhoto(user);
            updatePrivateChat(m_data, privateChat, user);
        } else
            removePrivateChat(m_data, *privateChat);
        if (!lifetime->alive)
            return;
    }

    // Forum topic rooms are projections of one parent membership list.
    // Refresh every active projection when a cached member's display name
    // changes, without creating conversations for inactive topics.
    const std::vector<SupergroupId> supergroups =
        m_data.getSupergroupsWithMember(userId);
    for (SupergroupId groupId : supergroups) {
        const td::td_api::chat *groupChat =
            m_data.getSupergroupChatByGroup(groupId);
        const td::td_api::chatMembers *members =
            m_data.getSupergroupMembers(groupId);
        if (groupChat && members &&
            isEligibleForumParent(m_data, *groupChat)) {
            if (!projectForumChatMembers(
                    m_data, *groupChat, *members,
                    canContinue)) {
                return;
            }
        }
    }

    // User could have renamed, or they may have become, or ceased being, libpurple buddy.
    // Update member list in all chat conversation where this user is a member.
    std::vector<std::pair<BasicGroupId, const td::td_api::basicGroupFullInfo *>> groups;
    groups = m_data.getBasicGroupsWithMember(userId);
    for (const auto &groupInfo: groups) {
        const td::td_api::chat *groupChat = m_data.getBasicGroupChatByGroup(groupInfo.first);
        PurpleConvChat *purpleChat = groupChat ? findChatConversation(m_account, *groupChat) : nullptr;
        if (purpleChat)
            updateChatConversation(purpleChat, *groupInfo.second, m_data);
        if (!lifetime->alive)
            return;
    }
}

void PurpleTdClient::downloadChatPhoto(const td::td_api::chat &chat)
{
    if (chat.photo_ && chat.photo_->small_ && shouldDownloadAvatar(*chat.photo_->small_)) {
        auto downloadReq = td::td_api::make_object<td::td_api::downloadFile>();
        downloadReq->file_id_ = chat.photo_->small_->id_;
        downloadReq->priority_ = FILE_DOWNLOAD_PRIORITY;
        downloadReq->synchronous_ = true;
        uint64_t queryId = m_transceiver.sendQuery(std::move(downloadReq), &PurpleTdClient::avatarDownloadResponse);
        m_data.addPendingRequest<AvatarDownloadRequest>(queryId, &chat);
    }
}

void PurpleTdClient::addChat(td::td_api::object_ptr<td::td_api::chat> chat)
{
    if (!chat) {
        purple_debug_warning(config::pluginId, "updateNewChat with null chat info\n");
        return;
    }

    purple_debug_misc(config::pluginId, "Add chat: '%s'\n", chat->title_.c_str());
    ChatId chatId = getId(*chat);
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    m_data.addChat(std::move(chat));
    updateChat(m_data.getChat(chatId));
    if (!lifetime->alive)
        return;
    retryExpectedForumTopicJoins(chatId);
    if (!lifetime->alive)
        return;
    projectForumTopics(chatId);
}

void PurpleTdClient::handleUserChatAction(const td::td_api::updateChatAction &updateChatAction)
{
    const td::td_api::chat *chat = m_data.getChat(getChatId(updateChatAction));
    if (!chat) {
        purple_debug_warning(config::pluginId, "Got user chat action for unknown chat %" G_GINT64_FORMAT "\n",
                             updateChatAction.chat_id_);
        return;
    }

    UserId chatUserId = getUserIdByPrivateChat(*chat);
    if (!chatUserId.valid()) {
        purple_debug_misc(config::pluginId, "Ignoring user chat action for non-private chat %" G_GINT64_FORMAT "\n",
                          updateChatAction.chat_id_);
        return;
    }

    if (chatUserId != getUserId(updateChatAction)) {
        purpleDebug("Got user action for private chat {} (with user {}) for another user {}", {
            std::to_string(updateChatAction.chat_id_), std::to_string(chatUserId.value()),
            std::to_string(getUserId(updateChatAction))
        });
    } else if (updateChatAction.action_) {
        if (updateChatAction.action_->get_id() == td::td_api::chatActionCancel::ID) {
            purpleDebug("User (id {}) stopped chat action", getUserId(updateChatAction));
            showUserChatAction(getUserId(updateChatAction), false);
        } else if (updateChatAction.action_->get_id() == td::td_api::chatActionStartPlayingGame::ID) {
            purpleDebug("User (id %d): treating chatActionStartPlayingGame as cancel", getUserId(updateChatAction));
            showUserChatAction(getUserId(updateChatAction), false);
        } else {
            purpleDebug("User (id {}) started chat action (id {})", {
		std::to_string(getUserId(updateChatAction)), std::to_string(updateChatAction.action_->get_id())
            });
            showUserChatAction(getUserId(updateChatAction), true);
        }
    }
}

void PurpleTdClient::showUserChatAction(UserId userId, bool isTyping)
{
    const td::td_api::user *user = m_data.getUser(userId);
    if (user) {
        std::string userName = getPurpleBuddyName(*user);
        if (isTyping)
            serv_got_typing(purple_account_get_connection(m_account),
                            userName.c_str(), REMOTE_TYPING_NOTICE_TIMEOUT,
                            PURPLE_TYPING);
        else
            serv_got_typing_stopped(purple_account_get_connection(m_account),
                                    userName.c_str());
    }
}

static void showFailedContactMessage(void *handle, const std::string &errorMessage)
{
    // TRANSLATOR: Error dialog, content
    std::string message = formatMessage(_("Failed to add contact: {}"), errorMessage);
    // TRANSLATOR: Error dialog, title
    purple_notify_error(handle, _("Failed to add contact"), message.c_str(), NULL);
}

static int failedContactIdle(gpointer userdata)
{
    char *message = static_cast<char *>(userdata);
    showFailedContactMessage(NULL, message);
    free(message);
    return FALSE; // This idle callback will not be called again
}

static void notifyFailedContactDeferred(const std::string &message)
{
    g_idle_add(failedContactIdle, strdup(message.c_str()));
}

void PurpleTdClient::addContact(const std::string &purpleName, const std::string &alias,
                                const std::string &groupName)
{
    if (m_data.getUserByPhone(purpleName.c_str())) {
        purple_debug_info(config::pluginId, "User with that phone number already exists\n");
        return;
    }

    std::vector<const td::td_api::user *> users;
    m_data.getUsersByDisplayName(purpleName.c_str(), users);
    if (users.size() > 1) {
        notifyFailedContactDeferred(formatMessage("More than one user known with name '{}'", purpleName));
        return;
    }

    if (users.size() == 1)
        addContactById(getId(*users[0]), "", purpleName, groupName);
    else if (isPhoneNumber(purpleName.c_str())) {
        td::td_api::object_ptr<td::td_api::importedContact> contact =
            td::td_api::make_object<td::td_api::importedContact>(purpleName, "", "", nullptr);
        td::td_api::object_ptr<td::td_api::changeImportedContacts> importReq =
            td::td_api::make_object<td::td_api::changeImportedContacts>();
        importReq->contacts_.push_back(std::move(contact));
        uint64_t requestId = m_transceiver.sendQuery(std::move(importReq),
                                                     &PurpleTdClient::importContactResponse);

        m_data.addPendingRequest<ContactRequest>(requestId, purpleName, alias, groupName, UserId::invalid);
    } else {
        auto     request   = td::td_api::make_object<td::td_api::searchPublicChat>(purpleName);
        uint64_t requestId = m_transceiver.sendQuery(std::move(request), &PurpleTdClient::addBuddySearchChatResponse);
        m_data.addPendingRequest<ContactRequest>(requestId, "", alias, groupName, UserId::invalid);
    }
}

void PurpleTdClient::addBuddySearchChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<ContactRequest> request = m_data.getPendingRequest<ContactRequest>(requestId);

    if (object && (object->get_id() == td::td_api::chat::ID)) {
        const td::td_api::chat *chat = static_cast<const td::td_api::chat *>(object.get());
        int32_t chatType = chat->type_ ? chat->type_->get_id() : 0;
        if (chatType == td::td_api::chatTypePrivate::ID) {
            if (request)
                addContactById(getUserIdByPrivateChat(*chat), "", request->alias, request->groupName);
        } else if ((chatType == td::td_api::chatTypeBasicGroup::ID) ||
                   (chatType == td::td_api::chatTypeSupergroup::ID))
        {
            // When trying to join a group but finding a user instead, we display an error message.
            // When it's vice versa here, don't make it an error: there are enough error messages to
            // translate as it is.
            joinGroupSearchChatResponse(requestId, std::move(object));
            chat = NULL;
        }
    } else
        notifyFailedContact(getDisplayedError(object));
}

void PurpleTdClient::addContactById(UserId userId, const std::string &phoneNumber, const std::string &alias,
                                    const std::string &groupName)
{
    purpleDebug("Adding contact: id={} alias={}", {std::to_string(userId.value()), alias});
    std::string firstName, lastName;
    getNamesFromAlias(alias.c_str(), firstName, lastName);

    td::td_api::object_ptr<td::td_api::importedContact> contact =
        td::td_api::make_object<td::td_api::importedContact>(phoneNumber, firstName, lastName, nullptr);
    td::td_api::object_ptr<td::td_api::addContact> addContact =
        td::td_api::make_object<td::td_api::addContact>(userId.value(), std::move(contact), true);
    uint64_t newRequestId = m_transceiver.sendQuery(std::move(addContact),
                                                    &PurpleTdClient::addContactResponse);
    m_data.addPendingRequest<ContactRequest>(newRequestId, phoneNumber, alias, groupName, userId);
}

void PurpleTdClient::importContactResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<ContactRequest> request = m_data.getPendingRequest<ContactRequest>(requestId);
    if (!request)
        return;

    UserId userId = UserId::invalid;
    if (object && (object->get_id() == td::td_api::importedContacts::ID)) {
        td::td_api::object_ptr<td::td_api::importedContacts> reply =
            td::move_tl_object_as<td::td_api::importedContacts>(object);
        if (!reply->user_ids_.empty())
            userId = getUserId(*reply, 0);
    }

    if (userId.valid())
        addContactById(userId, request->phoneNumber, request->alias, request->groupName);
    else {
        // TRANSLATOR: Buddy-window error message, title (no content), argument will be a phone number.
        notifyFailedContact(formatMessage(_("No user found with phone number '{}'"), request->phoneNumber));
    }
}

void PurpleTdClient::addContactResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<ContactRequest> request = m_data.getPendingRequest<ContactRequest>(requestId);
    if (!request)
        return;

    if (object && (object->get_id() == td::td_api::ok::ID)) {
        td::td_api::object_ptr<td::td_api::createPrivateChat> createChat =
            td::td_api::make_object<td::td_api::createPrivateChat>(request->userId.value(), false);
        uint64_t newRequestId = m_transceiver.sendQuery(std::move(createChat),
                                                        &PurpleTdClient::addContactCreatePrivateChatResponse);
        m_data.addPendingRequest(newRequestId, std::move(request));
    } else
        notifyFailedContact(getDisplayedError(object));
}

void PurpleTdClient::addContactCreatePrivateChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<ContactRequest> request = m_data.getPendingRequest<ContactRequest>(requestId);
    if (!request)
        return;

    if (object && (object->get_id() == td::td_api::chat::ID)) {
        const td::td_api::chat &chat = static_cast<const td::td_api::chat &>(*object);
        const td::td_api::user *user = m_data.getUserByPrivateChat(chat);
        if (user && !isChatInContactList(chat, user)) {
            // Normally, the user will become a contact and this won't happen. But it does happen
            // when adding BotFather, for example. Nothing will be added to buddy list, so open chat
            // window just to make something happen.
            // Since the user is not in buddy list, we have to use display name as libpurple name,
            // otherwise conversation title will be idXXXXXXX
            std::string displayName = m_data.getDisplayName(*user);
            getImConversation(m_account, displayName.c_str());
        }
    } else {
        purple_debug_misc(config::pluginId,
                          "Failed to create private chat for contact\n");
        notifyFailedContact(getDisplayedError(object));
    }
}

void PurpleTdClient::notifyFailedContact(const std::string &errorMessage)
{
    showFailedContactMessage(purple_account_get_connection(m_account), errorMessage);
}

void PurpleTdClient::renameContact(const char *buddyName, const char *newAlias)
{
    UserId userId = purpleBuddyNameToUserId(buddyName);
    if (!userId.valid()) {
        purple_debug_warning(config::pluginId, "Cannot rename %s: not a valid id\n", buddyName);
        return;
    }

    std::string firstName, lastName;
    getNamesFromAlias(newAlias, firstName, lastName);
    auto contact    = td::td_api::make_object<td::td_api::importedContact>("", firstName, lastName, nullptr);
    auto addContact = td::td_api::make_object<td::td_api::addContact>(userId.value(), std::move(contact), true);
    m_transceiver.sendQuery(std::move(addContact), nullptr);
}

void PurpleTdClient::removeContactAndPrivateChat(const std::string &buddyName)
{
    const td::td_api::chat *chat         = nullptr;
    UserId                  userId       = purpleBuddyNameToUserId(buddyName.c_str());
    SecretChatId            secretChatId = purpleBuddyNameToSecretChatId(buddyName.c_str());

    if (userId.valid())
        chat = m_data.getPrivateChatByUserId(userId);
    else if (secretChatId.valid())
        chat = m_data.getChatBySecretChat(secretChatId);

    if (chat) {
        ChatId chatId = getId(*chat);
        chat = nullptr;
        // Prevent accidentally re-creating buddy if any updateChat* or updateUser arrives
        m_data.deleteChat(chatId);

        auto deleteChat = td::td_api::make_object<td::td_api::deleteChatHistory>();
        deleteChat->chat_id_ = chatId.value();
        deleteChat->remove_from_chat_list_ = true;
        deleteChat->revoke_ = false;
        m_transceiver.sendQuery(std::move(deleteChat), nullptr);
    }

    if (userId.valid()) {
        auto removeContact = td::td_api::make_object<td::td_api::removeContacts>();
        removeContact->user_ids_.push_back(userId.value());
        m_transceiver.sendQuery(std::move(removeContact), nullptr);
    }

    if (secretChatId.valid()) {
        auto closeChat = td::td_api::make_object<td::td_api::closeSecretChat>(secretChatId.value());
        m_transceiver.sendQuery(std::move(closeChat), nullptr);
    }
}

void PurpleTdClient::getUsers(const char *username, std::vector<const td::td_api::user *> &users)
{
    users = getUsersByPurpleName(username, m_data, NULL);
}

bool PurpleTdClient::joinChat(const char *chatName)
{
    const std::shared_ptr<LifetimeState> lifetime =
        m_lifetime;
    const ContinuationGuard canContinue =
        [lifetime]() {
            return lifetime->alive;
        };
    const ChatTarget target = parsePurpleChatName(chatName);
    if (!target.valid())
        return false;
    if (isChildForumTopic(target))
        return joinForumTopic(target);

    ChatId                  id       = target.chatId();
    const td::td_api::chat *chat     = m_data.getChat(id);
    int32_t                 purpleId = m_data.getPurpleChatId(id);
    PurpleConvChat         *conv     = NULL;
    bool                     accepted = false;

    if (!chat) {
        // Either the user is actively trying to join non-existent chat (for example by entering
        // a chat ID when joining with pidgin), or this is the pidgin auto-rejoin that usually
        // happens before we get info about telegram chats.
        // Check if the latter is the case and if it is, schedule to rejoin when telegram chat appears
        PurpleConversation *baseConv = purple_find_conversation_with_account(PURPLE_CONV_TYPE_CHAT,
                                                                             chatName, m_account);
        if (baseConv && purple_conv_chat_has_left(purple_conversation_get_chat_data(baseConv))) {
            purple_debug_misc(config::pluginId, "Scheduling to rejoin group chat %s - "
                              "no telegram chat found at the moment\n", chatName);
            m_data.addExpectedChat(id);
            accepted = true;
        } else
            purple_debug_warning(config::pluginId, "No telegram chat found for purple name %s\n", chatName);
    } else if (!m_data.isGroupChatWithMembership(*chat))
        purple_debug_warning(config::pluginId, "Chat %s (%s) is not a group we a member of\n",
                             chatName, chat->title_.c_str());
    else if (purpleId) {
        conv = getChatConversation(
            m_data, *chat, purpleId, canContinue);
        if (!lifetime->alive)
            return false;
        if (conv)
            purple_conversation_present(purple_conv_chat_get_conversation(conv));
        if (!lifetime->alive)
            return true;
    }

    return conv || accepted;
}

bool PurpleTdClient::joinForumTopic(ChatTarget target)
{
    if (!isChildForumTopic(target))
        return false;

    const std::string chatName = getPurpleChatName(target);
    PurpleConversation *baseConv =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, chatName.c_str(), m_account);
    const bool isRejoin =
        baseConv &&
        purple_conv_chat_has_left(
            purple_conversation_get_chat_data(baseConv));
    PurpleChat *bookmark = purple_account_is_connected(m_account)
        ? purple_blist_find_chat(m_account, chatName.c_str())
        : nullptr;
    if (purple_account_is_connected(m_account))
        m_data.setForumTopicSaved(target, bookmark != nullptr);

    const td::td_api::chat *parent =
        m_data.getChat(target.chatId());
    if (!parent) {
        if (isRejoin || bookmark) {
            m_data.addExpectedChat(target);
            return true;
        }
        return false;
    }

    const SupergroupId groupId =
        getSupergroupId(*parent);
    if (!groupId.valid())
        return false;

    const td::td_api::supergroup *group =
        m_data.getSupergroup(groupId);
    if (!group) {
        if (isRejoin || bookmark) {
            m_data.addExpectedChat(target);
            return true;
        }
        return false;
    }

    if (!isEligibleForumParent(m_data, *parent))
        return false;

    const bool persistentIntent = isRejoin || bookmark;
    const ForumTopicJoinIntent intent = persistentIntent
        ? ForumTopicJoinIntent::PersistentRejoin
        : ForumTopicJoinIntent::UserRequest;
    if (++m_lastForumTopicJoinSerial == 0)
        ++m_lastForumTopicJoinSerial;
    const uint64_t joinSerial = m_lastForumTopicJoinSerial;
    const TdAccountData::ForumTopicState *topicAtStart =
        m_data.findForumTopic(target);
    const uint64_t liveMessageGenerationAtStart =
        topicAtStart
        ? topicAtStart->lastLiveMessageGeneration
        : 0;
    auto pendingJoin =
        m_pendingForumTopicJoins.emplace(
            target,
            PendingForumTopicJoin(
                intent, joinSerial,
                liveMessageGenerationAtStart));
    if (!pendingJoin.second) {
        if (!persistentIntent) {
            pendingJoin.first->second.intent =
                ForumTopicJoinIntent::UserRequest;
        }
        return true;
    }

    m_forumTopics->resolveForumTopic(
        target,
        [this, joinSerial](const ForumTopicLookupResult &result) {
            completeForumTopicJoin(result, joinSerial);
        });
    return true;
}

bool PurpleTdClient::satisfyForumTopicJoinIfOpen(
    ChatTarget target)
{
    if (!isChildForumTopic(target))
        return false;

    const TdAccountData::ForumTopicState *topic =
        m_data.findForumTopic(target);
    if (!topic || !topic->active || topic->purpleId == 0)
        return false;

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            getPurpleChatName(target).c_str(), m_account);
    PurpleConvChat *chat = conversation
        ? purple_conversation_get_chat_data(conversation)
        : nullptr;
    if (!chat || purple_conv_chat_has_left(chat) ||
        purple_conv_chat_get_id(chat) != topic->purpleId ||
        m_data.getChatTargetByPurpleId(topic->purpleId) !=
            target) {
        return false;
    }

    m_pendingForumTopicJoins.erase(target);
    m_data.removeExpectedChat(target);
    return true;
}

void PurpleTdClient::openPreparedForumTopicForPendingJoin(
    ChatTarget target)
{
    if (!isChildForumTopic(target) ||
        (m_pendingForumTopicJoins.find(target) ==
             m_pendingForumTopicJoins.end() &&
         !m_data.isExpectedChat(target))) {
        return;
    }

    const td::td_api::chat *parent =
        m_data.getChat(target.chatId());
    const TdAccountData::ForumTopicState *topic =
        m_data.findForumTopic(target);
    if (!parent || !isEligibleForumParent(m_data, *parent) ||
        !topic || topic->deleted) {
        return;
    }

    const bool wasActive = topic->active;
    const int32_t purpleId =
        m_data.activateForumTopic(target);
    if (purpleId == 0)
        return;

    const std::string displayTitle =
        getForumTopicDisplayTitle(*parent, *topic);
    const std::shared_ptr<LifetimeState> lifetime =
        m_lifetime;
    const ContinuationGuard canContinue =
        [lifetime]() {
            return lifetime->alive;
        };
    PurpleConvChat *conversation = getChatConversation(
        m_data, *parent, target, purpleId,
        displayTitle, canContinue);
    if (!lifetime->alive)
        return;
    if (!conversation ||
        !satisfyForumTopicJoinIfOpen(target)) {
        if (!wasActive)
            m_data.deactivateForumTopic(target);
    }
}

void PurpleTdClient::completeForumTopicJoin(
    const ForumTopicLookupResult &result,
    uint64_t joinSerial)
{
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    const ContinuationGuard canContinue =
        [lifetime]() {
            return lifetime->alive;
        };
    const ChatTarget target = result.target;
    auto pending = m_pendingForumTopicJoins.find(target);
    if (pending == m_pendingForumTopicJoins.end() ||
        pending->second.serial != joinSerial) {
        return;
    }
    const bool revalidatePersistentIntent =
        pending->second.intent ==
            ForumTopicJoinIntent::PersistentRejoin;
    const uint64_t liveMessageGenerationAtStart =
        pending->second.liveMessageGenerationAtStart;

    const std::string purpleName = getPurpleChatName(target);
    PurpleChat *bookmark = purple_account_is_connected(m_account)
        ? purple_blist_find_chat(m_account, purpleName.c_str())
        : nullptr;
    PurpleConversation *existingConversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), m_account);
    m_data.setForumTopicSaved(target, bookmark != nullptr);
    if (revalidatePersistentIntent &&
        !bookmark && !existingConversation) {
        m_pendingForumTopicJoins.erase(pending);
        m_data.removeExpectedChat(target);
        return;
    }

    const TdAccountData::ForumTopicState *topic =
        m_data.findForumTopic(target);
    const td::td_api::chat *parent =
        m_data.getChat(target.chatId());
    if (result.status != ForumTopicLookupStatus::Available ||
        !topic || !topic->metadataKnown || topic->deleted ||
        !parent || !isEligibleForumParent(m_data, *parent)) {
        if (topic &&
            topic->lastLiveMessageGeneration >
                liveMessageGenerationAtStart &&
            satisfyForumTopicJoinIfOpen(target)) {
            return;
        }
        m_pendingForumTopicJoins.erase(pending);
        m_data.removeExpectedChat(target);
        if (result.tdlibErrorCode != 0) {
            purple_debug_warning(
                config::pluginId,
                "Failed to open forum topic in chat %" G_GINT64_FORMAT
                " (topic %d, TDLib code %d)\n",
                target.chatId().value(),
                target.forumTopicId().value(),
                result.tdlibErrorCode);
        }
        failForumTopicJoin(target);
        return;
    }

    m_pendingForumTopicJoins.erase(pending);
    m_data.removeExpectedChat(target);

    const int32_t purpleId = m_data.activateForumTopic(target);
    if (purpleId == 0) {
        failForumTopicJoin(target);
        return;
    }

    const std::string displayTitle =
        getForumTopicDisplayTitle(*parent, *topic);
    if (bookmark) {
        const char *bookmarkTitle = purple_chat_get_name(bookmark);
        if (!bookmarkTitle || displayTitle != bookmarkTitle)
            purple_blist_alias_chat(bookmark, displayTitle.c_str());
        if (!lifetime->alive)
            return;
    }

    PurpleConvChat *conversation = getChatConversation(
        m_data, *parent, target, purpleId, displayTitle,
        canContinue);
    if (!lifetime->alive)
        return;
    if (!conversation) {
        m_data.deactivateForumTopic(target);
        failForumTopicJoin(target);
        return;
    }

    PurpleConversation *baseConversation =
        purple_conv_chat_get_conversation(conversation);
    const char *currentTitle =
        purple_conversation_get_title(baseConversation);
    if (!currentTitle || displayTitle != currentTitle)
        purple_conversation_set_title(
            baseConversation, displayTitle.c_str());
    if (!lifetime->alive)
        return;
    baseConversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            getPurpleChatName(target).c_str(), m_account);
    if (!baseConversation)
        return;
    purple_conversation_present(baseConversation);
}

void PurpleTdClient::failForumTopicJoin(ChatTarget target)
{
    if (!target.valid())
        return;

    PurpleConnection *connection =
        purple_account_get_connection(m_account);
    if (!connection)
        return;

    GHashTable *components = getChatComponents(target);
    purple_serv_got_join_chat_failed(
        connection, components);
    g_hash_table_destroy(components);
}

void PurpleTdClient::failForumTopicJoins(ChatId chatId)
{
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    pruneAbandonedForumTopicJoins(chatId);

    std::set<ChatTarget> failedTargets;
    std::vector<ChatTarget> expectedTargets;
    m_data.getExpectedForumTopics(chatId, expectedTargets);
    failedTargets.insert(
        expectedTargets.begin(), expectedTargets.end());

    for (auto pending = m_pendingForumTopicJoins.begin();
         pending != m_pendingForumTopicJoins.end();) {
        if (pending->first.chatId() == chatId) {
            const ChatTarget target = pending->first;
            failedTargets.insert(target);
            m_forumTopics->cancelForumTopicLookup(target);
            pending = m_pendingForumTopicJoins.erase(pending);
        } else {
            ++pending;
        }
    }

    for (ChatTarget target : failedTargets) {
        m_data.removeExpectedChat(target);
        failForumTopicJoin(target);
        if (!lifetime->alive)
            return;
    }
}

void PurpleTdClient::pruneAbandonedForumTopicJoins(ChatId chatId)
{
    if (!purple_account_is_connected(m_account))
        return;

    std::vector<ChatTarget> expectedTargets;
    m_data.getExpectedForumTopics(chatId, expectedTargets);
    for (ChatTarget target : expectedTargets) {
        if (hasPersistentForumTopicJoin(m_account, target))
            continue;

        m_data.removeExpectedChat(target);
        m_data.setForumTopicSaved(target, false);
    }

    for (auto pending = m_pendingForumTopicJoins.begin();
         pending != m_pendingForumTopicJoins.end();) {
        const ChatTarget target = pending->first;
        if (target.chatId() != chatId ||
            pending->second.intent !=
                ForumTopicJoinIntent::PersistentRejoin ||
            hasPersistentForumTopicJoin(m_account, target)) {
            ++pending;
            continue;
        }

        pending = m_pendingForumTopicJoins.erase(pending);
        m_data.removeExpectedChat(target);
        m_data.setForumTopicSaved(target, false);
    }
}

void PurpleTdClient::retryExpectedForumTopicJoins(ChatId chatId)
{
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    pruneAbandonedForumTopicJoins(chatId);

    std::vector<ChatTarget> targets;
    m_data.getExpectedForumTopics(chatId, targets);
    const bool pendingLookup = std::any_of(
        m_pendingForumTopicJoins.begin(),
        m_pendingForumTopicJoins.end(),
        [chatId](const auto &entry) {
            return entry.first.chatId() == chatId;
        });
    if (targets.empty() && !pendingLookup)
        return;

    const td::td_api::chat *parent = m_data.getChat(chatId);
    if (!parent)
        return;
    const SupergroupId groupId = getSupergroupId(*parent);
    if (!groupId.valid()) {
        failForumTopicJoins(chatId);
        return;
    }
    const td::td_api::supergroup *group =
        m_data.getSupergroup(groupId);
    if (!group)
        return;

    if (!isEligibleForumParent(m_data, *parent)) {
        failForumTopicJoins(chatId);
        return;
    }

    for (ChatTarget target : targets) {
        joinForumTopic(target);
        if (!lifetime->alive)
            return;
    }
}

void PurpleTdClient::projectForumTopic(ChatTarget target)
{
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    if (!isChildForumTopic(target))
        return;

    const TdAccountData::ForumTopicState *topic =
        m_data.findForumTopic(target);
    const td::td_api::chat *parent =
        m_data.getChat(target.chatId());
    const bool parentEligible =
        parent && isEligibleForumParent(m_data, *parent);
    if (topic && topic->metadataKnown && !parentEligible) {
        m_data.invalidateForumTopicMetadata(
            target, m_data.reserveForumTopicGeneration());
    }
    const bool available =
        topic && topic->metadataKnown && !topic->deleted &&
        parentEligible;
    if (topic && topic->active && !topic->metadataKnown &&
        !topic->deleted && parentEligible) {
        m_forumTopics->ensureForumTopicMetadata(target);
        return;
    }

    const std::string purpleName = getPurpleChatName(target);
    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), m_account);
    PurpleChat *bookmark = nullptr;
    if (purple_account_is_connected(m_account)) {
        bookmark = purple_blist_find_chat(
            m_account, purpleName.c_str());
        m_data.setForumTopicSaved(target, bookmark != nullptr);
    }
    if (!available) {
        m_data.deactivateForumTopic(target);
        if (conversation) {
            PurpleConvChat *chat =
                purple_conversation_get_chat_data(conversation);
            if (chat && !purple_conv_chat_has_left(chat)) {
                PurpleConnection *connection =
                    purple_account_get_connection(m_account);
                const int32_t purpleId =
                    purple_conv_chat_get_id(chat);
                if (connection &&
                    purple_find_chat(connection, purpleId) ==
                        conversation) {
                    serv_got_chat_left(connection, purpleId);
                } else {
                    // A stale numeric ID can point at another room. Leave the
                    // exact named conversation directly in that rare case.
                    purple_conv_chat_left(chat);
                }
            }
        }
        return;
    }

    const std::string displayTitle =
        getForumTopicDisplayTitle(*parent, *topic);

    if (bookmark) {
        const char *bookmarkTitle =
            purple_chat_get_name(bookmark);
        if (!bookmarkTitle || displayTitle != bookmarkTitle)
            purple_blist_alias_chat(
                bookmark, displayTitle.c_str());
        if (!lifetime->alive)
            return;
        conversation =
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT,
                purpleName.c_str(), m_account);
    }

    if (conversation) {
        const char *currentTitle =
            purple_conversation_get_title(conversation);
        if (!currentTitle || displayTitle != currentTitle)
            purple_conversation_set_title(
                conversation, displayTitle.c_str());
    }
}

void PurpleTdClient::suspendForumTopics(ChatId chatId)
{
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    failForumTopicJoins(chatId);
    if (!lifetime->alive)
        return;

    std::vector<const TdAccountData::ForumTopicState *> topics;
    m_data.getForumTopics(chatId, topics);
    const uint64_t generation =
        m_data.reserveForumTopicGeneration();
    for (const TdAccountData::ForumTopicState *topic : topics) {
        if (!topic || topic->isGeneral())
            continue;

        const ChatTarget target = topic->target;
        m_forumTopics->cancelForumTopicLookup(target);
        m_data.invalidateForumTopicMetadata(
            target, generation);
        projectForumTopic(target);
        if (!lifetime->alive)
            return;
    }
}

void PurpleTdClient::projectForumTopics(ChatId chatId)
{
    const std::shared_ptr<LifetimeState> lifetime = m_lifetime;
    const td::td_api::chat *parent =
        m_data.getChat(chatId);
    if (!parent) {
        suspendForumTopics(chatId);
        return;
    }
    if (!isEligibleForumParent(m_data, *parent)) {
        suspendForumTopics(chatId);
        return;
    }

    std::vector<const TdAccountData::ForumTopicState *> topics;
    m_data.getForumTopics(chatId, topics);
    for (const TdAccountData::ForumTopicState *topic : topics) {
        if (topic)
            projectForumTopic(topic->target);
        if (!lifetime->alive)
            return;
    }
}

void PurpleTdClient::closeConversation(const char *conversationName)
{
    const ChatTarget target =
        parsePurpleChatName(conversationName);
    if (!isChildForumTopic(target))
        return;

    m_pendingForumTopicJoins.erase(target);
    m_data.removeExpectedChat(target);
    m_data.deactivateForumTopic(target);
    if (purple_account_is_connected(m_account)) {
        const std::string purpleName = getPurpleChatName(target);
        m_data.setForumTopicSaved(
            target,
            purple_blist_find_chat(
                m_account, purpleName.c_str()) != nullptr);
    }
}

void PurpleTdClient::ensureForumTopicMetadata(
    ChatTarget target)
{
    m_forumTopics->ensureForumTopicMetadata(target);
}

int PurpleTdClient::sendGroupMessage(int purpleChatId, const char *message)
{
    const ChatTarget target =
        m_data.getChatTargetByPurpleId(purpleChatId);
    if (!hasSafeConversationTargetForSend(
            m_account, purpleChatId, target)) {
        purple_debug_warning(
            config::pluginId,
            "Refusing mismatched topic conversation for purple id %d\n",
            purpleChatId);
        return -1;
    }

    const td::td_api::chat *chat =
        target.valid() ? m_data.getChat(target.chatId()) : nullptr;

    if (!chat)
        purple_debug_warning(config::pluginId, "No chat found for purple id %d\n", purpleChatId);
    else if (!m_data.isGroupChatWithMembership(*chat))
        purple_debug_misc(config::pluginId, "purple id %d (chat %s) is not a group we a member of\n",
                             purpleChatId, chat->title_.c_str());
    else {
        if (isChildForumTopic(target)) {
            const TdAccountData::ForumTopicState *topic =
                m_data.findForumTopic(target);
            if (!topic || topic->deleted || !topic->active ||
                !isEligibleForumParent(m_data, *chat) ||
                m_data.getPurpleChatId(target) != purpleChatId) {
                purple_debug_warning(
                    config::pluginId,
                    "Refusing unavailable topic message for purple id %d\n",
                    purpleChatId);
                return -1;
            }
        }

        int ret = transmitMessage(
            target, message, m_transceiver, m_data,
            &PurpleTdClient::sendMessageResponse);
        if (ret < 0)
            return ret;
        return 0;
    }

    return -1;
}

void PurpleTdClient::joinChatByInviteLink(const char *inviteLink)
{
    auto     request   = td::td_api::make_object<td::td_api::joinChatByInviteLink>(inviteLink);
    uint64_t requestId = m_transceiver.sendQuery(std::move(request), &PurpleTdClient::joinChatResponse);
    m_data.addPendingRequest<GroupJoinRequest>(requestId, inviteLink, GroupJoinRequest::Type::InviteLink);
}

void PurpleTdClient::joinChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<GroupJoinRequest> request = m_data.getPendingRequest<GroupJoinRequest>(requestId);
    if (object && ((object->get_id() == td::td_api::chat::ID) || (object->get_id() == td::td_api::ok::ID))) {
        // If the chat was added with something like "Add chat" function from Pidgin, the chat in
        // contact list was created without id component (for if there was the id component,
        // tgprpl_chat_join would not have called PurpleTdClient::joinChatByLink).

        // So when updateNewChat came prior to this response (as it must have), a new chat with
        // correct id component (but without invite link component) was added to the contact list
        // by PurpleTdClient::addChat calling updateBasicGroupChat, or whatever happens for
        // supergroups.

        // Therefore, remove the original manually added chat, and keep the auto-added one.
        // Furthermore, user could have added same chat like that multiple times, in which case
        // remove all of them.
        if (request) {
            if (!request->joinString.empty()) {
                std::vector<PurpleChat *> obsoleteChats = findChatsByJoinString(request->joinString);
                for (PurpleChat *chat: obsoleteChats)
                    purple_blist_remove_chat(chat);
            }
            // Conversation window for the chat should be presented. If joining by invite link, it
            // will happen automatically due to messageChatJoinByLink message. If joining a public
            // group, conversation window needs to be created explicitly instead
            if (request->type != GroupJoinRequest::Type::InviteLink) {
                const td::td_api::chat *chat     = m_data.getChat(request->chatId);
                int32_t                 purpleId = m_data.getPurpleChatId(request->chatId);
                if (chat) {
                    const std::shared_ptr<LifetimeState> lifetime =
                        m_lifetime;
                    const ContinuationGuard canContinue =
                        [lifetime]() {
                            return lifetime->alive;
                        };
                    getChatConversation(
                        m_data, *chat, purpleId,
                        canContinue);
                }
            }
        }
    } else {
        // TRANSLATOR: Error dialog, content
        std::string message = formatMessage(_("Failed to join chat: {}"), getDisplayedError(object));
        // TRANSLATOR: Error dialog, title
        purple_notify_error(purple_account_get_connection(m_account), _("Failed to join chat"),
                            message.c_str(), NULL);
    }
}

void PurpleTdClient::joinChatByGroupName(const char *joinString, const char *groupName)
{
    auto     request   = td::td_api::make_object<td::td_api::searchPublicChat>(groupName);
    uint64_t requestId = m_transceiver.sendQuery(std::move(request), &PurpleTdClient::joinGroupSearchChatResponse);
    m_data.addPendingRequest<GroupJoinRequest>(requestId, joinString, GroupJoinRequest::Type::Username);
}

void PurpleTdClient::joinGroupSearchChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<GroupJoinRequest> request = m_data.getPendingRequest<GroupJoinRequest>(requestId);
    if (object && (object->get_id() == td::td_api::chat::ID)) {
        const td::td_api::chat &chat = static_cast<const td::td_api::chat &>(*object);
        if (chat.type_ && ((chat.type_->get_id() == td::td_api::chatTypeBasicGroup::ID) ||
                           (chat.type_->get_id() == td::td_api::chatTypeSupergroup::ID))) {
            auto     joinRequest = td::td_api::make_object<td::td_api::joinChat>(chat.id_);
            uint64_t requestId   = m_transceiver.sendQuery(std::move(joinRequest), &PurpleTdClient::joinChatResponse);
            m_data.addPendingRequest<GroupJoinRequest>(requestId, request ? request->joinString : std::string(),
                                                       GroupJoinRequest::Type::Username, getId(chat));
        } else
            // TRANSLATOR: Error dialog, title
            purple_notify_error(purple_account_get_connection(m_account), _("Failed to join chat"),
                                // TRANSLATOR: Error dialog, content
                                _("The name belongs to a user, not a group"), NULL);
    } else {
        // TRANSLATOR: Error dialog, content, argument is a reason (text)
        std::string message = formatMessage(_("Could not find group: {}"), getDisplayedError(object));
        // TRANSLATOR: Error dialog, title
        purple_notify_error(purple_account_get_connection(m_account), _("Failed to join chat"),
                            message.c_str(), NULL);
    }
}

void PurpleTdClient::createGroup(const char *name, int type,
                                 const std::vector<std::string> &basicGroupMembers)
{
    td::td_api::object_ptr<td::td_api::Function> request;
    if (type == GROUP_TYPE_BASIC) {
        auto createRequest = td::td_api::make_object<td::td_api::createNewBasicGroupChat>();
        createRequest->title_ = name;

        std::string errorMessage;
        if (basicGroupMembers.empty()) {
            // TRANSLATOR: Error dialog, secondary content
            errorMessage = _("Cannot create basic group without additional members");
        }
        for (const std::string &memberName: basicGroupMembers) {
            UserId userId = purpleBuddyNameToUserId(memberName.c_str());
            if (userId.valid()) {
                if (!m_data.getUser(userId)) {
                    errorMessage = formatMessage(_("No known user with id {}"), userId);
                }
            } else {
                std::vector<const td::td_api::user*> users;
                m_data.getUsersByDisplayName(memberName.c_str(), users);
                if (users.size() == 1)
                    userId = getId(*users[0]);
                else if (users.empty()) {
                    // TRANSLATOR: Error dialog, secondary content, argument is a username
                    errorMessage = formatMessage(_("No known user by the name '{}'"), memberName);
                } else {
                    // Unlikely error message not worth translating
                    errorMessage = formatMessage("More than one user known with name '{}'", memberName);
                }
            }
            if (!errorMessage.empty())
                break;
            createRequest->user_ids_.push_back(userId.value());
        }

        if (!errorMessage.empty())
            purple_notify_error(purple_account_get_connection(m_account),
                                // TRANSLATOR: Error dialog, title
                                _("Failed to create group"),
                                // TRANSLATOR: Error dialog, primary content
                                _("Invalid group members"),
                                errorMessage.c_str());
        else
            request = std::move(createRequest);
    } else if ((type == GROUP_TYPE_SUPER) || (type == GROUP_TYPE_CHANNEL)) {
        auto createRequest = td::td_api::make_object<td::td_api::createNewSupergroupChat>();
        createRequest->title_ = name;
        createRequest->is_channel_ = (type == GROUP_TYPE_CHANNEL);
        request = std::move(createRequest);
    }

    if (request) {
        // Same as for joining by invite link
        std::vector<PurpleChat *> obsoleteChats = findChatsByNewGroup(name, type);
        for (PurpleChat *chat: obsoleteChats)
            purple_blist_remove_chat(chat);

        m_transceiver.sendQuery(std::move(request), nullptr);
    }
}

BasicGroupMembership PurpleTdClient::getBasicGroupMembership(const char *purpleChatName)
{
    ChatId                        chatId     = getTdlibChatId(purpleChatName);
    const td::td_api::chat       *chat       = chatId.valid() ? m_data.getChat(chatId) : nullptr;
    BasicGroupId                  groupId    = chat ? getBasicGroupId(*chat) : BasicGroupId::invalid;
    const td::td_api::basicGroup *basicGroup = groupId.valid() ? m_data.getBasicGroup(groupId) : nullptr;

    if (basicGroup) {
        if (basicGroup->status_ && (basicGroup->status_->get_id() == td::td_api::chatMemberStatusCreator::ID))
            return BasicGroupMembership::Creator;
        else
            return BasicGroupMembership::NonCreator;
    }
    return BasicGroupMembership::Invalid;
}

void PurpleTdClient::leaveGroup(const std::string &purpleChatName, bool deleteSupergroup)
{
    ChatId                  chatId = getTdlibChatId(purpleChatName.c_str());
    const td::td_api::chat *chat   = chatId.valid() ? m_data.getChat(chatId) : nullptr;
    if (!chat) return;

    SupergroupId supergroupId = getSupergroupId(*chat);
    if (deleteSupergroup && supergroupId.valid()) {
        m_transceiver.sendQuery(td::td_api::make_object<td::td_api::deleteChat>(supergroupId.value()),
                                &PurpleTdClient::deleteSupergroupResponse);
    } else {
        m_transceiver.sendQuery(td::td_api::make_object<td::td_api::leaveChat>(chatId.value()), nullptr);
        auto deleteChatRequest = td::td_api::make_object<td::td_api::deleteChatHistory>();
        deleteChatRequest->chat_id_ = chatId.value();
        deleteChatRequest->remove_from_chat_list_ = true;
        deleteChatRequest->revoke_ = false;
        m_transceiver.sendQuery(std::move(deleteChatRequest), nullptr);
    }
}

void PurpleTdClient::deleteSupergroupResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (!object || (object->get_id() != td::td_api::ok::ID)) {
        std::string errorMessage = getDisplayedError(object).c_str();
        purple_notify_error(m_account,
                            // TRANSLATOR: Error dialog, title
                            _("Failed to delete group or channel"),
                            errorMessage.c_str(), NULL);
    }
}

void PurpleTdClient::setGroupDescription(int purpleChatId, const char *description)
{
    const ChatTarget target =
        m_data.getChatTargetByPurpleId(purpleChatId);
    if (isChildForumTopic(target) ||
        hasChildForumConversation(m_account, purpleChatId)) {
        purple_debug_warning(
            config::pluginId,
            "Refusing to change a parent description from topic purple id %d\n",
            purpleChatId);
        return;
    }

    const td::td_api::chat *chat =
        target.valid() ? m_data.getChat(target.chatId()) : nullptr;
    if (!chat) {
        purple_debug_warning(config::pluginId, "Unknown libpurple chat id %d\n", purpleChatId);
        return;
    }

    if (getBasicGroupId(*chat).valid() || getSupergroupId(*chat).valid()) {
        auto request = td::td_api::make_object<td::td_api::setChatDescription>();
        request->chat_id_ = chat->id_;
        request->description_ =  description ? description : "";
        m_transceiver.sendQuery(std::move(request), &PurpleTdClient::setGroupDescriptionResponse);
    }
}

void PurpleTdClient::setGroupDescriptionResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (!object || (object->get_id() != td::td_api::ok::ID)) {
        std::string message = getDisplayedError(object);
        purple_notify_error(m_account,
                            // TRANSLATOR: Error dialog, title
                            _("Failed to set group description"),
                            message.c_str(), NULL);
    }
}

void PurpleTdClient::kickUserFromChat(PurpleConversation *conv, const char *name)
{
    int purpleChatId = purple_conv_chat_get_id(PURPLE_CONV_CHAT(conv));
    const ChatTarget target =
        m_data.getChatTargetByPurpleId(purpleChatId);
    if (isChildForumTopic(target) ||
        hasChildForumConversation(m_account, purpleChatId)) {
        purple_conversation_write(
            conv, "",
            _("Group administration is unavailable from a topic room"),
            PURPLE_MESSAGE_NO_LOG, time(NULL));
        return;
    }

    const td::td_api::chat *chat =
        target.valid() ? m_data.getChat(target.chatId()) : nullptr;

    if (!chat) {
        // Unlikely error message not worth translating
        purple_conversation_write(conv, "", "Chat not found", PURPLE_MESSAGE_NO_LOG, time(NULL));
        return;
    }

    std::vector<const td::td_api::user *> users = getUsersByPurpleName(name, m_data, "kick user");
    if (users.size() != 1) {
        // TRANSLATOR: In-chat error message, appears after a colon (':')
        const char *reason = users.empty() ? _("User not found") :
                                             // Unlikely error message not worth translating
                                             "More than one user found with this name";
        // TRANSLATOR: In-chat error message, argument is a reason (text)
        std::string message = formatMessage(_("Cannot kick user: {}"), std::string(reason));
        purple_conversation_write(conv, "", message.c_str(), PURPLE_MESSAGE_NO_LOG, 0);
        return;
    }

    auto setStatusRequest = td::td_api::make_object<td::td_api::setChatMemberStatus>();
    setStatusRequest->chat_id_ = chat->id_;
    setStatusRequest->member_id_ = td::td_api::make_object<td::td_api::messageSenderUser>(users[0]->id_);
    setStatusRequest->status_ = td::td_api::make_object<td::td_api::chatMemberStatusLeft>();

    uint64_t requestId = m_transceiver.sendQuery(std::move(setStatusRequest), &PurpleTdClient::chatActionResponse);
    m_data.addPendingRequest<ChatActionRequest>(requestId, ChatActionRequest::Type::Kick, getId(*chat));
}

void PurpleTdClient::chatActionResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<ChatActionRequest> request = m_data.getPendingRequest<ChatActionRequest>(requestId);
    if (!request) return;

    int32_t expectedId = 0;
    switch (request->type) {
        case ChatActionRequest::Type::Kick:
        case ChatActionRequest::Type::Invite:
            expectedId = td::td_api::ok::ID;
            break;
        case ChatActionRequest::Type::GenerateInviteLink:
            expectedId = td::td_api::chatInviteLink::ID;
            break;
    }

    if (!object || (object->get_id() != expectedId)) {
        const td::td_api::chat *chat = request ? m_data.getChat(request->chatId) : nullptr;
        if (chat) {
            std::string message = getDisplayedError(object);
            switch (request->type) {
                case ChatActionRequest::Type::Kick:
                    // TRANSLATOR: In-chat error message, argument is a reason (text)
                    message = formatMessage(_("Cannot kick user: {}"), message);
                    break;
                case ChatActionRequest::Type::Invite:
                    // TRANSLATOR: In-chat error message, argument is a reason (text)
                    message = formatMessage(_("Cannot add user to group: {}"), message);
                    break;
                case ChatActionRequest::Type::GenerateInviteLink:
                    // TRANSLATOR: In-chat error message, argument is a reason (text)
                    message = formatMessage(_("Cannot generate invite link: {}"), message);
                    break;
            }
            showChatNotification(m_data, *chat, message.c_str());
        }
    } else {
        if (request->type == ChatActionRequest::Type::GenerateInviteLink) {
            const td::td_api::chatInviteLink &inviteLink = static_cast<const td::td_api::chatInviteLink &>(*object);
            const td::td_api::chat *chat = request ? m_data.getChat(request->chatId) : nullptr;
            if (chat)
                showChatNotification(m_data, *chat, inviteLink.invite_link_.c_str());
        }
    }
}

void PurpleTdClient::addUserToChat(int purpleChatId, const char *name)
{
    const ChatTarget target =
        m_data.getChatTargetByPurpleId(purpleChatId);
    if (isChildForumTopic(target) ||
        hasChildForumConversation(m_account, purpleChatId)) {
        purple_debug_warning(
            config::pluginId,
            "Refusing parent administration from topic purple id %d\n",
            purpleChatId);
        return;
    }

    const td::td_api::chat *chat =
        target.valid() ? m_data.getChat(target.chatId()) : nullptr;
    if (!chat) {
        purple_debug_warning(config::pluginId, "Unknown libpurple chat id %d\n", purpleChatId);
        return;
    }

    std::vector<const td::td_api::user *> users = getUsersByPurpleName(name, m_data, "kick user");
    if (users.size() != 1) {
        // TRANSLATOR: In-chat error message, appears after a colon (':')
        const char *reason = users.empty() ? _("User not found") :
                                             // Unlikely error message not worth translating
                                             "More than one user found with this name";
        // TRANSLATOR: In-chat error message, argument is a reason (text)
        std::string message = formatMessage(_("Cannot add user to group: {}"), std::string(reason));
        showChatNotification(m_data, *chat, message.c_str(), PURPLE_MESSAGE_NO_LOG);
        return;
    }

    if (getBasicGroupId(*chat).valid() || getSupergroupId(*chat).valid()) {
        auto request = td::td_api::make_object<td::td_api::addChatMember>();
        request->chat_id_ = chat->id_;
        request->user_id_ = users[0]->id_;
        uint64_t requestId = m_transceiver.sendQuery(std::move(request), &PurpleTdClient::chatActionResponse);
        m_data.addPendingRequest<ChatActionRequest>(requestId, ChatActionRequest::Type::Invite, getId(*chat));
    }
}

void PurpleTdClient::showInviteLink(const std::string& purpleChatName)
{
    ChatId                  chatId = getTdlibChatId(purpleChatName.c_str());
    const td::td_api::chat *chat   = chatId.valid() ? m_data.getChat(chatId) : nullptr;
    if (!chat) {
        purple_debug_warning(config::pluginId, "chat %s not found\n", purpleChatName.c_str());
        return;
    }
    BasicGroupId basicGroupId = getBasicGroupId(*chat);
    SupergroupId supergroupId = getSupergroupId(*chat);
    const td::td_api::basicGroupFullInfo *basicGroupInfo = basicGroupId.valid() ? m_data.getBasicGroupInfo(basicGroupId) : nullptr;
    const td::td_api::supergroupFullInfo *supergroupInfo = supergroupId.valid() ? m_data.getSupergroupInfo(supergroupId) : nullptr;
    bool fullInfoKnown = false;
    std::string inviteLink;

    if (basicGroupId.valid()) {
        fullInfoKnown = (basicGroupInfo != nullptr);
        if (basicGroupInfo && basicGroupInfo->invite_link_ && isInviteLinkActive(*basicGroupInfo->invite_link_))
            inviteLink = basicGroupInfo->invite_link_->invite_link_;
    }
    if (supergroupId.valid()) {
        fullInfoKnown = (supergroupInfo != nullptr);
        if (supergroupInfo && supergroupInfo->invite_link_ && isInviteLinkActive(*supergroupInfo->invite_link_))
            inviteLink = supergroupInfo->invite_link_->invite_link_;
    }

    if (!inviteLink.empty())
        showChatNotification(m_data, *chat, inviteLink.c_str());
    else if (fullInfoKnown) {
        auto linkRequest = td::td_api::make_object<td::td_api::createChatInviteLink>();
        linkRequest->chat_id_ = chat->id_;
        uint64_t requestId = m_transceiver.sendQuery(std::move(linkRequest), &PurpleTdClient::chatActionResponse);
        m_data.addPendingRequest<ChatActionRequest>(requestId, ChatActionRequest::Type::GenerateInviteLink, getId(*chat));
    } else
        // Unlikely error message not worth translating
        showChatNotification(m_data, *chat, "Failed to get invite link, full info not known");
}

void PurpleTdClient::getGroupChatList(PurpleRoomlist *roomlist)
{
    m_forumTopics->startRoomList(roomlist);
}

void PurpleTdClient::removeTempFile(
    const std::string &path)
{
    if (!path.empty()) {
        purple_debug_misc(config::pluginId, "Removing temporary file %s\n", path.c_str());
        remove(path.c_str());
    }
}

void PurpleTdClient::setTwoFactorAuth(const char *oldPassword, const char *newPassword,
                                    const char *hint, const char *email)
{
    auto setPassword = td::td_api::make_object<td::td_api::setPassword>();
    if (oldPassword)
        setPassword->old_password_ = oldPassword;
    if (newPassword)
        setPassword->new_password_ = newPassword;
    if (hint)
        setPassword->new_hint_ = hint;
    setPassword->set_recovery_email_address_ = (email && *email);
    if (email)
        setPassword->new_recovery_email_address_ = email;

    m_transceiver.sendQuery(std::move(setPassword), &PurpleTdClient::setTwoFactorAuthResponse);
}

static void inputCancelled(void *data)
{
}

void PurpleTdClient::requestRecoveryEmailConfirmation(const std::string &emailInfo)
{
    // TRANSLATOR: 2FA setup confirmation dialog, secondary content, argument is an e-mail description (address and code length)
    std::string secondary = formatMessage(_("Password will be changed after new e-mail is confirmed\n{}"), emailInfo);
    PurpleConnection *gc = purple_account_get_connection(m_account);
    // TRANSLATOR: 2FA setup confirmation dialog, title
    purple_request_input(gc, _("Two-factor authentication"),
                         // TRANSLATOR: 2FA setup confirmation dialog, primary content
                         _("Enter verification code received in the e-mail"), secondary.c_str(),
                         NULL,  // default value
                         FALSE, // multiline input
                         FALSE, // masked input
                         NULL,
                         // TRANSLATOR: 2FA setup confirmation dialog, alternative is "_Cancel". The underscore marks accelerator keys, they must be different!
                         _("_OK"), G_CALLBACK(PurpleTdClient::verifyRecoveryEmail),
                         // TRANSLATOR: 2FA setup confirmation dialog, alternative is "_OK". The underscore marks accelerator keys, they must be different!
                         _("_Cancel"), G_CALLBACK(inputCancelled),
                         purple_connection_get_account(gc),
                         NULL, // buddy
                         NULL, // conversation
                         this);
}

static void notifyPasswordChangeSuccess(PurpleAccount *account, const td::td_api::passwordState &passwordState)
{
    // TRANSLATOR: 2FA success notification, title
    purple_notify_info(account, _("Two-factor authentication"),
                        // TRANSLATOR: 2FA success notification, primary content
                        passwordState.has_password_ ? _("Password set") :
                                                      // TRANSLATOR: 2FA success notification, primary content
                                                      _("Password cleared"),
                        // TRANSLATOR: 2FA success notification, secondary content
                        passwordState.has_recovery_email_address_ ? _("Recovery e-mail is configured") :
                                                                    // TRANSLATOR: 2FA success notification, secondary content
                                                                    _("No recovery e-mail configured"));
}

void PurpleTdClient::setTwoFactorAuthResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && (object->get_id() == td::td_api::passwordState::ID)) {
        const td::td_api::passwordState &passwordState = static_cast<const td::td_api::passwordState &>(*object);
        if (passwordState.recovery_email_address_code_info_) {
            // TRANSLATOR: 2FA setup confirmation dialog, e-mail description
            std::string emailInfo = formatMessage(_("Code sent to {0} (length: {1})"),
                                                  {passwordState.recovery_email_address_code_info_->email_address_pattern_,
                                                   std::to_string(passwordState.recovery_email_address_code_info_->length_)});
            requestRecoveryEmailConfirmation(emailInfo);
        } else
            notifyPasswordChangeSuccess(m_account, passwordState);
    } else {
        std::string errorMessage = getDisplayedError(object);
        // TRANSLATOR: 2FA failure notification, title
        purple_notify_error(m_account, _("Two-factor authentication"),
                            // TRANSLATOR: 2FA failure notification, primary content
                            _("Failed to set password"), errorMessage.c_str());
    }
}

void PurpleTdClient::verifyRecoveryEmail(PurpleTdClient *self, const char *code)
{
    auto checkCode = td::td_api::make_object<td::td_api::checkRecoveryEmailAddressCode>();
    if (code)
        checkCode->code_ = code;
    self->m_transceiver.sendQuery(std::move(checkCode), &PurpleTdClient::verifyRecoveryEmailResponse);
}

void PurpleTdClient::verifyRecoveryEmailResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && (object->get_id() == td::td_api::passwordState::ID)) {
        const td::td_api::passwordState &passwordState = static_cast<const td::td_api::passwordState &>(*object);
        if (passwordState.recovery_email_address_code_info_) {
            if (passwordState.recovery_email_address_code_info_->length_ > 0) {
                // Not expected to happen, so not worth translating
                std::string emailInfo = formatMessage("E-mail address: {}",
                                                      passwordState.recovery_email_address_code_info_->email_address_pattern_);
                purple_notify_info(m_account, _("Two-factor authentication"),
                                   "For some reason, new confirmation code was sent", emailInfo.c_str());
            } else
                // TRANSLATOR: 2FA failure notification, title
                purple_notify_error(m_account, _("Two-factor authentication"),
                                    // TRANSLATOR: 2FA failure notification, content
                                    _("Looks like the code was wrong"), NULL);
        } else
            notifyPasswordChangeSuccess(m_account, passwordState);
    } else {
        // Shouldn't really happen, so not worth translating. The only reasonable failure is wrong
        // code, which is handled elsewhere.
        std::string errorMessage = getDisplayedError(object);
        purple_notify_error(m_account, "Two-factor authentication",
                            "Failed to verify recovery e-mail", errorMessage.c_str());
    }
}

bool PurpleTdClient::canSendFileToUser(const char *purpleName)
{
    if (!purpleName)
        return false;

    SecretChatId secretChatId = purpleBuddyNameToSecretChatId(purpleName);
    if (secretChatId.valid())
        return m_data.getChatBySecretChat(secretChatId) != nullptr;

    std::vector<const td::td_api::user *> users = getUsersByPurpleName(purpleName, m_data, NULL);
    return users.size() == 1;
}

bool PurpleTdClient::canSendFileToChat(int purpleChatId)
{
    return resolveFileChatTarget(purpleChatId).valid();
}

ChatTarget PurpleTdClient::resolveFileChatTarget(
    int purpleChatId) const
{
    const ChatTarget target =
        m_data.getChatTargetByPurpleId(purpleChatId);
    if (!target.valid() ||
        !hasSafeConversationTargetForSend(
            m_account, purpleChatId, target)) {
        return ChatTarget();
    }

    const td::td_api::chat *chat =
        m_data.getChat(target.chatId());
    if (!chat || !m_data.isGroupChatWithMembership(*chat))
        return ChatTarget();

    if (target.isForumTopic()) {
        if (!isEligibleForumParent(m_data, *chat))
            return ChatTarget();

        if (isChildForumTopic(target)) {
            const TdAccountData::ForumTopicState *topic =
                m_data.findForumTopic(target);
            if (!topic || topic->deleted || !topic->active ||
                m_data.getPurpleChatId(target) != purpleChatId) {
                return ChatTarget();
            }
        }
    }

    return target;
}

void PurpleTdClient::sendFileToChat(PurpleXfer *xfer, const char *purpleName,
                                    PurpleConversationType type, ChatTarget target)
{
    const char *filename = purple_xfer_get_local_filename(xfer);
    const td::td_api::user *privateUser = nullptr;
    const td::td_api::chat *chat        = nullptr;

    if (type == PURPLE_CONV_TYPE_IM) {
        SecretChatId secretChatId = purpleBuddyNameToSecretChatId(purpleName);
        if (secretChatId.valid()) {
            chat = m_data.getChatBySecretChat(secretChatId);
            if (chat)
                target = ChatTarget::chat(getId(*chat));
        } else {
            std::vector<const td::td_api::user *> users = getUsersByPurpleName(purpleName, m_data, "send message");
            if (users.size() == 1) {
                privateUser = users[0];
                chat = m_data.getPrivateChatByUserId(getId(*privateUser));
                if (chat)
                    target = ChatTarget::chat(getId(*chat));
            }
        }
    } else if (type == PURPLE_CONV_TYPE_CHAT) {
        if (target.valid())
            chat = m_data.getChat(target.chatId());
    }

    if (filename && chat)
        startDocumentUpload(
            target, filename, xfer, m_transceiver, m_data,
            &PurpleTdClient::uploadResponse);
    else if (filename && privateUser) {
        purple_debug_misc(config::pluginId, "Requesting private chat for user id %d\n", (int)privateUser->id_);
        td::td_api::object_ptr<td::td_api::createPrivateChat> createChat =
            td::td_api::make_object<td::td_api::createPrivateChat>(privateUser->id_, false);
        uint64_t requestId = m_transceiver.sendQuery(std::move(createChat), &PurpleTdClient::sendMessageCreatePrivateChatResponse);
        purple_xfer_ref(xfer);
        m_data.addPendingRequest<NewPrivateChatForMessage>(requestId, purpleName, xfer);
    } else {
        if (!filename)
            purple_debug_warning(config::pluginId, "Failed to send file, no file name\n");
        else if (!chat)
            purple_debug_warning(config::pluginId, "Failed to send file %s, chat not found\n", filename);
        purple_xfer_cancel_local(xfer);
    }
}

void PurpleTdClient::sendMessageCreatePrivateChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<NewPrivateChatForMessage> request = m_data.getPendingRequest<NewPrivateChatForMessage>(requestId);
    if (!request) return;
    auto chat = (object && (object->get_id() == td::td_api::chat::ID)) ?
                static_cast<const td::td_api::chat *>(object.get()) : nullptr;

    if (request->fileUpload) {
        PurpleXfer *fileUpload = request->fileUpload;
        if (purple_xfer_is_canceled(fileUpload)) {
            // User cancelled the upload really fast
        } else if (chat) {
            const char *filename =
                purple_xfer_get_local_filename(fileUpload);
            if (filename)
                startDocumentUpload(ChatTarget::chat(getId(*chat)), filename, fileUpload, m_transceiver, m_data,
                                    &PurpleTdClient::uploadResponse);
            else
                purple_xfer_cancel_local(fileUpload);
        } else {
            const PurpleXferType type =
                purple_xfer_get_type(fileUpload);
            PurpleAccount *purpleAccount = m_account;
            const std::string username = request->username;
            const std::string message =
                getDisplayedError(object);

            // Notification handlers may synchronously disconnect. Everything
            // below this boundary uses only cached values and the independent
            // request reference.
            purple_xfer_error(
                type, purpleAccount, username.c_str(),
                message.c_str());
            if (fileUpload->data &&
                !purple_xfer_is_canceled(fileUpload)) {
                purple_xfer_cancel_local(fileUpload);
            }
        }

        purple_xfer_unref(fileUpload);
    } else {
        std::string errorMessage;

        if (chat) {
            int ret = transmitMessage(
                ChatTarget::chat(getId(*chat)),
                request->message.c_str(),
                m_transceiver, m_data,
                &PurpleTdClient::sendMessageResponse);
            // Messages copied from libpurple
            if (ret == -E2BIG) {
                // TRANSLATOR: In-chat error message
                errorMessage = _("Unable to send message: The message is too large.");
            } else if (ret < 0) {
                // TRANSLATOR: In-chat error message
                errorMessage = _("Unable to send message.");
            }
        } else {
            // TRANSLATOR: In-chat(?) error message, argument is an error description (text)
            errorMessage = formatMessage(_("Failed to open chat: {}"), getDisplayedError(object));
        }

        if (!errorMessage.empty())
            showMessageTextIm(m_data, request->username.c_str(), NULL, errorMessage.c_str(),
                              time(NULL), PURPLE_MESSAGE_ERROR);
    }
}

void PurpleTdClient::uploadResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<UploadRequest> request = m_data.getPendingRequest<UploadRequest>(requestId);
    const td::td_api::file        *file    = nullptr;

    if (object && (object->get_id() == td::td_api::file::ID))
        file = static_cast<const td::td_api::file *>(object.get());

    if (!request)
        return;

    if (file) {
        if (purple_xfer_is_canceled(request->xfer)) {
            deferOrCancelUpload(file->id_);
            flushDeferredUploadCancels();
            purple_xfer_unref(request->xfer);
            return;
        }

        // A live owner for this TDLib file ID supersedes an earlier deferred
        // cancellation from another waiter.
        m_deferredUploadCancels.erase(file->id_);
        flushDeferredUploadCancels();
        startDocumentUploadProgress(
            request->target, request->xfer, *file,
            m_transceiver, m_data,
            &PurpleTdClient::sendMessageResponse);
        return;
    }

    // Resolve all non-Purple bookkeeping before the error notification can
    // synchronously disconnect and destroy this client.
    flushDeferredUploadCancels();
    uploadResponseError(
        request->xfer, getDisplayedError(object),
        m_data);
}

void PurpleTdClient::cancelUpload(PurpleXfer *xfer)
{
    int32_t fileId;
    if (m_data.getFileIdForTransfer(xfer, fileId)) {
        purple_debug_misc(config::pluginId, "Cancelling upload of %s (file id %d)\n",
                          purple_xfer_get_local_filename(xfer), fileId);
        m_data.removeFileTransfer(fileId, xfer);
        if (!m_data.hasFileTransfer(
                fileId, PURPLE_XFER_SEND)) {
            deferOrCancelUpload(fileId);
        }
        purple_xfer_unref(xfer);
    } else {
        // This could mean that response to upload request has not come yet - when it does,
        // uploadResponse will notice that the transfer is cancelled and act accordingly.
        // Or it could just be that the upload got cancelled programmatically due to some error,
        // in which case nothing more should be done.
    }
}

void PurpleTdClient::deferOrCancelUpload(int32_t fileId)
{
    if (m_data.hasFileTransfer(
            fileId, PURPLE_XFER_SEND)) {
        m_deferredUploadCancels.erase(fileId);
        return;
    }

    if (m_data.hasPendingUploadRequests()) {
        // TDLib canonicalizes identical local files to one FileId and exposes
        // one user upload slot for that ID. Any unresolved preliminary reply
        // may therefore become another owner of this same upload.
        m_deferredUploadCancels.insert(fileId);
        return;
    }

    m_deferredUploadCancels.erase(fileId);
    sendUploadCancel(fileId);
}

void PurpleTdClient::flushDeferredUploadCancels()
{
    if (m_data.hasPendingUploadRequests())
        return;

    std::set<int32_t> fileIds;
    fileIds.swap(m_deferredUploadCancels);
    for (int32_t fileId: fileIds) {
        if (!m_data.hasFileTransfer(
                fileId, PURPLE_XFER_SEND)) {
            sendUploadCancel(fileId);
        }
    }
}

void PurpleTdClient::sendUploadCancel(int32_t fileId)
{
    auto cancelRequest =
        td::td_api::make_object<
            td::td_api::cancelPreliminaryUploadFile>(
            fileId);
    m_transceiver.sendQuery(
        std::move(cancelRequest), nullptr);
}

bool PurpleTdClient::startVoiceCall(const char *buddyName)
{
    std::vector<const td::td_api::user *> users = getUsersByPurpleName(buddyName, m_data, "start voice call");
    if (users.size() != 1) {
        // Unlikely error messages not worth translating
        std::string errorMessage;
        if (users.empty())
            errorMessage = "User not found";
        else
            errorMessage = formatMessage("More than one user known with name '{}'", std::string(buddyName));
        showMessageTextIm(m_data, buddyName, NULL, errorMessage.c_str(), time(NULL), PURPLE_MESSAGE_ERROR);
        return false;
    }

    return initiateCall(users.front()->id_, m_data, m_transceiver);
}

bool PurpleTdClient::terminateCall(PurpleConversation *conv)
{
    if (!m_data.hasActiveCall())
        return false;

    discardCurrentCall(m_data, m_transceiver);
    return true;
}

void PurpleTdClient::createSecretChat(const char* buddyName)
{
    std::vector<const td::td_api::user *> users = getUsersByPurpleName(buddyName, m_data, "create secret chat");
    if (users.size() != 1) {
        // Unlikely error messages not worth translating
        const char *reason = users.empty() ? "User not found" :
                                             "More than one user found with this name";
        std::string message = formatMessage("Cannot create secret chat: {}", std::string(reason));
        purple_notify_error(purple_account_get_connection(m_account),
                            // TRANSLATOR: Failure notification, title
                            _("Failed to create secret chat"),
                            message.c_str(), NULL);

        return;
    }

    auto request = td::td_api::make_object<td::td_api::createNewSecretChat>(getId(*users[0]).value());
    m_transceiver.sendQuery(std::move(request), nullptr);
}
