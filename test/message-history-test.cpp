#include "supergroup-test.h"
#include "libpurple-mock.h"
#include "purple-info.h"
#include <td/telegram/td_api.h>
using namespace td::td_api;

class MessageHistoryTest: public SupergroupTest {
protected:
    const std::string userNameInChat = userFirstNames[0] + " " + userLastNames[0];
};

class ForumTopicHistoryTest: public SupergroupTest {
protected:
    const std::string userNameInChat =
        userFirstNames[0] + " " + userLastNames[0];

    void loginWithForumSupergroup()
    {
        loginWithSupergroup();
        tgl.update(make_object<updateSupergroup>(
            makeForumSupergroup(
                groupId,
                make_object<chatMemberStatusMember>(), 2)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
        // Read-receipt behavior has separate coverage. Keep these tests
        // focused on history, readiness, and exact room ordering.
        setUiName("BitlBee");
        purple_account_set_bool(
            account, AccountOptions::ReadReceipts, FALSE);
        ASSERT_FALSE(isReadReceiptsEnabled(account));
    }

    ChatTarget topicTarget(int32_t topicId) const
    {
        const std::string chatIdText =
            std::to_string(groupChatId);
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

    void cacheTopic(
        int32_t topicId, const std::string &topicName)
    {
        tgl.update(make_object<updateForumTopicInfo>(
            makeForumTopicInfo(
                groupChatId, topicId, topicName)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    object_ptr<message> makeForumTextMessage(
        int64_t messageId, const std::string &text,
        int32_t topicId) const
    {
        return makeMessage(
            messageId, userIds[0], groupChatId, false,
            static_cast<int32_t>(messageId),
            makeTextMessage(text),
            make_object<messageTopicForum>(topicId));
    }

    void expectConversation(
        const std::string &purpleName, int32_t purpleId,
        const std::string &title) const
    {
        PurpleConversation *conversation =
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT,
                purpleName.c_str(), account);
        ASSERT_NE(nullptr, conversation);
        ASSERT_NE(
            nullptr,
            purple_conversation_get_chat_data(conversation));
        EXPECT_EQ(
            purpleId,
            purple_conv_chat_get_id(
                purple_conversation_get_chat_data(
                    conversation)));
        EXPECT_STREQ(
            title.c_str(),
            purple_conversation_get_title(conversation));
    }

    void expectNoConversation(
        const std::string &purpleName) const
    {
        EXPECT_EQ(
            nullptr,
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT,
                purpleName.c_str(), account));
    }
};

TEST_F(MessageHistoryTest, TdlibSkipMessages_LastMessageUnknown)
{
    loginWithSupergroup();

    tgl.update(make_object<updateChatLastMessage>(
        groupChatId, nullptr, std::vector<object_ptr<chatPosition>>()
    ));
    tgl.verifyNoRequests();
}

TEST_F(MessageHistoryTest, TdlibSkipMessages)
{
    const int purpleChatId = 1;
    purple_account_set_string(account, ("last-message-chat" + std::to_string(groupChatId)).c_str(), "1");
    loginWithSupergroup();

    tgl.update(make_object<updateChatLastMessage>(
        groupChatId, nullptr, std::vector<object_ptr<chatPosition>>()
    ));

    tgl.update(make_object<updateNewMessage>(
        makeMessage(6, userIds[0], groupChatId, false, 6, makeTextMessage("6"))
    ));
    tgl.verifyRequest(getChatHistory(groupChatId, 6, 0, 30, false));
    tgl.update(make_object<updateChatLastMessage>(
        groupChatId,
        makeMessage(6, userIds[0], groupChatId, false, 6, makeTextMessage("6")),
        std::vector<object_ptr<chatPosition>>()
    ));

    tgl.update(make_object<updateNewMessage>(
        makeMessage(7, userIds[0], groupChatId, false, 7, makeTextMessage("7"))
    ));
    tgl.update(make_object<updateChatLastMessage>(
        groupChatId,
        makeMessage(7, userIds[0], groupChatId, false, 7, makeTextMessage("7")),
        std::vector<object_ptr<chatPosition>>()
    ));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    std::vector<object_ptr<message>> history;
    history.push_back(makeMessage(5, userIds[0], groupChatId, false, 5, makeTextMessage("5")));
    history.push_back(makeMessage(4, userIds[0], groupChatId, false, 4, makeTextMessage("4")));
    tgl.reply(make_object<messages>(history.size(), std::move(history)));
    prpl.verifyNoEvents();
    tgl.verifyRequest(getChatHistory(groupChatId, 4, 0, 30, false));

    history.clear();
    history.push_back(makeMessage(3, userIds[0], groupChatId, false, 3, makeTextMessage("3")));
    history.push_back(makeMessage(2, userIds[0], groupChatId, false, 2, makeTextMessage("2")));
    history.push_back(makeMessage(1, userIds[0], groupChatId, false, 1, makeTextMessage("1")));
    tgl.reply(make_object<messages>(history.size(), std::move(history)));

    prpl.verifyEvents(
        ServGotJoinedChatEvent(connection, purpleChatId, groupChatPurpleName, groupChatTitle),
        ChatSetTopicEvent(groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "2", PURPLE_MESSAGE_RECV, 2),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "3", PURPLE_MESSAGE_RECV, 3),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "4", PURPLE_MESSAGE_RECV, 4),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "5", PURPLE_MESSAGE_RECV, 5),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "6", PURPLE_MESSAGE_RECV, 6),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "7", PURPLE_MESSAGE_RECV, 7)
    );
    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId, {2, 3, 4, 5, 6, 7}, true));

    tgl.update(make_object<updateNewMessage>(
        makeMessage(8, userIds[0], groupChatId, false, 8, makeTextMessage("8"))
    ));
    tgl.update(make_object<updateChatLastMessage>(
        groupChatId,
        makeMessage(8, userIds[0], groupChatId, false, 8, makeTextMessage("8")),
        std::vector<object_ptr<chatPosition>>()
    ));
    prpl.verifyEvents(
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "8", PURPLE_MESSAGE_RECV, 8)
    );
    tgl.verifyRequest(*Mock_ViewMessages(groupChatId, {8}, true));

    ASSERT_EQ(std::string("8"), std::string(purple_account_get_string(
        account, ("last-message-chat" + std::to_string(groupChatId)).c_str(), "")));
}

TEST_F(MessageHistoryTest, TdlibSkipMessages_FlushAtLogout)
{
    const int purpleChatId = 1;
    purple_account_set_string(account, ("last-message-chat" + std::to_string(groupChatId)).c_str(), "1");
    loginWithSupergroup();

    tgl.update(make_object<updateChatLastMessage>(
        groupChatId, nullptr, std::vector<object_ptr<chatPosition>>()
    ));

    tgl.update(make_object<updateNewMessage>(
        makeMessage(6, userIds[0], groupChatId, false, 6, makeTextMessage("6"))
    ));
    tgl.verifyRequest(getChatHistory(groupChatId, 6, 0, 30, false));
    tgl.update(make_object<updateChatLastMessage>(
        groupChatId,
        makeMessage(6, userIds[0], groupChatId, false, 6, makeTextMessage("6")),
        std::vector<object_ptr<chatPosition>>()
    ));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    std::vector<object_ptr<message>> history;
    history.push_back(makeMessage(5, userIds[0], groupChatId, false, 5, makeTextMessage("5")));
    history.push_back(makeMessage(4, userIds[0], groupChatId, false, 4, makeTextMessage("4")));
    tgl.reply(make_object<messages>(history.size(), std::move(history)));
    prpl.verifyNoEvents();
    tgl.verifyRequest(getChatHistory(groupChatId, 4, 0, 30, false));

    history.clear();
    history.push_back(makeMessage(3, userIds[0], groupChatId, false, 3, makeTextMessage("3")));
    history.push_back(makeMessage(2, userIds[0], groupChatId, false, 2, makeTextMessage("2")));
    tgl.reply(make_object<messages>(history.size(), std::move(history)));
    prpl.verifyNoEvents();
    tgl.verifyRequest(getChatHistory(groupChatId, 2, 0, 30, false));

    pluginInfo().close(connection);
    prpl.verifyEvents(
        ServGotJoinedChatEvent(connection, purpleChatId, groupChatPurpleName, groupChatTitle),
        ChatSetTopicEvent(groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "2", PURPLE_MESSAGE_RECV, 2),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "3", PURPLE_MESSAGE_RECV, 3),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "4", PURPLE_MESSAGE_RECV, 4),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "5", PURPLE_MESSAGE_RECV, 5),
        ServGotChatEvent(connection, purpleChatId, userNameInChat, "6", PURPLE_MESSAGE_RECV, 6)
    );
    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId, {2, 3, 4, 5, 6}, true));

    ASSERT_EQ(std::string("6"), std::string(purple_account_get_string(
        account, ("last-message-chat" + std::to_string(groupChatId)).c_str(), "")));
}

TEST_F(
    ForumTopicHistoryTest,
    MixedGapUsesAggregateWatermarkAndRoutesExactlyOnce)
{
    constexpr int32_t FirstTopicId = 42;
    constexpr int32_t SecondTopicId = 43;
    const std::string firstName =
        topicPurpleName(FirstTopicId);
    const std::string secondName =
        topicPurpleName(SecondTopicId);
    const std::string firstTitle =
        topicDisplayTitle(FirstTopicId, "Alpha");
    const std::string secondTitle =
        topicDisplayTitle(SecondTopicId, "Beta");
    const std::string watermarkSetting =
        "last-message-chat" + std::to_string(groupChatId);

    purple_account_set_string(
        account, watermarkSetting.c_str(), "1");
    loginWithForumSupergroup();
    cacheTopic(FirstTopicId, "Alpha");
    cacheTopic(SecondTopicId, "Beta");

    tgl.update(make_object<updateChatLastMessage>(
        groupChatId, nullptr,
        std::vector<object_ptr<chatPosition>>()));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.update(make_object<updateNewMessage>(
        makeForumTextMessage(
            6, "Alpha live", FirstTopicId)));
    tgl.verifyRequest(
        getChatHistory(groupChatId, 6, 0, 30, false));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateChatLastMessage>(
        groupChatId,
        makeForumTextMessage(
            6, "Alpha live", FirstTopicId),
        std::vector<object_ptr<chatPosition>>()));
    tgl.update(make_object<updateNewMessage>(
        makeForumTextMessage(
            7, "General live",
            ForumTopicId::general().value())));
    tgl.update(make_object<updateChatLastMessage>(
        groupChatId,
        makeForumTextMessage(
            7, "General live",
            ForumTopicId::general().value()),
        std::vector<object_ptr<chatPosition>>()));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    std::vector<object_ptr<message>> history;
    history.push_back(makeForumTextMessage(
        5, "Beta history", SecondTopicId));
    history.push_back(makeForumTextMessage(
        4, "General history",
        ForumTopicId::general().value()));
    history.push_back(makeForumTextMessage(
        3, "Alpha history", FirstTopicId));
    history.push_back(makeMessage(
        1, userIds[0], groupChatId, false, 1,
        makeTextMessage("Already displayed")));
    tgl.reply(make_object<messages>(
        history.size(), std::move(history)));

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, firstName, firstName),
        ConvSetTitleEvent(firstName, firstTitle),
        ServGotChatEvent(
            connection, 2, userNameInChat,
            "Alpha history", PURPLE_MESSAGE_RECV, 3),
        ServGotJoinedChatEvent(
            connection, 1, groupChatPurpleName,
            groupChatTitle),
        ChatSetTopicEvent(
            groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ServGotChatEvent(
            connection, 1, userNameInChat,
            "General history", PURPLE_MESSAGE_RECV, 4),
        ServGotJoinedChatEvent(
            connection, 3, secondName, secondName),
        ConvSetTitleEvent(secondName, secondTitle),
        ServGotChatEvent(
            connection, 3, userNameInChat,
            "Beta history", PURPLE_MESSAGE_RECV, 5),
        ServGotChatEvent(
            connection, 2, userNameInChat,
            "Alpha live", PURPLE_MESSAGE_RECV, 6),
        ServGotChatEvent(
            connection, 1, userNameInChat,
            "General live", PURPLE_MESSAGE_RECV, 7));

    expectConversation(firstName, 2, firstTitle);
    expectConversation(
        groupChatPurpleName, 1, groupChatTitle);
    expectConversation(secondName, 3, secondTitle);
    EXPECT_STREQ(
        "7",
        purple_account_get_string(
            account, watermarkSetting.c_str(), ""));
}

TEST_F(
    ForumTopicHistoryTest,
    DelayedMediaPreservesChatOrderAndExactTopicTargets)
{
    constexpr int32_t FirstTopicId = 42;
    constexpr int32_t SecondTopicId = 43;
    constexpr int32_t FileId = 1234;
    const std::string firstName =
        topicPurpleName(FirstTopicId);
    const std::string secondName =
        topicPurpleName(SecondTopicId);
    const std::string firstTitle =
        topicDisplayTitle(FirstTopicId, "Alpha");
    const std::string secondTitle =
        topicDisplayTitle(SecondTopicId, "Beta");

    loginWithForumSupergroup();
    cacheTopic(FirstTopicId, "Alpha");
    cacheTopic(SecondTopicId, "Beta");

    tgl.update(make_object<updateNewMessage>(makeMessage(
        10, userIds[0], groupChatId, false, 10,
        makeMessagePhoto(
            makePhotoRemote(FileId, 10000, 640, 480),
            make_object<formattedText>(
                "Alpha photo",
                std::vector<object_ptr<textEntity>>()),
            false),
        make_object<messageTopicForum>(
            FirstTopicId))));
    const uint64_t downloadRequest = tgl.verifyRequest(
        downloadFile(FileId, 1, 0, 0, true));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateNewMessage>(
        makeForumTextMessage(
            11, "Beta ready", SecondTopicId)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.reply(
        downloadRequest,
        make_object<file>(
            FileId, 10000, 10000,
            make_object<localFile>(
                "/path", true, true, false, true,
                0, 10000, 10000),
            make_object<remoteFile>(
                "beh", "bleh", false, true, 10000)));

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, firstName, firstName),
        ConvSetTitleEvent(firstName, firstTitle),
        ServGotChatEvent(
            connection, 2, userNameInChat,
            "<img src=\"file:///path\">\nAlpha photo",
            static_cast<PurpleMessageFlags>(
                PURPLE_MESSAGE_RECV |
                PURPLE_MESSAGE_IMAGES),
            10),
        ServGotJoinedChatEvent(
            connection, 3, secondName, secondName),
        ConvSetTitleEvent(secondName, secondTitle),
        ServGotChatEvent(
            connection, 3, userNameInChat,
            "Beta ready", PURPLE_MESSAGE_RECV, 11));

    expectConversation(firstName, 2, firstTitle);
    expectConversation(secondName, 3, secondTitle);
    expectNoConversation(groupChatPurpleName);
}

TEST_F(
    ForumTopicHistoryTest,
    LegacyAggregateWatermarkIsNotReinterpretedAsTopicHistory)
{
    constexpr int32_t FirstTopicId = 42;
    constexpr int32_t SecondTopicId = 43;
    const std::string watermarkSetting =
        "last-message-chat" + std::to_string(groupChatId);

    purple_account_set_string(
        account, watermarkSetting.c_str(), "77");
    loginWithForumSupergroup();
    cacheTopic(FirstTopicId, "Alpha");
    cacheTopic(SecondTopicId, "Beta");

    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    EXPECT_STREQ(
        "77",
        purple_account_get_string(
            account, watermarkSetting.c_str(), ""));
    expectNoConversation(groupChatPurpleName);
    expectNoConversation(topicPurpleName(FirstTopicId));
    expectNoConversation(topicPurpleName(SecondTopicId));
}
