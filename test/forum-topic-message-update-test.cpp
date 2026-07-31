#include "purple-info.h"
#include "supergroup-test.h"

#include <gtest/gtest.h>

using namespace td::td_api;

namespace {

constexpr int32_t TopicId = 42;
constexpr int32_t TopicPurpleId = 2;
constexpr int32_t MessageDate = 12345;
constexpr int64_t FirstMessageId = 10000;
constexpr int64_t SecondMessageId = 10001;
constexpr int64_t GeneralMessageId = 10002;
constexpr int64_t UnknownMessageId = 99999;
const char *const NotificationWho = " ";

class ForumTopicMessageUpdateTest : public SupergroupTest {
protected:
    void loginWithForumSupergroup()
    {
        loginWithSupergroup();
        tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
            groupId, make_object<chatMemberStatusMember>(), 2)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    ChatTarget topicTarget() const
    {
        const std::string chatId = std::to_string(groupChatId);
        return ChatTarget::forumTopic(
            ChatId::fromString(chatId.c_str()),
            ForumTopicId::fromValue(TopicId));
    }

    std::string topicPurpleName() const
    {
        return getPurpleChatName(topicTarget());
    }

    std::string topicDisplayTitle() const
    {
        return groupChatTitle + " / Support";
    }

    void cacheTopic()
    {
        tgl.update(make_object<updateForumTopicInfo>(
            makeForumTopicInfo(groupChatId, TopicId, "Support")));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    void receiveText(
        int64_t messageId, const std::string &text,
        object_ptr<MessageTopic> topic)
    {
        tgl.update(make_object<updateNewMessage>(makeMessage(
            messageId, userIds[0], groupChatId, false, MessageDate,
            makeTextMessage(text), std::move(topic))));
    }

    void displayChildMessage(
        int64_t messageId, const std::string &text = "original")
    {
        receiveText(
            messageId, text,
            make_object<messageTopicForum>(TopicId));
        tgl.verifyRequest(*Mock_ViewMessages(
            groupChatId, {messageId}, true,
            make_object<messageSourceForumTopicHistory>()));
        tgl.verifyNoRequests();
        prpl.verifyEvents(
            ServGotJoinedChatEvent(
                connection, TopicPurpleId,
                topicPurpleName(), topicPurpleName()),
            ConvSetTitleEvent(
                topicPurpleName(), topicDisplayTitle()),
            ServGotChatEvent(
                connection, TopicPurpleId,
                userFirstNames[0] + " " + userLastNames[0],
                text, PURPLE_MESSAGE_RECV, MessageDate));
    }

    void displayAnotherChildMessage(
        int64_t messageId, const std::string &text)
    {
        receiveText(
            messageId, text,
            make_object<messageTopicForum>(TopicId));
        tgl.verifyRequest(*Mock_ViewMessages(
            groupChatId, {messageId}, true,
            make_object<messageSourceForumTopicHistory>()));
        tgl.verifyNoRequests();
        prpl.verifyEvents(ServGotChatEvent(
            connection, TopicPurpleId,
            userFirstNames[0] + " " + userLastNames[0],
            text, PURPLE_MESSAGE_RECV, MessageDate));
    }

    void displayGeneralMessage(
        int64_t messageId, const std::string &text = "general")
    {
        receiveText(
            messageId, text,
            make_object<messageTopicForum>(
                ForumTopicId::general().value()));
        tgl.verifyRequest(*Mock_ViewMessages(
            groupChatId, {messageId}, true,
            make_object<messageSourceForumTopicHistory>()));
        tgl.verifyNoRequests();
        prpl.verifyEvents(
            ServGotJoinedChatEvent(
                connection, 1,
                groupChatPurpleName, groupChatTitle),
            ChatSetTopicEvent(groupChatPurpleName, "", ""),
            ChatClearUsersEvent(groupChatPurpleName),
            ServGotChatEvent(
                connection, 1,
                userFirstNames[0] + " " + userLastNames[0],
                text, PURPLE_MESSAGE_RECV, MessageDate));
    }

    void displayOrdinaryMessage(
        int64_t messageId, const std::string &text = "ordinary")
    {
        receiveText(messageId, text, nullptr);
        tgl.verifyRequest(*Mock_ViewMessages(
            groupChatId, {messageId}, true));
        tgl.verifyNoRequests();
        prpl.verifyEvents(
            ServGotJoinedChatEvent(
                connection, 1,
                groupChatPurpleName, groupChatTitle),
            ChatSetTopicEvent(groupChatPurpleName, "", ""),
            ChatClearUsersEvent(groupChatPurpleName),
            ServGotChatEvent(
                connection, 1,
                userFirstNames[0] + " " + userLastNames[0],
                text, PURPLE_MESSAGE_RECV, MessageDate));
    }

    void closeConversation(const std::string &name)
    {
        PurpleConversation *conversation =
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT, name.c_str(), account);
        ASSERT_NE(nullptr, conversation);
        purple_conversation_destroy(conversation);
        prpl.verifyNoEvents();
    }

    void updateContent(
        int64_t messageId, const std::string &text = "edited")
    {
        tgl.update(make_object<updateMessageContent>(
            groupChatId, messageId, makeTextMessage(text)));
    }

    void updateNotices(int64_t messageId, bool includeDelete = true)
    {
        tgl.update(make_object<updateMessageIsPinned>(
            groupChatId, messageId, true));
        tgl.update(make_object<updateMessageIsPinned>(
            groupChatId, messageId, false));
        tgl.update(make_object<updateMessageReaction>(
            groupChatId, messageId,
            make_object<messageSenderUser>(userIds[0]),
            MessageDate + 1,
            make_vector<ReactionType>(
                make_object<reactionTypeEmoji>("old")),
            make_vector<ReactionType>(
                make_object<reactionTypeEmoji>("new"))));
        tgl.update(make_object<updateMessageReactions>(
            groupChatId, messageId, MessageDate + 1,
            make_vector<messageReaction>(
                make_object<messageReaction>(
                    make_object<reactionTypeEmoji>("new"),
                    2, false, nullptr,
                    std::vector<object_ptr<MessageSender>>()))));
        if (includeDelete) {
            tgl.update(make_object<updateDeleteMessages>(
                groupChatId, std::vector<int64_t>{messageId},
                true, false));
        }
    }

    void updateAll(int64_t messageId)
    {
        updateContent(messageId);
        updateNotices(messageId);
    }

    void expectMessageUpdates(
        const std::string &conversationName, int64_t messageId)
    {
        const std::string id = std::to_string(messageId);
        prpl.verifyEvents(
            ConversationWriteEvent(
                conversationName,
                "Updated " +
                    userFirstNames[0] + " " + userLastNames[0],
                "edited", PURPLE_MESSAGE_RECV, MessageDate),
            ConversationWriteEvent(
                conversationName, NotificationWho,
                userFirstNames[0] + " " + userLastNames[0] +
                    " changed reactions on message " + id +
                    ": old -> new",
                PURPLE_MESSAGE_NO_LOG, 0),
            ConversationWriteEvent(
                conversationName, NotificationWho,
                "Reactions on message " + id + ": new x2",
                PURPLE_MESSAGE_NO_LOG, 0),
            ConversationWriteEvent(
                conversationName, NotificationWho,
                "Deleted message(s): " + id,
                PURPLE_MESSAGE_SYSTEM, 0));
    }

    void expectNoGeneralConversation() const
    {
        EXPECT_EQ(
            nullptr,
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT,
                groupChatPurpleName.c_str(), account));
    }
};

TEST_F(
    ForumTopicMessageUpdateTest,
    ContentUpdateUsesExactOpenChild)
{
    loginWithForumSupergroup();
    cacheTopic();
    displayChildMessage(FirstMessageId);

    updateContent(FirstMessageId);

    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(),
        "Updated " + userFirstNames[0] + " " + userLastNames[0],
        "edited", PURPLE_MESSAGE_RECV, MessageDate));
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicMessageUpdateTest,
    MessageLinkedUpdatesUseExactOpenChild)
{
    loginWithForumSupergroup();
    cacheTopic();
    displayChildMessage(FirstMessageId);

    updateNotices(FirstMessageId);

    const std::string id = std::to_string(FirstMessageId);
    prpl.verifyEvents(
        ConversationWriteEvent(
            topicPurpleName(), NotificationWho,
            userFirstNames[0] + " " + userLastNames[0] +
                " changed reactions on message " + id +
                ": old -> new",
            PURPLE_MESSAGE_NO_LOG, 0),
        ConversationWriteEvent(
            topicPurpleName(), NotificationWho,
            "Reactions on message " + id + ": new x2",
            PURPLE_MESSAGE_NO_LOG, 0),
        ConversationWriteEvent(
            topicPurpleName(), NotificationWho,
            "Deleted message(s): " + id,
            PURPLE_MESSAGE_SYSTEM, 0));
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicMessageUpdateTest,
    PinStateMetadataDoesNotDuplicateTopicNotices)
{
    loginWithForumSupergroup();
    cacheTopic();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        FirstMessageId, userIds[0], groupChatId, false,
        MessageDate,
        make_object<messageChatChangeTitle>("Renamed"),
        make_object<messageTopicForum>(TopicId))));
    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId, {FirstMessageId}, true,
        make_object<messageSourceForumTopicHistory>()));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, TopicPurpleId,
            topicPurpleName(), topicPurpleName()),
        ConvSetTitleEvent(
            topicPurpleName(), topicDisplayTitle()),
        ConversationWriteEvent(
            topicPurpleName(), NotificationWho,
            userFirstNames[0] + " " + userLastNames[0] +
                " changed group name to Renamed",
            PURPLE_MESSAGE_SYSTEM, MessageDate));

    tgl.update(make_object<updateMessageIsPinned>(
        groupChatId, FirstMessageId, true));
    tgl.update(make_object<updateMessageIsPinned>(
        groupChatId, FirstMessageId, false));

    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicMessageUpdateTest,
    FinalSendIdKeepsExactTopicRoute)
{
    loginWithForumSupergroup();
    cacheTopic();
    displayChildMessage(FirstMessageId);

    tgl.update(make_object<updateMessageSendSucceeded>(
        makeMessage(
            SecondMessageId, userIds[0], groupChatId,
            false, MessageDate, makeTextMessage("original"),
            make_object<messageTopicForum>(TopicId)),
        FirstMessageId));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    updateAll(SecondMessageId);

    expectMessageUpdates(topicPurpleName(), SecondMessageId);
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicMessageUpdateTest,
    FinalSendTargetChangeDoesNotReuseOldTopicRoute)
{
    loginWithForumSupergroup();
    cacheTopic();
    displayChildMessage(FirstMessageId);

    tgl.update(make_object<updateMessageSendSucceeded>(
        makeMessage(
            SecondMessageId, userIds[0], groupChatId,
            false, MessageDate, makeTextMessage("original"),
            make_object<messageTopicForum>(TopicId + 1)),
        FirstMessageId));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    updateAll(SecondMessageId);

    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicMessageUpdateTest,
    FinalSendToGeneralDoesNotReuseChildConversation)
{
    loginWithForumSupergroup();
    cacheTopic();
    displayChildMessage(FirstMessageId);
    displayGeneralMessage(GeneralMessageId);

    tgl.update(make_object<updateMessageSendSucceeded>(
        makeMessage(
            SecondMessageId, userIds[0], groupChatId,
            false, MessageDate, makeTextMessage("original"),
            make_object<messageTopicForum>(
                ForumTopicId::general().value())),
        FirstMessageId));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.update(make_object<updateMessageContent>(
        groupChatId, SecondMessageId,
        makeTextMessage("edited")));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConversationWriteEvent(
        groupChatPurpleName, NotificationWho,
        "Message " + std::to_string(SecondMessageId) +
            " updated: edited",
        PURPLE_MESSAGE_SYSTEM, 0));
    EXPECT_NE(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account));
}

TEST_F(
    ForumTopicMessageUpdateTest,
    ClosedChildUpdatesAreSuppressedWithoutGeneralFallback)
{
    loginWithForumSupergroup();
    cacheTopic();
    displayChildMessage(FirstMessageId);
    closeConversation(topicPurpleName());
    displayGeneralMessage(GeneralMessageId);

    updateAll(FirstMessageId);

    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account));
}

TEST_F(
    ForumTopicMessageUpdateTest,
    GeneralUpdatesUseLegacyRoom)
{
    loginWithForumSupergroup();
    displayGeneralMessage(FirstMessageId);

    updateAll(FirstMessageId);

    expectMessageUpdates(groupChatPurpleName, FirstMessageId);
}

TEST_F(
    ForumTopicMessageUpdateTest,
    OrdinaryGroupUpdatesRemainCompatible)
{
    loginWithSupergroup();
    displayOrdinaryMessage(FirstMessageId);

    updateAll(FirstMessageId);

    expectMessageUpdates(groupChatPurpleName, FirstMessageId);
}

TEST_F(
    ForumTopicMessageUpdateTest,
    ClosedOrdinaryGroupConversationUsesLegacyFallback)
{
    loginWithSupergroup();
    displayOrdinaryMessage(FirstMessageId);
    closeConversation(groupChatPurpleName);

    updateContent(FirstMessageId);

    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 1,
            groupChatPurpleName, groupChatTitle),
        ChatSetTopicEvent(groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ConversationWriteEvent(
            groupChatPurpleName, NotificationWho,
            "Message " + std::to_string(FirstMessageId) +
                " updated: edited",
            PURPLE_MESSAGE_SYSTEM, 0));
}

TEST_F(
    ForumTopicMessageUpdateTest,
    UnknownForumMessageUpdatesDoNotGuessGeneral)
{
    loginWithForumSupergroup();
    displayGeneralMessage(GeneralMessageId);

    updateAll(UnknownMessageId);

    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
}

TEST_F(
    ForumTopicMessageUpdateTest,
    FormerForumUnknownUpdatesStillFailClosed)
{
    loginWithForumSupergroup();

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    updateAll(UnknownMessageId);

    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicMessageUpdateTest,
    UnknownOrdinaryMessageUpdatesKeepLegacyFallback)
{
    loginWithSupergroup();
    displayOrdinaryMessage(FirstMessageId);

    updateAll(UnknownMessageId);

    const std::string id = std::to_string(UnknownMessageId);
    prpl.verifyEvents(
        ConversationWriteEvent(
            groupChatPurpleName, NotificationWho,
            "Message " + id + " updated: edited",
            PURPLE_MESSAGE_SYSTEM, 0),
        ConversationWriteEvent(
            groupChatPurpleName, NotificationWho,
            userFirstNames[0] + " " + userLastNames[0] +
                " changed reactions on message " + id +
                ": old -> new",
            PURPLE_MESSAGE_NO_LOG, 0),
        ConversationWriteEvent(
            groupChatPurpleName, NotificationWho,
            "Reactions on message " + id + ": new x2",
            PURPLE_MESSAGE_NO_LOG, 0),
        ConversationWriteEvent(
            groupChatPurpleName, NotificationWho,
            "Deleted message(s): " + id,
            PURPLE_MESSAGE_SYSTEM, 0));
}

TEST_F(
    ForumTopicMessageUpdateTest,
    MixedOrdinaryDeleteKeepsOneLegacyNoticeInMessageOrder)
{
    loginWithSupergroup();
    displayOrdinaryMessage(FirstMessageId);

    tgl.update(make_object<updateDeleteMessages>(
        groupChatId,
        std::vector<int64_t>{
            UnknownMessageId,
            FirstMessageId,
            SecondMessageId},
        true, false));

    prpl.verifyEvents(ConversationWriteEvent(
        groupChatPurpleName, NotificationWho,
        "Deleted message(s): " +
            std::to_string(UnknownMessageId) + ", " +
            std::to_string(FirstMessageId) + ", " +
            std::to_string(SecondMessageId),
        PURPLE_MESSAGE_SYSTEM, 0));
}

TEST_F(
    ForumTopicMessageUpdateTest,
    DeleteUpdateGroupsMessagesByExactConversation)
{
    loginWithForumSupergroup();
    cacheTopic();
    displayChildMessage(FirstMessageId, "first child");
    displayAnotherChildMessage(
        SecondMessageId, "second child");
    displayGeneralMessage(GeneralMessageId);

    tgl.update(make_object<updateDeleteMessages>(
        groupChatId,
        std::vector<int64_t>{
            FirstMessageId,
            SecondMessageId,
            GeneralMessageId,
            UnknownMessageId},
        true, false));

    prpl.verifyEvents(
        ConversationWriteEvent(
            topicPurpleName(), NotificationWho,
            "Deleted message(s): " +
                std::to_string(FirstMessageId) + ", " +
                std::to_string(SecondMessageId),
            PURPLE_MESSAGE_SYSTEM, 0),
        ConversationWriteEvent(
            groupChatPurpleName, NotificationWho,
            "Deleted message(s): " +
                std::to_string(GeneralMessageId),
            PURPLE_MESSAGE_SYSTEM, 0));
}

TEST_F(
    ForumTopicMessageUpdateTest,
    DeleteStopsAfterSynchronousDisconnect)
{
    loginWithForumSupergroup();
    cacheTopic();
    displayChildMessage(FirstMessageId, "child");
    displayGeneralMessage(GeneralMessageId);

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::ConversationWrite, type);
            pluginInfo().close(connection);
        });
    tgl.update(make_object<updateDeleteMessages>(
        groupChatId,
        std::vector<int64_t>{
            FirstMessageId, GeneralMessageId},
        true, false));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), NotificationWho,
        "Deleted message(s): " +
            std::to_string(FirstMessageId),
        PURPLE_MESSAGE_SYSTEM, 0));
}

} // namespace
