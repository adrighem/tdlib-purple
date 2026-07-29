#include "purple-info.h"
#include "supergroup-test.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

using namespace td::td_api;

class ForumTopicReceivingTest : public SupergroupTest {
protected:
    void loginWithForumSupergroup()
    {
        loginWithSupergroup();
        tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
            groupId, make_object<chatMemberStatusMember>(), 2)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    ChatTarget topicTarget(int32_t topicId) const
    {
        const std::string chatIdText = std::to_string(groupChatId);
        return ChatTarget::forumTopic(
            ChatId::fromString(chatIdText.c_str()),
            ForumTopicId::fromValue(topicId));
    }

    std::string topicPurpleName(int32_t topicId) const
    {
        return getPurpleChatName(topicTarget(topicId));
    }

    std::string topicDisplayTitle(
        int32_t topicId, const std::string &topicName) const
    {
        return groupChatTitle + " / " +
               (topicName.empty()
                    ? "Topic " + std::to_string(topicId)
                    : topicName);
    }

    void cacheTopic(int32_t topicId, const std::string &topicName)
    {
        tgl.update(make_object<updateForumTopicInfo>(
            makeForumTopicInfo(groupChatId, topicId, topicName)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    void receiveText(
        int64_t messageId, int32_t date, const std::string &text,
        object_ptr<MessageTopic> topic)
    {
        tgl.update(make_object<updateNewMessage>(makeMessage(
            messageId, userIds[0], groupChatId, false, date,
            makeTextMessage(text), std::move(topic))));
    }

    void verifyForumTopicReadReceipt(int64_t messageId)
    {
        tgl.verifyRequest(*Mock_ViewMessages(
            groupChatId, {messageId}, true,
            make_object<messageSourceForumTopicHistory>()));
    }

    void receiveUnreadReaction(
        int64_t messageId, const std::string &emoji)
    {
        tgl.update(make_object<updateMessageUnreadReactions>(
            groupChatId, messageId,
            make_vector<unreadReaction>(
                make_object<unreadReaction>(
                    make_object<reactionTypeEmoji>(emoji),
                    make_object<messageSenderUser>(userIds[0]),
                    false)),
            1));
    }

    void expectConversation(
        const std::string &purpleName, int32_t purpleId,
        const std::string &title) const
    {
        PurpleConversation *conversation =
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account);
        ASSERT_NE(nullptr, conversation);
        ASSERT_NE(
            nullptr, purple_conversation_get_chat_data(conversation));
        EXPECT_EQ(
            purpleId,
            purple_conv_chat_get_id(
                purple_conversation_get_chat_data(conversation)));
        EXPECT_STREQ(
            title.c_str(), purple_conversation_get_title(conversation));
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

TEST_F(ForumTopicReceivingTest, ChildMessageOpensOnlyItsExactRoom)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t MessageId = 10000;
    constexpr int32_t Date = 12345;
    const std::string purpleName = topicPurpleName(TopicId);
    const std::string displayTitle =
        topicDisplayTitle(TopicId, "Support");

    loginWithForumSupergroup();
    cacheTopic(TopicId, "Support");

    receiveText(
        MessageId, Date, "Exact child",
        make_object<messageTopicForum>(TopicId));

    verifyForumTopicReadReceipt(MessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(purpleName, displayTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Exact child", PURPLE_MESSAGE_RECV, Date));
    expectConversation(purpleName, 2, displayTitle);
    expectNoGeneralConversation();
}

TEST_F(ForumTopicReceivingTest, RemoteOutgoingChildEchoUsesExactRoom)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t MessageId = 10000;
    constexpr int32_t Date = 12345;
    const std::string purpleName = topicPurpleName(TopicId);
    const std::string displayTitle =
        topicDisplayTitle(TopicId, "Support");

    loginWithForumSupergroup();
    cacheTopic(TopicId, "Support");

    object_ptr<message> message = makeMessage(
        MessageId, selfId, groupChatId, true, Date,
        makeTextMessage("Remote echo"),
        make_object<messageTopicForum>(TopicId));
    ASSERT_NE(nullptr, message->sending_state_);
    message->sending_state_ = nullptr;
    tgl.update(make_object<updateNewMessage>(std::move(message)));

    verifyForumTopicReadReceipt(MessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(purpleName, displayTitle),
        ConversationWriteEvent(
            purpleName, selfFirstName + " " + selfLastName,
            "Remote echo",
            static_cast<PurpleMessageFlags>(
                PURPLE_MESSAGE_SEND |
                PURPLE_MESSAGE_REMOTE_SEND),
            Date));
    expectConversation(purpleName, 2, displayTitle);
    expectNoGeneralConversation();
}

TEST_F(ForumTopicReceivingTest, DelayedPhotoRetainsExactTopic)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t MessageId = 10000;
    constexpr int32_t Date = 12345;
    constexpr int32_t FileId = 1234;
    const std::string purpleName = topicPurpleName(TopicId);
    const std::string displayTitle =
        topicDisplayTitle(TopicId, "Support");

    loginWithForumSupergroup();
    cacheTopic(TopicId, "Support");

    tgl.update(make_object<updateNewMessage>(makeMessage(
        MessageId, userIds[0], groupChatId, false, Date,
        makeMessagePhoto(
            makePhotoRemote(FileId, 10000, 640, 480),
            make_object<formattedText>(
                "photo", std::vector<object_ptr<textEntity>>()),
            false),
        make_object<messageTopicForum>(TopicId))));

    tgl.verifyRequest(downloadFile(FileId, 1, 0, 0, true));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.reply(make_object<file>(
        FileId, 10000, 10000,
        make_object<localFile>(
            "/path", true, true, false, true,
            0, 10000, 10000),
        make_object<remoteFile>(
            "beh", "bleh", false, true, 10000)));

    verifyForumTopicReadReceipt(MessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(purpleName, displayTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "<img src=\"file:///path\">\nphoto",
            static_cast<PurpleMessageFlags>(
                PURPLE_MESSAGE_RECV |
                PURPLE_MESSAGE_IMAGES),
            Date));
    expectConversation(purpleName, 2, displayTitle);
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicReceivingTest,
    DelayedPhotoReopensExactChildClosedBeforeCompletion)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t InitialMessageId = 10000;
    constexpr int64_t PhotoMessageId = 10001;
    constexpr int32_t Date = 12345;
    constexpr int32_t FileId = 1234;
    const std::string purpleName = topicPurpleName(TopicId);
    const std::string displayTitle =
        topicDisplayTitle(TopicId, "Support");

    loginWithForumSupergroup();
    cacheTopic(TopicId, "Support");

    receiveText(
        InitialMessageId, Date, "Open child",
        make_object<messageTopicForum>(TopicId));
    verifyForumTopicReadReceipt(InitialMessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(purpleName, displayTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Open child", PURPLE_MESSAGE_RECV, Date));

    tgl.update(make_object<updateNewMessage>(makeMessage(
        PhotoMessageId, userIds[0], groupChatId, false, Date + 1,
        makeMessagePhoto(
            makePhotoRemote(FileId, 10000, 640, 480),
            make_object<formattedText>(
                "photo", std::vector<object_ptr<textEntity>>()),
            false),
        make_object<messageTopicForum>(TopicId))));
    tgl.verifyRequest(downloadFile(FileId, 1, 0, 0, true));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account);
    ASSERT_NE(nullptr, conversation);
    purple_conversation_destroy(conversation);
    prpl.verifyNoEvents();
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account));

    tgl.reply(make_object<file>(
        FileId, 10000, 10000,
        make_object<localFile>(
            "/path", true, true, false, true,
            0, 10000, 10000),
        make_object<remoteFile>(
            "beh", "bleh", false, true, 10000)));

    verifyForumTopicReadReceipt(PhotoMessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(purpleName, displayTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "<img src=\"file:///path\">\nphoto",
            static_cast<PurpleMessageFlags>(
                PURPLE_MESSAGE_RECV |
                PURPLE_MESSAGE_IMAGES),
            Date + 1));
    expectConversation(purpleName, 2, displayTitle);
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicReceivingTest,
    HistoricalChildDoesNotReviveTombstonedTopic)
{
    constexpr int32_t TopicId = 42;
    const std::string purpleName = topicPurpleName(TopicId);

    purple_account_set_string(
        account,
        ("last-message-chat" + std::to_string(groupChatId)).c_str(),
        "1");
    loginWithForumSupergroup();
    cacheTopic(TopicId, "Removed");

    receiveText(
        2, 2, "Before removal",
        make_object<messageTopicForum>(TopicId));
    verifyForumTopicReadReceipt(2);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(
            purpleName,
            topicDisplayTitle(TopicId, "Removed")),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Before removal", PURPLE_MESSAGE_RECV, 2));

    PurpleConversation *child =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account);
    ASSERT_NE(nullptr, child);
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(child));

    PurpleRoomlist *roomlist =
        pluginInfo().roomlist_get_list(connection);
    ASSERT_NE(nullptr, roomlist);
    prpl.verifyEvents(
        RoomlistInProgressEvent(roomlist, TRUE),
        RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle,
            nullptr,
            std::vector<std::string>{groupChatPurpleName}));
    const uint64_t listRequest = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 0, 0, 0, 100));
    tgl.reply(
        listRequest,
        makeForumTopicsPage(
            0, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(
        RoomlistInProgressEvent(roomlist, FALSE));
    purple_roomlist_unref(roomlist);
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(child)));

    tgl.update(make_object<updateChatLastMessage>(
        groupChatId, nullptr,
        std::vector<object_ptr<chatPosition>>()));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    receiveText(
        6, 6, "Live General",
        make_object<messageTopicForum>(
            ForumTopicId::general().value()));
    tgl.verifyRequest(
        getChatHistory(groupChatId, 6, 0, 30, false));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    std::vector<object_ptr<message>> history;
    history.push_back(makeMessage(
        5, userIds[0], groupChatId, false, 5,
        makeTextMessage("Historical child"),
        make_object<messageTopicForum>(TopicId)));
    history.push_back(makeMessage(
        1, userIds[0], groupChatId, false, 1,
        makeTextMessage("Stop")));
    tgl.reply(make_object<messages>(
        history.size(), std::move(history)));

    verifyForumTopicReadReceipt(6);
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
            "Live General", PURPLE_MESSAGE_RECV, 6));
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(child)));
}

TEST_F(
    ForumTopicReceivingTest,
    HistoricalChildDoesNotReopenWhenForumIsDisabled)
{
    constexpr int32_t TopicId = 42;
    const std::string purpleName = topicPurpleName(TopicId);

    purple_account_set_string(
        account,
        ("last-message-chat" + std::to_string(groupChatId)).c_str(),
        "1");
    loginWithForumSupergroup();
    cacheTopic(TopicId, "Suspended");

    receiveText(
        2, 2, "Before disable",
        make_object<messageTopicForum>(TopicId));
    verifyForumTopicReadReceipt(2);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(
            purpleName,
            topicDisplayTitle(TopicId, "Suspended")),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Before disable", PURPLE_MESSAGE_RECV, 2));

    PurpleConversation *child =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account);
    ASSERT_NE(nullptr, child);
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(child));

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(child)));

    tgl.update(make_object<updateChatLastMessage>(
        groupChatId, nullptr,
        std::vector<object_ptr<chatPosition>>()));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    receiveText(6, 6, "Live parent", nullptr);
    tgl.verifyRequest(
        getChatHistory(groupChatId, 6, 0, 30, false));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    std::vector<object_ptr<message>> history;
    history.push_back(makeMessage(
        5, userIds[0], groupChatId, false, 5,
        makeTextMessage("Historical child"),
        make_object<messageTopicForum>(TopicId)));
    history.push_back(makeMessage(
        1, userIds[0], groupChatId, false, 1,
        makeTextMessage("Stop")));
    tgl.reply(make_object<messages>(
        history.size(), std::move(history)));

    tgl.verifyRequest(
        *Mock_ViewMessages(groupChatId, {6}, true));
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
            "Live parent", PURPLE_MESSAGE_RECV, 6));
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(child)));
}

TEST_F(ForumTopicReceivingTest, InterleavedChildTopicsStaySeparated)
{
    constexpr int32_t FirstTopicId = 42;
    constexpr int32_t SecondTopicId = 43;
    const std::string firstName = topicPurpleName(FirstTopicId);
    const std::string secondName = topicPurpleName(SecondTopicId);
    const std::string firstTitle =
        topicDisplayTitle(FirstTopicId, "Alpha");
    const std::string secondTitle =
        topicDisplayTitle(SecondTopicId, "Beta");

    loginWithForumSupergroup();
    cacheTopic(FirstTopicId, "Alpha");
    cacheTopic(SecondTopicId, "Beta");

    receiveText(
        10000, 12345, "Alpha one",
        make_object<messageTopicForum>(FirstTopicId));
    verifyForumTopicReadReceipt(10000);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, firstName, firstName),
        ConvSetTitleEvent(firstName, firstTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Alpha one", PURPLE_MESSAGE_RECV, 12345));

    receiveText(
        10001, 12346, "Beta one",
        make_object<messageTopicForum>(SecondTopicId));
    verifyForumTopicReadReceipt(10001);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 3, secondName, secondName),
        ConvSetTitleEvent(secondName, secondTitle),
        ServGotChatEvent(
            connection, 3,
            userFirstNames[0] + " " + userLastNames[0],
            "Beta one", PURPLE_MESSAGE_RECV, 12346));

    receiveText(
        10002, 12347, "Alpha two",
        make_object<messageTopicForum>(FirstTopicId));
    verifyForumTopicReadReceipt(10002);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotChatEvent(
        connection, 2,
        userFirstNames[0] + " " + userLastNames[0],
        "Alpha two", PURPLE_MESSAGE_RECV, 12347));

    expectConversation(firstName, 2, firstTitle);
    expectConversation(secondName, 3, secondTitle);
    expectNoGeneralConversation();
}

TEST_F(ForumTopicReceivingTest, GeneralTopicKeepsLegacyRoom)
{
    constexpr int64_t MessageId = 10000;
    constexpr int32_t Date = 12345;

    loginWithForumSupergroup();
    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, ForumTopicId::general().value(),
            "General", true)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    PurpleChat *bookmark =
        purple_blist_find_chat(account, groupChatPurpleName.c_str());
    ASSERT_NE(nullptr, bookmark);
    purple_blist_remove_chat(bookmark);
    prpl.discardEvents();

    receiveText(
        MessageId, Date, "General",
        make_object<messageTopicForum>(
            ForumTopicId::general().value()));

    verifyForumTopicReadReceipt(MessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 1, groupChatPurpleName,
            groupChatPurpleName),
        ConvSetTitleEvent(groupChatPurpleName, groupChatTitle),
        ChatSetTopicEvent(groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ServGotChatEvent(
            connection, 1,
            userFirstNames[0] + " " + userLastNames[0],
            "General", PURPLE_MESSAGE_RECV, Date));
    expectConversation(
        groupChatPurpleName, 1, groupChatTitle);
}

TEST_F(ForumTopicReceivingTest, NullAndNonForumTopicsKeepLegacyRouting)
{
    loginWithForumSupergroup();

    receiveText(10000, 12345, "No topic", nullptr);
    tgl.verifyRequest(
        *Mock_ViewMessages(groupChatId, {10000}, true));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 1, groupChatPurpleName, groupChatTitle),
        ChatSetTopicEvent(groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ServGotChatEvent(
            connection, 1,
            userFirstNames[0] + " " + userLastNames[0],
            "No topic", PURPLE_MESSAGE_RECV, 12345));

    receiveText(
        10001, 12346, "Non-forum thread",
        make_object<messageTopicThread>(99));
    tgl.verifyRequest(
        *Mock_ViewMessages(groupChatId, {10001}, true));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotChatEvent(
        connection, 1,
        userFirstNames[0] + " " + userLastNames[0],
        "Non-forum thread", PURPLE_MESSAGE_RECV, 12346));

    expectConversation(
        groupChatPurpleName, 1, groupChatTitle);
}

TEST_F(
    ForumTopicReceivingTest,
    UnknownChildUsesPlaceholderAndCoalescesMetadataLookup)
{
    constexpr int32_t TopicId = 42;
    const std::string purpleName = topicPurpleName(TopicId);
    const std::string placeholderTitle =
        topicDisplayTitle(TopicId, "");

    loginWithForumSupergroup();

    receiveText(
        10000, 12345, "Before metadata",
        make_object<messageTopicForum>(TopicId));
    const uint64_t lookupRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    verifyForumTopicReadReceipt(10000);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(purpleName, placeholderTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Before metadata", PURPLE_MESSAGE_RECV, 12345));

    receiveText(
        10001, 12346, "Still waiting",
        make_object<messageTopicForum>(TopicId));
    verifyForumTopicReadReceipt(10001);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotChatEvent(
        connection, 2,
        userFirstNames[0] + " " + userLastNames[0],
        "Still waiting", PURPLE_MESSAGE_RECV, 12346));
    expectConversation(purpleName, 2, placeholderTitle);
    expectNoGeneralConversation();

    const std::string refinedTitle =
        topicDisplayTitle(TopicId, "Resolved");
    tgl.reply(
        lookupRequest,
        makeForumTopic(makeForumTopicInfo(
            groupChatId, TopicId, "Resolved")));

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ConvSetTitleEvent(purpleName, refinedTitle));
    expectConversation(purpleName, 2, refinedTitle);
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicReceivingTest,
    UnreadReactionUnknownChildUsesExactPlaceholderAndCoalescesMetadata)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t FirstMessageId = 10000;
    constexpr int64_t SecondMessageId = 10001;
    const std::string purpleName = topicPurpleName(TopicId);
    const std::string placeholderTitle =
        topicDisplayTitle(TopicId, "");

    loginWithForumSupergroup();

    receiveUnreadReaction(FirstMessageId, "first");
    const uint64_t firstMessageRequest = tgl.verifyRequest(
        getMessage(groupChatId, FirstMessageId));
    prpl.verifyNoEvents();

    tgl.reply(firstMessageRequest, makeMessage(
        FirstMessageId, selfId, groupChatId, true, 12345,
        makeTextMessage("First message"),
        make_object<messageTopicForum>(TopicId)));

    const uint64_t metadataRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(purpleName, placeholderTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            fmt::format(
                replyPattern,
                selfFirstName + " " + selfLastName,
                "First message", "first"),
            static_cast<PurpleMessageFlags>(
                PURPLE_MESSAGE_RECV |
                PURPLE_MESSAGE_NO_LOG),
            0));
    expectConversation(purpleName, 2, placeholderTitle);
    expectNoGeneralConversation();

    receiveUnreadReaction(SecondMessageId, "second");
    const uint64_t secondMessageRequest = tgl.verifyRequest(
        getMessage(groupChatId, SecondMessageId));
    prpl.verifyNoEvents();

    tgl.reply(secondMessageRequest, makeMessage(
        SecondMessageId, selfId, groupChatId, true, 12346,
        makeTextMessage("Second message"),
        make_object<messageTopicForum>(TopicId)));

    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotChatEvent(
        connection, 2,
        userFirstNames[0] + " " + userLastNames[0],
        fmt::format(
            replyPattern,
            selfFirstName + " " + selfLastName,
            "Second message", "second"),
        static_cast<PurpleMessageFlags>(
            PURPLE_MESSAGE_RECV |
            PURPLE_MESSAGE_NO_LOG),
        0));
    expectConversation(purpleName, 2, placeholderTitle);
    expectNoGeneralConversation();

    const std::string refinedTitle =
        topicDisplayTitle(TopicId, "Resolved");
    tgl.reply(
        metadataRequest,
        makeForumTopic(makeForumTopicInfo(
            groupChatId, TopicId, "Resolved")));

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ConvSetTitleEvent(purpleName, refinedTitle));
    expectConversation(purpleName, 2, refinedTitle);
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicReceivingTest,
    UnreadReactionGetMessageFailureDoesNotGuessGeneral)
{
    constexpr int64_t MessageId = 10000;

    loginWithForumSupergroup();

    receiveUnreadReaction(MessageId, "failed");
    const uint64_t messageRequest = tgl.verifyRequest(
        getMessage(groupChatId, MessageId));
    prpl.verifyNoEvents();

    tgl.reply(
        messageRequest,
        make_object<error>(404, "Message not found"));

    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicReceivingTest,
    UnreadReactionGetMessageTimeoutDoesNotGuessGeneral)
{
    constexpr int64_t MessageId = 10000;

    loginWithForumSupergroup();

    receiveUnreadReaction(MessageId, "timeout");
    const uint64_t messageRequest = tgl.verifyRequest(
        getMessage(groupChatId, MessageId));
    prpl.verifyNoEvents();

    tgl.runTimeouts();

    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    expectNoGeneralConversation();

    tgl.reply(messageRequest, makeMessage(
        MessageId, selfId, groupChatId, true, 12345,
        makeTextMessage("Late General"),
        make_object<messageTopicForum>(
            ForumTopicId::general().value())));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicReceivingTest,
    CallbacklessMetadataLookupRefetchesAfterForumRestore)
{
    constexpr int32_t TopicId = 42;
    const std::string purpleName = topicPurpleName(TopicId);
    const std::string placeholderTitle =
        topicDisplayTitle(TopicId, "");

    loginWithForumSupergroup();

    receiveText(
        10000, 12345, "Before disable",
        make_object<messageTopicForum>(TopicId));
    const uint64_t oldRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    verifyForumTopicReadReceipt(10000);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(purpleName, placeholderTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Before disable", PURPLE_MESSAGE_RECV, 12345));

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account);
    ASSERT_NE(nullptr, conversation);
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    receiveText(
        10001, 12346, "After restore",
        make_object<messageTopicForum>(TopicId));
    const uint64_t freshRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    EXPECT_NE(oldRequest, freshRequest);
    verifyForumTopicReadReceipt(10001);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, placeholderTitle),
        ConvSetTitleEvent(purpleName, placeholderTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "After restore", PURPLE_MESSAGE_RECV, 12346));
    expectConversation(purpleName, 2, placeholderTitle);
    expectNoGeneralConversation();

    tgl.reply(
        oldRequest,
        makeForumTopic(makeForumTopicInfo(
            groupChatId, TopicId, "Stale")));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    expectConversation(purpleName, 2, placeholderTitle);

    const std::string refinedTitle =
        topicDisplayTitle(TopicId, "Fresh");
    tgl.reply(
        freshRequest,
        makeForumTopic(makeForumTopicInfo(
            groupChatId, TopicId, "Fresh")));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ConvSetTitleEvent(purpleName, refinedTitle));
    expectConversation(purpleName, 2, refinedTitle);
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicReceivingTest,
    ClientDestructionDuringReadyChildBatchStopsEmission)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t InitialMessageId = 10000;
    constexpr int64_t FirstBatchMessageId = 10001;
    constexpr int64_t SecondBatchMessageId = 10002;
    constexpr int64_t ReplyMessageId = 9999;
    const std::string purpleName = topicPurpleName(TopicId);
    const std::string displayTitle =
        topicDisplayTitle(TopicId, "Support");

    loginWithForumSupergroup();
    cacheTopic(TopicId, "Support");

    receiveText(
        InitialMessageId, 12345, "Open child",
        make_object<messageTopicForum>(TopicId));
    verifyForumTopicReadReceipt(InitialMessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(purpleName, displayTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Open child", PURPLE_MESSAGE_RECV, 12345));

    object_ptr<message> firstMessage = makeMessage(
        FirstBatchMessageId, userIds[0], groupChatId, false, 12346,
        makeTextMessage("First batch message"),
        make_object<messageTopicForum>(TopicId));
    firstMessage->reply_to_ =
        makeMessageReplyTo(groupChatId, ReplyMessageId);
    tgl.update(make_object<updateNewMessage>(
        std::move(firstMessage)));
    const uint64_t replyRequest = tgl.verifyRequest(
        getMessage(groupChatId, ReplyMessageId));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    receiveText(
        SecondBatchMessageId, 12347, "Second batch message",
        make_object<messageTopicForum>(TopicId));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::ServGotChat, type);
            pluginInfo().close(connection);
        });
    tgl.reply(replyRequest, makeMessage(
        ReplyMessageId, userIds[0], groupChatId, false, 12344,
        makeTextMessage("Original"),
        make_object<messageTopicForum>(TopicId)));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotChatEvent(
        connection, 2,
        userFirstNames[0] + " " + userLastNames[0],
        fmt::format(
            replyPattern,
            userFirstNames[0] + " " + userLastNames[0],
            "Original", "First batch message"),
        PURPLE_MESSAGE_RECV, 12346));
}

TEST_F(
    ForumTopicReceivingTest,
    DelayedPhotoDoesNotDisplayAfterForumDisable)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t MessageId = 10000;
    constexpr int32_t FileId = 1234;
    const std::string purpleName = topicPurpleName(TopicId);

    loginWithForumSupergroup();
    cacheTopic(TopicId, "Support");

    tgl.update(make_object<updateNewMessage>(makeMessage(
        MessageId, userIds[0], groupChatId, false, 12345,
        makeMessagePhoto(
            makePhotoRemote(FileId, 10000, 640, 480),
            make_object<formattedText>(
                "photo", std::vector<object_ptr<textEntity>>()),
            false),
        make_object<messageTopicForum>(TopicId))));
    tgl.verifyRequest(downloadFile(FileId, 1, 0, 0, true));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.reply(make_object<file>(
        FileId, 10000, 10000,
        make_object<localFile>(
            "/path", true, true, false, true,
            0, 10000, 10000),
        make_object<remoteFile>(
            "beh", "bleh", false, true, 10000)));

    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account));
    expectNoGeneralConversation();
}
