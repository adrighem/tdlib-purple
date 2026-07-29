#include "purple-info.h"
#include "supergroup-test.h"
#include "td-client.h"

#include <fmt/format.h>
#include <gtest/gtest.h>

using namespace td::td_api;

static gboolean readReceiptConversationFocused(
    PurpleConversation *)
{
    return TRUE;
}

static gboolean readReceiptConversationUnfocused(
    PurpleConversation *)
{
    return FALSE;
}

static PurpleConnection *readReceiptClosingConnection = nullptr;
static PurplePluginProtocolInfo *readReceiptClosingPlugin = nullptr;

static gboolean closeConnectionWhileCheckingFocus(
    PurpleConversation *)
{
    PurpleConnection *connection =
        readReceiptClosingConnection;
    PurplePluginProtocolInfo *plugin =
        readReceiptClosingPlugin;
    readReceiptClosingConnection = nullptr;
    readReceiptClosingPlugin = nullptr;
    if (connection && plugin)
        plugin->close(connection);
    return TRUE;
}

static PurpleConversationUiOps *readReceiptUiOps(bool focused)
{
    static PurpleConversationUiOps focusedOps = {};
    static PurpleConversationUiOps unfocusedOps = {};
    focusedOps.has_focus = readReceiptConversationFocused;
    unfocusedOps.has_focus = readReceiptConversationUnfocused;
    return focused ? &focusedOps : &unfocusedOps;
}

static PurpleConversationUiOps *closingReadReceiptUiOps()
{
    static PurpleConversationUiOps closingOps = {};
    closingOps.has_focus =
        closeConnectionWhileCheckingFocus;
    return &closingOps;
}

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

    void receiveForumTopicServiceMessage(
        int64_t messageId, int32_t date,
        object_ptr<MessageContent> content, int32_t topicId)
    {
        tgl.update(make_object<updateNewMessage>(makeMessage(
            messageId, userIds[0], groupChatId, false, date,
            std::move(content),
            make_object<messageTopicForum>(topicId))));
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

    PurpleConversation *findRoom(
        const std::string &purpleName) const
    {
        return purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account);
    }

    void makeNextJoinedRoomUnfocused(
        const std::string &purpleName)
    {
        prpl.onNextEvent(
            [this, purpleName](PurpleEventType type) {
                EXPECT_EQ(
                    PurpleEventType::ServGotJoinedChat, type);
                PurpleConversation *conversation =
                    findRoom(purpleName);
                ASSERT_NE(nullptr, conversation);
                conversation->ui_ops =
                    readReceiptUiOps(false);
            });
    }

    void setRoomFocused(
        const std::string &purpleName, bool focused)
    {
        PurpleConversation *conversation = findRoom(purpleName);
        ASSERT_NE(nullptr, conversation);
        conversation->ui_ops =
            readReceiptUiOps(focused);
    }

    void sendRoomReadReceipts(
        const std::string &purpleName)
    {
        PurpleConversation *conversation = findRoom(purpleName);
        ASSERT_NE(nullptr, conversation);
        PurpleTdClient *client = getTdClient(account);
        ASSERT_NE(nullptr, client);
        client->sendReadReceipts(conversation);
    }

    std::string senderNotice(const std::string &notice) const
    {
        return userFirstNames[0] + " " + userLastNames[0] +
               ": " + notice;
    }

    void openChildTopic(
        int32_t topicId, const std::string &topicName,
        int64_t messageId = 9000)
    {
        const std::string purpleName = topicPurpleName(topicId);
        cacheTopic(topicId, topicName);
        receiveText(
            messageId, 9000, "Open child",
            make_object<messageTopicForum>(topicId));
        verifyForumTopicReadReceipt(messageId);
        tgl.verifyNoRequests();
        prpl.verifyEvents(
            ServGotJoinedChatEvent(
                connection, 2, purpleName, purpleName),
            ConvSetTitleEvent(
                purpleName,
                topicDisplayTitle(topicId, topicName)),
            ServGotChatEvent(
                connection, 2,
                userFirstNames[0] + " " + userLastNames[0],
                "Open child", PURPLE_MESSAGE_RECV, 9000));
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

TEST_F(
    ForumTopicReceivingTest,
    TopicCreationWritesOneEscapedNoticeInExactChild)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t MessageId = 10000;
    constexpr int32_t Date = 12345;
    const std::string topicName = "Ops <b>&";
    const std::string purpleName = topicPurpleName(TopicId);

    loginWithForumSupergroup();
    cacheTopic(TopicId, topicName);

    receiveForumTopicServiceMessage(
        MessageId, Date,
        make_object<messageForumTopicCreated>(
            topicName, false,
            make_object<forumTopicIcon>(0, 0)),
        TopicId);

    verifyForumTopicReadReceipt(MessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(
            purpleName,
            topicDisplayTitle(TopicId, topicName)),
        ConversationWriteEvent(
            purpleName, " ",
            senderNotice(
                "Created topic Ops &lt;b&gt;&amp;"),
            PURPLE_MESSAGE_SYSTEM, Date));
    expectConversation(
        purpleName, 2,
        topicDisplayTitle(TopicId, topicName));
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicReceivingTest,
    TopicEditsWriteOneNoticeAfterMetadataProjection)
{
    constexpr int32_t TopicId = 42;
    const std::string purpleName = topicPurpleName(TopicId);

    loginWithForumSupergroup();
    openChildTopic(TopicId, "Old name");

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, TopicId, "Renamed")));
    tgl.verifyNoRequests();

    receiveForumTopicServiceMessage(
        10000, 12345,
        make_object<messageForumTopicEdited>(
            "Renamed", true, 123),
        TopicId);
    verifyForumTopicReadReceipt(10000);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ConvSetTitleEvent(
            purpleName,
            topicDisplayTitle(TopicId, "Renamed")),
        ConversationWriteEvent(
            purpleName, " ",
            senderNotice(
                "Renamed the topic to Renamed and changed its icon"),
            PURPLE_MESSAGE_SYSTEM, 12345));

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, TopicId, "Renamed")));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    receiveForumTopicServiceMessage(
        10001, 12346,
        make_object<messageForumTopicEdited>(
            "", true, 456),
        TopicId);
    verifyForumTopicReadReceipt(10001);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConversationWriteEvent(
        purpleName, " ",
        senderNotice("Changed the topic icon"),
        PURPLE_MESSAGE_SYSTEM, 12346));
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicReceivingTest,
    TopicCloseAndReopenWriteOneNoticeEach)
{
    constexpr int32_t TopicId = 42;
    const std::string purpleName = topicPurpleName(TopicId);

    loginWithForumSupergroup();
    openChildTopic(TopicId, "Support");

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, TopicId, "Support",
            false, true)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    receiveForumTopicServiceMessage(
        10000, 12345,
        make_object<messageForumTopicIsClosedToggled>(true),
        TopicId);
    verifyForumTopicReadReceipt(10000);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConversationWriteEvent(
        purpleName, " ",
        senderNotice("Closed the topic"),
        PURPLE_MESSAGE_SYSTEM, 12345));

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, TopicId, "Support")));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    receiveForumTopicServiceMessage(
        10001, 12346,
        make_object<messageForumTopicIsClosedToggled>(false),
        TopicId);
    verifyForumTopicReadReceipt(10001);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConversationWriteEvent(
        purpleName, " ",
        senderNotice("Reopened the topic"),
        PURPLE_MESSAGE_SYSTEM, 12346));
    expectNoGeneralConversation();
}

TEST_F(
    ForumTopicReceivingTest,
    GeneralHideAndUnhideWriteOneNoticeInLegacyRoom)
{
    const int32_t GeneralId =
        ForumTopicId::general().value();

    loginWithForumSupergroup();

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, GeneralId, "General",
            true, true, true)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    receiveForumTopicServiceMessage(
        10000, 12345,
        make_object<messageForumTopicIsHiddenToggled>(true),
        GeneralId);
    verifyForumTopicReadReceipt(10000);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 1, groupChatPurpleName,
            groupChatTitle),
        ChatSetTopicEvent(groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ConversationWriteEvent(
            groupChatPurpleName, " ",
            senderNotice("Hid the General topic"),
            PURPLE_MESSAGE_SYSTEM, 12345));

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, GeneralId, "General",
            true, true, false)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    receiveForumTopicServiceMessage(
        10001, 12346,
        make_object<messageForumTopicIsHiddenToggled>(false),
        GeneralId);
    verifyForumTopicReadReceipt(10001);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConversationWriteEvent(
        groupChatPurpleName, " ",
        senderNotice("Unhid the General topic"),
        PURPLE_MESSAGE_SYSTEM, 12346));
    expectConversation(
        groupChatPurpleName, 1, groupChatTitle);
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
    MultiStagePhotoQueuesOneTopicReadReceipt)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t MessageId = 10000;
    constexpr int32_t Date = 12345;
    constexpr int32_t FileId = 1234;
    const std::string purpleName = topicPurpleName(TopicId);
    const std::string displayTitle =
        topicDisplayTitle(TopicId, "Support");

    purple_account_set_string(
        account, "download-behaviour", "file-transfer");
    loginWithForumSupergroup();
    cacheTopic(TopicId, "Support");

    tgl.update(make_object<updateNewMessage>(makeMessage(
        MessageId, userIds[0], groupChatId, false, Date,
        makeMessagePhoto(
            makePhotoRemote(FileId, 10000, 640, 480),
            make_object<formattedText>(
                "photo",
                std::vector<object_ptr<textEntity>>()),
            false),
        make_object<messageTopicForum>(TopicId))));
    const uint64_t downloadRequest = tgl.verifyRequest(
        downloadFile(FileId, 1, 0, 0, true));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.update(make_object<updateFile>(make_object<file>(
        FileId, 10000, 10000,
        make_object<localFile>(
            "/path", true, true, true, false,
            0, 0, 2000),
        make_object<remoteFile>(
            "beh", "bleh", false, true, 10000))));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.runTimeouts();
    verifyForumTopicReadReceipt(MessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(purpleName, displayTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "photo", PURPLE_MESSAGE_RECV, Date),
        ConversationWriteEvent(
            purpleName, " ",
            userFirstNames[0] + " " +
                userLastNames[0] +
                ": Downloading photo",
            PURPLE_MESSAGE_SYSTEM, Date));

    tgl.update(make_object<updateFile>(make_object<file>(
        FileId, 10000, 10000,
        make_object<localFile>(
            "/path", true, true, true, false,
            0, 0, 5000),
        make_object<remoteFile>(
            "beh", "bleh", false, true, 10000))));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.reply(downloadRequest, make_object<file>(
        FileId, 10000, 10000,
        make_object<localFile>(
            "/path", true, true, false, true,
            0, 10000, 10000),
        make_object<remoteFile>(
            "beh", "bleh", false, true, 10000)));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotChatEvent(
        connection, 2,
        userFirstNames[0] + " " + userLastNames[0],
        "<img src=\"file:///path\">",
        static_cast<PurpleMessageFlags>(
            PURPLE_MESSAGE_RECV |
            PURPLE_MESSAGE_IMAGES),
        Date));
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

TEST_F(
    ForumTopicReceivingTest,
    FocusCallbackCanDisconnectBeforeReceiptRouting)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t MessageId = 10000;
    const std::string purpleName =
        topicPurpleName(TopicId);

    loginWithForumSupergroup();
    cacheTopic(TopicId, "Support");

    prpl.onNextEvent(
        [this, purpleName](PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::ServGotJoinedChat, type);
            PurpleConversation *conversation =
                findRoom(purpleName);
            ASSERT_NE(nullptr, conversation);
            readReceiptClosingConnection = connection;
            readReceiptClosingPlugin = &pluginInfo();
            conversation->ui_ops =
                closingReadReceiptUiOps();
        });
    receiveText(
        MessageId, 12345, "Disconnect safely",
        make_object<messageTopicForum>(TopicId));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    EXPECT_EQ(nullptr, readReceiptClosingConnection);
    EXPECT_EQ(nullptr, readReceiptClosingPlugin);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(
            purpleName,
            topicDisplayTitle(TopicId, "Support")),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Disconnect safely",
            PURPLE_MESSAGE_RECV, 12345));
}

TEST_F(
    ForumTopicReceivingTest,
    FocusingOneChildDrainsOnlyThatTopicsReadReceipts)
{
    constexpr int32_t FirstTopicId = 42;
    constexpr int32_t SecondTopicId = 43;
    constexpr int64_t FirstMessageId = 10000;
    constexpr int64_t SecondMessageId = 10001;
    const std::string firstName =
        topicPurpleName(FirstTopicId);
    const std::string secondName =
        topicPurpleName(SecondTopicId);

    loginWithForumSupergroup();
    cacheTopic(FirstTopicId, "Alpha");
    cacheTopic(SecondTopicId, "Beta");

    makeNextJoinedRoomUnfocused(firstName);
    receiveText(
        FirstMessageId, 12345, "Alpha",
        make_object<messageTopicForum>(FirstTopicId));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, firstName, firstName),
        ConvSetTitleEvent(
            firstName,
            topicDisplayTitle(FirstTopicId, "Alpha")),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Alpha", PURPLE_MESSAGE_RECV, 12345));

    makeNextJoinedRoomUnfocused(secondName);
    receiveText(
        SecondMessageId, 12346, "Beta",
        make_object<messageTopicForum>(SecondTopicId));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 3, secondName, secondName),
        ConvSetTitleEvent(
            secondName,
            topicDisplayTitle(SecondTopicId, "Beta")),
        ServGotChatEvent(
            connection, 3,
            userFirstNames[0] + " " + userLastNames[0],
            "Beta", PURPLE_MESSAGE_RECV, 12346));

    setRoomFocused(firstName, true);
    sendRoomReadReceipts(firstName);
    verifyForumTopicReadReceipt(FirstMessageId);
    tgl.verifyNoRequests();

    setRoomFocused(secondName, true);
    sendRoomReadReceipts(secondName);
    verifyForumTopicReadReceipt(SecondMessageId);
    tgl.verifyNoRequests();
}

TEST_F(
    ForumTopicReceivingTest,
    GeneralAndLegacyReceiptsRemainSeparate)
{
    constexpr int64_t GeneralMessageId = 10000;
    constexpr int64_t LegacyMessageId = 10001;

    loginWithForumSupergroup();
    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId,
            ForumTopicId::general().value(),
            "General", true)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    makeNextJoinedRoomUnfocused(groupChatPurpleName);
    receiveText(
        GeneralMessageId, 12345, "General",
        make_object<messageTopicForum>(
            ForumTopicId::general().value()));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 1, groupChatPurpleName,
            groupChatTitle),
        ChatSetTopicEvent(groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ServGotChatEvent(
            connection, 1,
            userFirstNames[0] + " " + userLastNames[0],
            "General", PURPLE_MESSAGE_RECV, 12345));

    receiveText(
        LegacyMessageId, 12346, "Legacy", nullptr);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotChatEvent(
        connection, 1,
        userFirstNames[0] + " " + userLastNames[0],
        "Legacy", PURPLE_MESSAGE_RECV, 12346));

    setRoomFocused(groupChatPurpleName, true);
    sendRoomReadReceipts(groupChatPurpleName);
    tgl.verifyRequestsV(
        Mock_ViewMessages(
            groupChatId, {GeneralMessageId}, true,
            make_object<messageSourceForumTopicHistory>()),
        Mock_ViewMessages(
            groupChatId, {LegacyMessageId}, true));
}

TEST_F(
    ForumTopicReceivingTest,
    ParentNameWithChildIdFailsClosedButGeneralIdIsCompatible)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t GeneralMessageId = 10000;
    constexpr int64_t ChildMessageId = 10001;
    const std::string childName = topicPurpleName(TopicId);

    loginWithForumSupergroup();
    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId,
            ForumTopicId::general().value(),
            "General", true)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    cacheTopic(TopicId, "Support");

    makeNextJoinedRoomUnfocused(groupChatPurpleName);
    receiveText(
        GeneralMessageId, 12345, "Pending General",
        make_object<messageTopicForum>(
            ForumTopicId::general().value()));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 1, groupChatPurpleName,
            groupChatTitle),
        ChatSetTopicEvent(groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ServGotChatEvent(
            connection, 1,
            userFirstNames[0] + " " + userLastNames[0],
            "Pending General", PURPLE_MESSAGE_RECV,
            12345));

    receiveText(
        ChildMessageId, 12346, "Child",
        make_object<messageTopicForum>(TopicId));
    verifyForumTopicReadReceipt(ChildMessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, childName, childName),
        ConvSetTitleEvent(
            childName,
            topicDisplayTitle(TopicId, "Support")),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Child", PURPLE_MESSAGE_RECV, 12346));

    PurpleConversation *parent =
        findRoom(groupChatPurpleName);
    PurpleConversation *child = findRoom(childName);
    ASSERT_NE(nullptr, parent);
    ASSERT_NE(nullptr, child);
    PurpleConvChat *parentChat =
        purple_conversation_get_chat_data(parent);
    PurpleConvChat *childChat =
        purple_conversation_get_chat_data(child);
    ASSERT_NE(nullptr, parentChat);
    ASSERT_NE(nullptr, childChat);
    const int parentId = purple_conv_chat_get_id(parentChat);
    parentChat->id = purple_conv_chat_get_id(childChat);

    setRoomFocused(groupChatPurpleName, true);
    sendRoomReadReceipts(groupChatPurpleName);
    tgl.verifyNoRequests();

    parentChat->id = parentId;
    sendRoomReadReceipts(groupChatPurpleName);
    verifyForumTopicReadReceipt(GeneralMessageId);
    tgl.verifyNoRequests();
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
    NonForumParentKeepsLegacyReadReceipt)
{
    constexpr int64_t MessageId = 10000;

    loginWithSupergroup();

    receiveText(
        MessageId, 12345, "Legacy parent", nullptr);
    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId, {MessageId}, true));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 1, groupChatPurpleName,
            groupChatTitle),
        ChatSetTopicEvent(groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ServGotChatEvent(
            connection, 1,
            userFirstNames[0] + " " + userLastNames[0],
            "Legacy parent", PURPLE_MESSAGE_RECV,
            12345));
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
    DroppedDelayedPhotoIsNeverFlushedByLaterDisplay)
{
    constexpr int32_t TopicId = 42;
    constexpr int64_t MessageId = 10000;
    constexpr int64_t LaterMessageId = 10001;
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

    tgl.update(make_object<updateSupergroup>(
        makeForumSupergroup(
            groupId,
            make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    receiveText(
        LaterMessageId, 12346, "Displayed later",
        make_object<messageTopicForum>(TopicId));
    const uint64_t metadataRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    verifyForumTopicReadReceipt(LaterMessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, purpleName),
        ConvSetTitleEvent(
            purpleName, topicDisplayTitle(TopicId, "")),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Displayed later", PURPLE_MESSAGE_RECV,
            12346));

    tgl.reply(
        metadataRequest,
        makeForumTopic(makeForumTopicInfo(
            groupChatId, TopicId, "Support")));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConvSetTitleEvent(
        purpleName, topicDisplayTitle(TopicId, "Support")));
}
