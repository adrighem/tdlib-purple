#include "purple-info.h"
#include "supergroup-test.h"

#include <gtest/gtest.h>

using namespace td::td_api;

class ForumTopicJoinTest: public SupergroupTest {
protected:
    static constexpr int32_t TopicId = 42;

    void loginWithForumSupergroup()
    {
        loginWithSupergroup();
        tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
            groupId, make_object<chatMemberStatusMember>(), 2)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    ChatTarget topicTarget(int32_t topicId = TopicId) const
    {
        const std::string chatIdText = std::to_string(groupChatId);
        return ChatTarget::forumTopic(
            ChatId::fromString(chatIdText.c_str()),
            ForumTopicId::fromValue(topicId));
    }

    std::string topicPurpleName(int32_t topicId = TopicId) const
    {
        return getPurpleChatName(topicTarget(topicId));
    }

    std::string topicDisplayName(const std::string &topicName) const
    {
        return groupChatTitle + " / " + topicName;
    }

    void cacheTopic(const std::string &topicName,
                    int32_t topicId = TopicId)
    {
        tgl.update(make_object<updateForumTopicInfo>(
            makeForumTopicInfo(groupChatId, topicId, topicName)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    void joinTopic(int32_t topicId = TopicId)
    {
        GHashTable *components = getChatComponents(topicTarget(topicId));
        pluginInfo().join_chat(connection, components);
        g_hash_table_destroy(components);
    }

    PurpleRoomlist *startRoomList(uint64_t &requestId)
    {
        PurpleRoomlist *roomlist =
            pluginInfo().roomlist_get_list(connection);
        EXPECT_NE(nullptr, roomlist);
        if (!roomlist) {
            requestId = 0;
            return nullptr;
        }

        prpl.verifyEvents(
            RoomlistInProgressEvent(roomlist, TRUE),
            RoomlistAddRoomEvent(
                roomlist,
                PURPLE_ROOMLIST_ROOMTYPE_ROOM,
                groupChatTitle,
                nullptr,
                std::vector<std::string>{groupChatPurpleName}
            )
        );
        requestId = tgl.verifyRequest(getForumTopics(
            groupChatId, "", 0, 0, 0, 100));
        return roomlist;
    }

    void receiveTopicText(
        int64_t messageId, int32_t date,
        const std::string &text)
    {
        tgl.update(make_object<updateNewMessage>(makeMessage(
            messageId, userIds[0], groupChatId, false, date,
            makeTextMessage(text),
            make_object<messageTopicForum>(TopicId))));
    }

    void verifyForumTopicReadReceipt(int64_t messageId)
    {
        tgl.verifyRequest(*Mock_ViewMessages(
            groupChatId, {messageId}, true,
            make_object<messageSourceForumTopicHistory>()));
    }

    uint64_t receiveDelayedTopicPhoto(
        int64_t messageId, int32_t date, int32_t fileId)
    {
        tgl.update(make_object<updateNewMessage>(makeMessage(
            messageId, userIds[0], groupChatId, false, date,
            makeMessagePhoto(
                makePhotoRemote(fileId, 10000, 640, 480),
                make_object<formattedText>(
                    "photo",
                    std::vector<object_ptr<textEntity>>()),
                false),
            make_object<messageTopicForum>(TopicId))));
        return tgl.verifyRequest(
            downloadFile(fileId, 1, 0, 0, true));
    }

    void completeTopicPhotoDownload(
        uint64_t requestId, int32_t fileId)
    {
        tgl.reply(requestId, make_object<file>(
            fileId, 10000, 10000,
            make_object<localFile>(
                "/path", true, true, false, true,
                0, 10000, 10000),
            make_object<remoteFile>(
                "beh", "bleh", false, true, 10000)));
    }

    PurpleConversation *addSavedLeftTopic(
        const std::string &displayName, int32_t purpleId = 99)
    {
        const std::string purpleName = topicPurpleName();
        addSavedTopicBookmark(displayName);
        serv_got_joined_chat(
            connection, purpleId, purpleName.c_str());
        PurpleConversation *conversation =
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account);
        EXPECT_NE(nullptr, conversation);
        if (conversation) {
            purple_conv_chat_left(
                purple_conversation_get_chat_data(conversation));
        }
        prpl.discardEvents();
        return conversation;
    }

    PurpleChat *addSavedTopicBookmark(
        const std::string &displayName)
    {
        PurpleChat *bookmark = purple_chat_new(
            account, displayName.c_str(),
            getChatComponents(topicTarget()));
        purple_blist_add_chat(
            bookmark, nullptr, nullptr);
        prpl.discardEvents();
        return bookmark;
    }

    void expectTopicOpened(const std::string &topicName,
                           int32_t topicId = TopicId,
                           int32_t purpleId = 2)
    {
        const std::string purpleName = topicPurpleName(topicId);
        prpl.verifyEvents(
            ServGotJoinedChatEvent(
                connection, purpleId, purpleName, purpleName),
            ConvSetTitleEvent(purpleName, topicDisplayName(topicName)),
            PresentConversationEvent(purpleName)
        );

        PurpleConversation *conversation =
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account);
        ASSERT_NE(nullptr, conversation);
        ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));
        EXPECT_EQ(
            purpleId,
            purple_conv_chat_get_id(
                purple_conversation_get_chat_data(conversation)));
        EXPECT_STREQ(
            topicDisplayName(topicName).c_str(),
            purple_conversation_get_title(conversation));
    }

    void expectNoTopicOrGeneralConversation(
        int32_t topicId = TopicId) const
    {
        const std::string purpleName = topicPurpleName(topicId);
        EXPECT_EQ(
            nullptr,
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account));
        EXPECT_EQ(
            nullptr,
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT,
                groupChatPurpleName.c_str(), account));
    }
};

TEST_F(ForumTopicJoinTest, CachedTopicJoinsExactConversation)
{
    loginWithForumSupergroup();
    cacheTopic("Cached");

    joinTopic();

    tgl.verifyNoRequests();
    expectTopicOpened("Cached");
    EXPECT_EQ(
        nullptr,
        purple_blist_find_chat(account, topicPurpleName().c_str()));
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            groupChatPurpleName.c_str(), account));
}

TEST_F(
    ForumTopicJoinTest,
    CachedParentDescriptionAndMembersHydrateNewChild)
{
    auto fullInfo = make_object<supergroupFullInfo>();
    fullInfo->description_ = "Shared parent description";

    auto members = make_object<chatMembers>();
    members->members_.push_back(makeChatMember(
        userIds[1], userIds[1], 0,
        make_object<chatMemberStatusCreator>("", true),
        nullptr));

    auto administrators = make_object<chatMembers>();
    administrators->members_.push_back(makeChatMember(
        userIds[0], userIds[1], 0,
        make_object<chatMemberStatusAdministrator>(),
        nullptr));

    loginWithSupergroup(
        std::move(fullInfo), std::move(members),
        std::move(administrators));
    tgl.update(make_object<updateSupergroup>(
        makeForumSupergroup(
            groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    cacheTopic("Hydrated");

    joinTopic();

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, topicPurpleName(),
            topicPurpleName()),
        ConvSetTitleEvent(
            topicPurpleName(),
            topicDisplayName("Hydrated")),
        ChatSetTopicEvent(
            topicPurpleName(),
            "Shared parent description", ""),
        ChatClearUsersEvent(topicPurpleName()),
        ChatAddUserEvent(
            topicPurpleName(),
            userFirstNames[1] + " " + userLastNames[1],
            "", PURPLE_CBFLAGS_FOUNDER, false),
        ChatAddUserEvent(
            topicPurpleName(),
            userFirstNames[0] + " " + userLastNames[0],
            "", PURPLE_CBFLAGS_OP, false),
        PresentConversationEvent(topicPurpleName()));
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            groupChatPurpleName.c_str(), account));
}

TEST_F(ForumTopicJoinTest, UnknownTopicFetchesExactMetadataAndJoinsTransiently)
{
    loginWithForumSupergroup();

    joinTopic();

    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Fetched")));

    expectTopicOpened("Fetched");
    EXPECT_EQ(
        nullptr,
        purple_blist_find_chat(account, topicPurpleName().c_str()));
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            groupChatPurpleName.c_str(), account));
}

TEST_F(ForumTopicJoinTest, DuplicatePendingJoinsCoalesceAndPresentOnce)
{
    loginWithForumSupergroup();

    joinTopic();
    joinTopic();

    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Coalesced")));

    expectTopicOpened("Coalesced");
    tgl.verifyNoRequests();
}

TEST_F(ForumTopicJoinTest, LiveMetadataWinsDelayedLookupResponse)
{
    loginWithForumSupergroup();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, TopicId, "Current")));
    expectTopicOpened("Current");

    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Stale")));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    EXPECT_STREQ(
        topicDisplayName("Current").c_str(),
        purple_conversation_get_title(conversation));
}

TEST_F(ForumTopicJoinTest, RoomListMetadataCompletesPendingExactLookup)
{
    loginWithForumSupergroup();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    uint64_t listRequest = 0;
    PurpleRoomlist *roomlist = startRoomList(listRequest);
    ASSERT_NE(nullptr, roomlist);

    std::vector<object_ptr<forumTopic>> topics;
    topics.push_back(makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Listed")));
    tgl.reply(listRequest, makeForumTopicsPage(
        1, std::move(topics), 10, 20, TopicId));

    prpl.verifyEvents(
        RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            topicDisplayName("Listed"),
            nullptr,
            std::vector<std::string>{topicPurpleName()}
        ),
        ServGotJoinedChatEvent(
            connection, 2, topicPurpleName(), topicPurpleName()),
        ConvSetTitleEvent(
            topicPurpleName(), topicDisplayName("Listed")),
        PresentConversationEvent(topicPurpleName())
    );
    const uint64_t finalListRequest = tgl.verifyRequest(
        getForumTopics(groupChatId, "", 10, 20, TopicId, 100));

    tgl.reply(exactRequest, make_object<error>(
        404, "Exact lookup raced with listing"));
    prpl.verifyNoEvents();

    tgl.reply(finalListRequest, makeForumTopicsPage(
        1, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(
        RoomlistInProgressEvent(roomlist, FALSE));
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicJoinTest, NewerCompleteListingSupersedesExactLookup)
{
    loginWithForumSupergroup();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    uint64_t listRequest = 0;
    PurpleRoomlist *roomlist = startRoomList(listRequest);
    ASSERT_NE(nullptr, roomlist);

    tgl.reply(listRequest, makeForumTopicsPage(
        0, std::vector<object_ptr<forumTopic>>()));

    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()),
        RoomlistInProgressEvent(roomlist, FALSE));
    expectNoTopicOrGeneralConversation();

    tgl.reply(exactRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Stale exact result")));
    prpl.verifyNoEvents();
    expectNoTopicOrGeneralConversation();
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicJoinTest, OlderCompleteListingDoesNotBeatExactLookup)
{
    loginWithForumSupergroup();

    uint64_t listRequest = 0;
    PurpleRoomlist *roomlist = startRoomList(listRequest);
    ASSERT_NE(nullptr, roomlist);

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.reply(listRequest, makeForumTopicsPage(
        0, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(
        RoomlistInProgressEvent(roomlist, FALSE));

    tgl.reply(exactRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Newer exact result")));
    expectTopicOpened("Newer exact result");
    purple_roomlist_unref(roomlist);
}

TEST_F(
    ForumTopicJoinTest,
    LiveChildAfterListingSnapshotSurvivesAndLookupStillRefines)
{
    constexpr int64_t FirstMessageId = 10000;
    constexpr int64_t SecondMessageId = 10001;
    const std::string placeholderTitle =
        groupChatTitle + " / Topic " +
        std::to_string(TopicId);

    loginWithForumSupergroup();

    receiveTopicText(
        FirstMessageId, 12345, "Before listing");
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    verifyForumTopicReadReceipt(FirstMessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, topicPurpleName(),
            topicPurpleName()),
        ConvSetTitleEvent(
            topicPurpleName(), placeholderTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Before listing", PURPLE_MESSAGE_RECV,
            12345));

    uint64_t listRequest = 0;
    PurpleRoomlist *roomlist = startRoomList(listRequest);
    ASSERT_NE(nullptr, roomlist);

    receiveTopicText(
        SecondMessageId, 12346, "After listing started");
    verifyForumTopicReadReceipt(SecondMessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotChatEvent(
        connection, 2,
        userFirstNames[0] + " " + userLastNames[0],
        "After listing started", PURPLE_MESSAGE_RECV,
        12346));

    tgl.reply(listRequest, makeForumTopicsPage(
        0, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(
        RoomlistInProgressEvent(roomlist, FALSE));

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    ASSERT_NE(
        nullptr,
        purple_conversation_get_chat_data(conversation));
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));

    tgl.reply(exactRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Refined")));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConvSetTitleEvent(
        topicPurpleName(), topicDisplayName("Refined")));
    EXPECT_STREQ(
        topicDisplayName("Refined").c_str(),
        purple_conversation_get_title(conversation));
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicJoinTest, OlderListedTopicDoesNotBeatExactLookupError)
{
    loginWithForumSupergroup();

    uint64_t listRequest = 0;
    PurpleRoomlist *roomlist = startRoomList(listRequest);
    ASSERT_NE(nullptr, roomlist);

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    std::vector<object_ptr<forumTopic>> topics;
    topics.push_back(makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Older listing")));
    tgl.reply(listRequest, makeForumTopicsPage(
        1, std::move(topics), 10, 20, TopicId));
    prpl.verifyEvents(RoomlistAddRoomEvent(
        roomlist,
        PURPLE_ROOMLIST_ROOMTYPE_ROOM,
        topicDisplayName("Older listing"),
        nullptr,
        std::vector<std::string>{topicPurpleName()}
    ));
    const uint64_t finalListRequest = tgl.verifyRequest(
        getForumTopics(groupChatId, "", 10, 20, TopicId, 100));

    tgl.reply(exactRequest, make_object<error>(
        404, "Newer exact result"));
    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    expectNoTopicOrGeneralConversation();

    tgl.reply(finalListRequest, makeForumTopicsPage(
        1, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(
        RoomlistInProgressEvent(roomlist, FALSE));
    purple_roomlist_unref(roomlist);

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t retryRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    tgl.reply(retryRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Fresh exact result")));
    expectTopicOpened("Fresh exact result");
}

TEST_F(ForumTopicJoinTest, TopicLookupErrorFailsWithoutGeneralFallback)
{
    loginWithForumSupergroup();

    joinTopic();

    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.reply(requestId, make_object<error>(404, "Topic not found"));

    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    expectNoTopicOrGeneralConversation();
    EXPECT_EQ(
        nullptr,
        purple_blist_find_chat(account, topicPurpleName().c_str()));
}

TEST_F(ForumTopicJoinTest, WrongLookupTargetFailsWithoutGeneralFallback)
{
    loginWithForumSupergroup();

    joinTopic();

    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId + 1, "Wrong")));

    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    expectNoTopicOrGeneralConversation();
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName(TopicId + 1).c_str(), account));
    EXPECT_EQ(
        nullptr,
        purple_blist_find_chat(account, topicPurpleName().c_str()));
}

TEST_F(ForumTopicJoinTest, TopicLookupTimeoutFailsExactJoin)
{
    loginWithForumSupergroup();

    joinTopic();

    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.runTimeouts();

    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    expectNoTopicOrGeneralConversation();

    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Late")));
    prpl.verifyNoEvents();
    expectNoTopicOrGeneralConversation();
}

TEST_F(ForumTopicJoinTest, DisconnectDropsOutstandingExactLookup)
{
    loginWithForumSupergroup();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    pluginInfo().close(connection);
    ASSERT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));

    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Late")));
    prpl.verifyNoEvents();
    expectNoTopicOrGeneralConversation();
}

TEST_F(ForumTopicJoinTest, MembershipLossCancelsPendingLookupOnce)
{
    loginWithForumSupergroup();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusLeft>(), 2)));
    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    tgl.verifyNoRequests();

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyNoEvents();
    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Late")));

    prpl.verifyNoEvents();
    expectNoTopicOrGeneralConversation();
}

TEST_F(ForumTopicJoinTest, RejoinAfterMembershipRestoreUsesFreshLookup)
{
    loginWithForumSupergroup();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t oldRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusLeft>(), 2)));
    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyNoEvents();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t newRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    EXPECT_NE(oldRequest, newRequest);

    tgl.reply(oldRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Stale")));
    prpl.verifyNoEvents();
    expectNoTopicOrGeneralConversation();

    tgl.reply(newRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Fresh")));
    expectTopicOpened("Fresh");
}

TEST_F(ForumTopicJoinTest, ForumDisableCancelsPendingLookupOnce)
{
    loginWithForumSupergroup();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    tgl.verifyNoRequests();

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyNoEvents();
    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Late")));

    prpl.verifyNoEvents();
    expectNoTopicOrGeneralConversation();
}

TEST_F(ForumTopicJoinTest, ChatFirstIneligibleParentClearsExpectedJoin)
{
    login();
    addSavedLeftTopic(topicDisplayName("Saved"));

    joinTopic();
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateNewChat>(makeChat(
        groupChatId,
        make_object<chatTypeSupergroup>(groupId, false),
        groupChatTitle, nullptr, 0, 0, 0)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusLeft>(), 2)));
    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    tgl.verifyNoRequests();
}

TEST_F(ForumTopicJoinTest, GroupFirstIneligibleParentClearsExpectedJoin)
{
    login();
    addSavedLeftTopic(topicDisplayName("Saved"));

    joinTopic();
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusLeft>(), 2)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateNewChat>(makeChat(
        groupChatId,
        make_object<chatTypeSupergroup>(groupId, false),
        groupChatTitle, nullptr, 0, 0, 0)));
    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    tgl.verifyNoRequests();
}

TEST_F(ForumTopicJoinTest, GroupFirstIneligibleParentLeavesActiveChild)
{
    login();
    addSavedTopicBookmark(topicDisplayName("Saved"));
    serv_got_joined_chat(
        connection, 99, topicPurpleName().c_str());
    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));
    prpl.discardEvents();

    joinTopic();
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusLeft>(), 2)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateNewChat>(makeChat(
        groupChatId,
        make_object<chatTypeSupergroup>(groupId, false),
        groupChatTitle, nullptr, 0, 0, 0)));
    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    tgl.verifyNoRequests();
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(ForumTopicJoinTest, KnownBasicGroupRejectsTopicJoin)
{
    login();
    tgl.update(make_object<updateBasicGroup>(make_object<basicGroup>(
        groupId, 2, make_object<chatMemberStatusMember>(), true, 0)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateNewChat>(makeChat(
        groupChatId,
        make_object<chatTypeBasicGroup>(groupId),
        groupChatTitle, nullptr, 0, 0, 0)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    addSavedTopicBookmark(topicDisplayName("Invalid"));
    joinTopic();

    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    tgl.verifyNoRequests();
    expectNoTopicOrGeneralConversation();
}

TEST_F(ForumTopicJoinTest, BasicGroupMetadataFailsDeferredTopicJoin)
{
    login();
    addSavedTopicBookmark(topicDisplayName("Invalid"));

    joinTopic();
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateNewChat>(makeChat(
        groupChatId,
        make_object<chatTypeBasicGroup>(groupId),
        groupChatTitle, nullptr, 0, 0, 0)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateBasicGroup>(make_object<basicGroup>(
        groupId, 2, make_object<chatMemberStatusMember>(), true, 0)));

    prpl.verifyEvents(
        JoinChatFailedEvent(connection, topicPurpleName()));
    tgl.verifyNoRequests();
    expectNoTopicOrGeneralConversation();
}

TEST_F(ForumTopicJoinTest, ClosingPendingAutoRejoinCancelsItSilently)
{
    loginWithForumSupergroup();
    PurpleConversation *conversation =
        addSavedLeftTopic(topicDisplayName("Saved"));
    ASSERT_NE(nullptr, conversation);

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    PurpleChat *bookmark =
        purple_blist_find_chat(account, topicPurpleName().c_str());
    ASSERT_NE(nullptr, bookmark);
    purple_blist_remove_chat(bookmark);
    purple_conversation_destroy(conversation);
    prpl.discardEvents();

    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Must stay closed")));

    prpl.verifyNoEvents();
    expectNoTopicOrGeneralConversation();
}

TEST_F(
    ForumTopicJoinTest,
    LiveChildSatisfiesPendingAutoRejoinBeforeLookupError)
{
    constexpr int64_t MessageId = 10000;
    const std::string savedTitle =
        topicDisplayName("Saved");
    PurpleConversation *conversation = nullptr;

    loginWithForumSupergroup();
    conversation = addSavedLeftTopic(savedTitle);
    ASSERT_NE(nullptr, conversation);

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    receiveTopicText(
        MessageId, 12345, "Live child");
    verifyForumTopicReadReceipt(MessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, topicPurpleName(),
            savedTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Live child", PURPLE_MESSAGE_RECV,
            12345));
    ASSERT_NE(
        nullptr,
        purple_conversation_get_chat_data(conversation));
    ASSERT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));

    tgl.reply(
        exactRequest,
        make_object<error>(404, "Stale lookup failure"));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(
    ForumTopicJoinTest,
    LiveChildOpeningReentrantlyPreventsJoinedThenFailedResult)
{
    constexpr int64_t MessageId = 10000;
    const std::string savedTitle =
        topicDisplayName("Saved");

    loginWithForumSupergroup();
    PurpleConversation *conversation =
        addSavedLeftTopic(savedTitle);
    ASSERT_NE(nullptr, conversation);

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    bool injectedLookupFailure = false;
    prpl.onNextEvent(
        [this, exactRequest, &injectedLookupFailure](
            PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::ServGotJoinedChat, type);
            injectedLookupFailure = true;
            tgl.reply(
                exactRequest,
                make_object<error>(
                    404, "Synchronous stale lookup failure"));
        });

    receiveTopicText(
        MessageId, 12345, "Live child");

    EXPECT_TRUE(injectedLookupFailure);
    verifyForumTopicReadReceipt(MessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, topicPurpleName(),
            savedTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Live child", PURPLE_MESSAGE_RECV,
            12345));
    ASSERT_NE(
        nullptr,
        purple_conversation_get_chat_data(conversation));
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(
    ForumTopicJoinTest,
    HistoricalChildDisplayDoesNotSatisfyPendingExactJoin)
{
    constexpr int64_t HistoricalMessageId = 5;
    constexpr int64_t LiveMessageId = 6;
    const std::string placeholderTitle =
        groupChatTitle + " / Topic " +
        std::to_string(TopicId);

    purple_account_set_string(
        account,
        ("last-message-chat" +
         std::to_string(groupChatId)).c_str(),
        "1");
    loginWithForumSupergroup();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.update(make_object<updateChatLastMessage>(
        groupChatId, nullptr,
        std::vector<object_ptr<chatPosition>>()));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        LiveMessageId, userIds[0], groupChatId, false,
        LiveMessageId, makeTextMessage("Live General"),
        make_object<messageTopicForum>(
            ForumTopicId::general().value()))));
    const uint64_t historyRequest = tgl.verifyRequest(
        getChatHistory(
            groupChatId, LiveMessageId,
            0, 30, false));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    std::vector<object_ptr<message>> history;
    history.push_back(makeMessage(
        HistoricalMessageId, userIds[0], groupChatId,
        false, HistoricalMessageId,
        makeTextMessage("Historical child"),
        make_object<messageTopicForum>(TopicId)));
    history.push_back(makeMessage(
        1, userIds[0], groupChatId, false, 1,
        makeTextMessage("Stop")));
    tgl.reply(historyRequest, make_object<messages>(
        history.size(), std::move(history)));

    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId,
        {HistoricalMessageId}, true,
        make_object<messageSourceForumTopicHistory>()));
    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId,
        {LiveMessageId}, true,
        make_object<messageSourceForumTopicHistory>()));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, topicPurpleName(),
            topicPurpleName()),
        ConvSetTitleEvent(
            topicPurpleName(), placeholderTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Historical child", PURPLE_MESSAGE_RECV,
            HistoricalMessageId),
        ServGotJoinedChatEvent(
            connection, 1, groupChatPurpleName,
            groupChatTitle),
        ChatSetTopicEvent(
            groupChatPurpleName, "", ""),
        ChatClearUsersEvent(groupChatPurpleName),
        ServGotChatEvent(
            connection, 1,
            userFirstNames[0] + " " + userLastNames[0],
            "Live General", PURPLE_MESSAGE_RECV,
            LiveMessageId));

    tgl.reply(
        exactRequest,
        make_object<error>(
            404, "Historical evidence is not a join result"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        JoinChatFailedEvent(
            connection, topicPurpleName()));
}

TEST_F(
    ForumTopicJoinTest,
    HistoricalChildCannotReuseLiveEvidenceFromBeforeJoin)
{
    constexpr int64_t PriorLiveMessageId = 2;
    constexpr int64_t HistoricalMessageId = 5;
    constexpr int64_t LiveGeneralMessageId = 6;

    purple_account_set_string(
        account,
        ("last-message-chat" +
         std::to_string(groupChatId)).c_str(),
        "1");
    loginWithForumSupergroup();
    cacheTopic("Prior");

    receiveTopicText(
        PriorLiveMessageId, PriorLiveMessageId,
        "Prior live child");
    verifyForumTopicReadReceipt(PriorLiveMessageId);
    tgl.verifyNoRequests();
    prpl.discardEvents();

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    ASSERT_NE(
        nullptr,
        purple_conversation_get_chat_data(conversation));

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    ASSERT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));

    tgl.update(make_object<updateSupergroup>(
        makeForumSupergroup(
            groupId,
            make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    addSavedTopicBookmark(topicDisplayName("Prior"));
    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    tgl.update(make_object<updateChatLastMessage>(
        groupChatId, nullptr,
        std::vector<object_ptr<chatPosition>>()));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        LiveGeneralMessageId, userIds[0], groupChatId,
        false, LiveGeneralMessageId,
        makeTextMessage("Live General"),
        make_object<messageTopicForum>(
            ForumTopicId::general().value()))));
    const uint64_t historyRequest = tgl.verifyRequest(
        getChatHistory(
            groupChatId, LiveGeneralMessageId,
            0, 30, false));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    std::vector<object_ptr<message>> history;
    history.push_back(makeMessage(
        HistoricalMessageId, userIds[0], groupChatId,
        false, HistoricalMessageId,
        makeTextMessage("Historical child"),
        make_object<messageTopicForum>(TopicId)));
    history.push_back(makeMessage(
        1, userIds[0], groupChatId, false, 1,
        makeTextMessage("Stop")));
    tgl.reply(historyRequest, make_object<messages>(
        history.size(), std::move(history)));

    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId,
        {HistoricalMessageId}, true,
        make_object<messageSourceForumTopicHistory>()));
    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId,
        {LiveGeneralMessageId}, true,
        make_object<messageSourceForumTopicHistory>()));
    tgl.verifyNoRequests();
    prpl.discardEvents();
    ASSERT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));

    tgl.reply(
        exactRequest,
        make_object<error>(
            404, "Historical evidence is not fresh"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        JoinChatFailedEvent(
            connection, topicPurpleName()));
}

TEST_F(
    ForumTopicJoinTest,
    LiveChildKeepsPendingMetadataLookupForTitleRefinement)
{
    constexpr int64_t MessageId = 10000;
    const std::string savedTitle =
        topicDisplayName("Saved");
    const std::string refinedTitle =
        topicDisplayName("Refined");
    PurpleConversation *conversation = nullptr;

    loginWithForumSupergroup();
    conversation = addSavedLeftTopic(savedTitle);
    ASSERT_NE(nullptr, conversation);

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    receiveTopicText(
        MessageId, 12345, "Live child");
    verifyForumTopicReadReceipt(MessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, topicPurpleName(),
            savedTitle),
        ServGotChatEvent(
            connection, 2,
            userFirstNames[0] + " " + userLastNames[0],
            "Live child", PURPLE_MESSAGE_RECV,
            12345));

    tgl.reply(exactRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Refined")));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        AliasChatEvent(topicPurpleName(), refinedTitle),
        ConvSetTitleEvent(topicPurpleName(), refinedTitle));
    ASSERT_NE(
        nullptr,
        purple_conversation_get_chat_data(conversation));
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
    EXPECT_STREQ(
        refinedTitle.c_str(),
        purple_conversation_get_title(conversation));
}

TEST_F(
    ForumTopicJoinTest,
    DelayedLiveChildProactivelyOpensAutoRejoinAndRefinesTitle)
{
    constexpr int64_t MessageId = 10000;
    constexpr int32_t FileId = 1234;
    const std::string savedTitle =
        topicDisplayName("Saved");
    const std::string refinedTitle =
        topicDisplayName("Refined");

    loginWithForumSupergroup();
    PurpleConversation *conversation =
        addSavedLeftTopic(savedTitle);
    ASSERT_NE(nullptr, conversation);

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    const uint64_t downloadRequest =
        receiveDelayedTopicPhoto(
            MessageId, 12345, FileId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotJoinedChatEvent(
        connection, 2, topicPurpleName(),
        savedTitle));
    ASSERT_NE(
        nullptr,
        purple_conversation_get_chat_data(conversation));
    ASSERT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));

    tgl.reply(exactRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Refined")));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        AliasChatEvent(topicPurpleName(), refinedTitle),
        ConvSetTitleEvent(topicPurpleName(), refinedTitle));

    completeTopicPhotoDownload(
        downloadRequest, FileId);
    verifyForumTopicReadReceipt(MessageId);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotChatEvent(
        connection, 2,
        userFirstNames[0] + " " + userLastNames[0],
        "<img src=\"file:///path\">\nphoto",
        static_cast<PurpleMessageFlags>(
            PURPLE_MESSAGE_RECV |
            PURPLE_MESSAGE_IMAGES),
        12345));
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(ForumTopicJoinTest, RemovedBookmarkCancelsDeferredAutoRejoinSilently)
{
    login();
    PurpleChat *bookmark =
        addSavedTopicBookmark(topicDisplayName("Saved"));
    ASSERT_NE(nullptr, bookmark);

    joinTopic();
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    purple_blist_remove_chat(bookmark);
    prpl.discardEvents();

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateNewChat>(makeChat(
        groupChatId,
        make_object<chatTypeSupergroup>(groupId, false),
        groupChatTitle, nullptr, 0, 0, 0)));

    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
    expectNoTopicOrGeneralConversation();
}

TEST_F(ForumTopicJoinTest, RemovedBookmarkCancelsPendingAutoRejoinSilently)
{
    loginWithForumSupergroup();
    PurpleChat *bookmark =
        addSavedTopicBookmark(topicDisplayName("Saved"));
    ASSERT_NE(nullptr, bookmark);

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    purple_blist_remove_chat(bookmark);
    prpl.discardEvents();
    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Must stay closed")));

    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
    expectNoTopicOrGeneralConversation();
}

TEST_F(ForumTopicJoinTest, RemovedBookmarkSuppressesEligibilityFailure)
{
    loginWithForumSupergroup();
    PurpleChat *bookmark =
        addSavedTopicBookmark(topicDisplayName("Saved"));
    ASSERT_NE(nullptr, bookmark);

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    purple_blist_remove_chat(bookmark);
    prpl.discardEvents();
    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusLeft>(), 2)));

    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Late")));
    prpl.verifyNoEvents();
    expectNoTopicOrGeneralConversation();
}

TEST_F(ForumTopicJoinTest, ExactAutoRejoinWaitsForParentMetadata)
{
    login();

    const std::string purpleName = topicPurpleName();
    const std::string displayName = topicDisplayName("Saved");
    purple_blist_add_chat(
        purple_chat_new(
            account, displayName.c_str(),
            getChatComponents(topicTarget())),
        nullptr, nullptr);
    serv_got_joined_chat(connection, 99, purpleName.c_str());
    PurpleConversation *oldConversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account);
    ASSERT_NE(nullptr, oldConversation);
    purple_conv_chat_left(
        purple_conversation_get_chat_data(oldConversation));
    prpl.discardEvents();

    joinTopic();
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.update(make_object<updateNewChat>(makeChat(
        groupChatId, make_object<chatTypeSupergroup>(groupId, false),
        groupChatTitle, nullptr, 0, 0, 0)));
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    prpl.verifyNoEvents();

    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Saved")));
    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, 2, purpleName, displayName),
        PresentConversationEvent(purpleName));
    tgl.verifyNoRequests();
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            groupChatPurpleName.c_str(), account));
}

TEST_F(ForumTopicJoinTest, SavedJoinRefreshesAliasAndConversationTitle)
{
    loginWithForumSupergroup();
    cacheTopic("Current");
    addSavedLeftTopic("Old parent / Old topic");

    joinTopic();

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        AliasChatEvent(
            topicPurpleName(), topicDisplayName("Current")),
        ServGotJoinedChatEvent(
            connection, 2, topicPurpleName(),
            topicDisplayName("Current")),
        PresentConversationEvent(topicPurpleName())
    );

    PurpleChat *bookmark =
        purple_blist_find_chat(account, topicPurpleName().c_str());
    ASSERT_NE(nullptr, bookmark);
    EXPECT_STREQ(
        topicDisplayName("Current").c_str(),
        purple_chat_get_name(bookmark));
    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    EXPECT_STREQ(
        topicDisplayName("Current").c_str(),
        purple_conversation_get_title(conversation));
}

TEST_F(ForumTopicJoinTest, LiveRenameRefreshesSavedAliasAndOpenTitle)
{
    loginWithForumSupergroup();
    cacheTopic("Before");
    joinTopic();
    expectTopicOpened("Before");
    addSavedTopicBookmark(topicDisplayName("Before"));

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, TopicId, "After", false, true)));

    prpl.verifyEvents(
        AliasChatEvent(
            topicPurpleName(), topicDisplayName("After")),
        ConvSetTitleEvent(
            topicPurpleName(), topicDisplayName("After"))
    );
    tgl.verifyNoRequests();

    PurpleChat *bookmark =
        purple_blist_find_chat(account, topicPurpleName().c_str());
    ASSERT_NE(nullptr, bookmark);
    EXPECT_STREQ(
        topicDisplayName("After").c_str(),
        purple_chat_get_name(bookmark));

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
    EXPECT_STREQ(
        topicDisplayName("After").c_str(),
        purple_conversation_get_title(conversation));
    EXPECT_STREQ(
        topicPurpleName().c_str(),
        purple_conversation_get_name(conversation));

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, TopicId, "After", false, true)));
    prpl.verifyNoEvents();
    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, TopicId, "After", false, false)));
    prpl.verifyNoEvents();
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(ForumTopicJoinTest, ParentRenameRefreshesSavedTopicPresentation)
{
    loginWithForumSupergroup();
    cacheTopic("Child");
    joinTopic();
    expectTopicOpened("Child");
    addSavedTopicBookmark(topicDisplayName("Child"));

    tgl.update(make_object<updateChatTitle>(
        groupChatId, "Renamed parent"));

    const std::string renamedTitle =
        "Renamed parent / Child";
    prpl.verifyEvents(
        AliasChatEvent(groupChatPurpleName, "Renamed parent"),
        AliasChatEvent(topicPurpleName(), renamedTitle),
        ConvSetTitleEvent(topicPurpleName(), renamedTitle)
    );

    PurpleChat *bookmark =
        purple_blist_find_chat(account, topicPurpleName().c_str());
    ASSERT_NE(nullptr, bookmark);
    EXPECT_STREQ(
        renamedTitle.c_str(), purple_chat_get_name(bookmark));
    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    EXPECT_STREQ(
        renamedTitle.c_str(),
        purple_conversation_get_title(conversation));
    EXPECT_STREQ(
        topicPurpleName().c_str(),
        purple_conversation_get_name(conversation));
}

TEST_F(ForumTopicJoinTest, ClientDestructionDuringParentRenameIsSafe)
{
    loginWithForumSupergroup();
    cacheTopic("Child");
    joinTopic();
    expectTopicOpened("Child");
    addSavedTopicBookmark(topicDisplayName("Child"));

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::AliasChat, type);
            pluginInfo().close(connection);
        });
    tgl.update(make_object<updateChatTitle>(
        groupChatId, "Renamed parent"));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.discardEvents();
    tgl.verifyNoRequests();
}

TEST_F(ForumTopicJoinTest, ClientDestructionDuringRenameProjectionIsSafe)
{
    loginWithForumSupergroup();
    cacheTopic("Before");
    joinTopic();
    expectTopicOpened("Before");
    addSavedTopicBookmark(topicDisplayName("Before"));

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::AliasChat, type);
            pluginInfo().close(connection);
        });
    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, TopicId, "After")));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.discardEvents();
    tgl.verifyNoRequests();
}

TEST_F(ForumTopicJoinTest, ClientDestructionDuringExactJoinIsSafe)
{
    loginWithForumSupergroup();
    addSavedLeftTopic("Old parent / Old topic");

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::AliasChat, type);
            pluginInfo().close(connection);
        });
    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Current")));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.discardEvents();
    tgl.verifyNoRequests();
}

TEST_F(ForumTopicJoinTest, ClientDestructionDuringJoinedChatIsSafe)
{
    loginWithForumSupergroup();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t requestId = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::ServGotJoinedChat, type);
            pluginInfo().close(connection);
        });
    tgl.reply(requestId, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Current")));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.discardEvents();
    tgl.verifyNoRequests();
}

TEST_F(ForumTopicJoinTest, CompleteListingRemovalLeavesAndCanReviveTopic)
{
    loginWithForumSupergroup();
    cacheTopic("Before");
    joinTopic();
    expectTopicOpened("Before");
    addSavedTopicBookmark(topicDisplayName("Before"));

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));
    const int32_t purpleId = purple_conv_chat_get_id(
        purple_conversation_get_chat_data(conversation));

    uint64_t listRequest = 0;
    PurpleRoomlist *roomlist = startRoomList(listRequest);
    ASSERT_NE(nullptr, roomlist);
    tgl.reply(listRequest, makeForumTopicsPage(
        0, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(
        RoomlistInProgressEvent(roomlist, FALSE));
    purple_roomlist_unref(roomlist);

    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
    EXPECT_NE(
        nullptr,
        purple_blist_find_chat(
            account, topicPurpleName().c_str()));

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    tgl.reply(exactRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Restored")));

    prpl.verifyEvents(
        AliasChatEvent(
            topicPurpleName(), topicDisplayName("Restored")),
        ServGotJoinedChatEvent(
            connection, purpleId, topicPurpleName(),
            topicDisplayName("Restored")),
        PresentConversationEvent(topicPurpleName())
    );
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(ForumTopicJoinTest, PartialListingDoesNotRemoveActiveTopicEarly)
{
    loginWithForumSupergroup();
    cacheTopic("Active");
    joinTopic();
    expectTopicOpened("Active");

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));

    uint64_t listRequest = 0;
    PurpleRoomlist *roomlist = startRoomList(listRequest);
    ASSERT_NE(nullptr, roomlist);

    constexpr int32_t OtherTopicId = 43;
    std::vector<object_ptr<forumTopic>> topics;
    topics.push_back(makeForumTopic(makeForumTopicInfo(
        groupChatId, OtherTopicId, "Other")));
    tgl.reply(listRequest, makeForumTopicsPage(
        1, std::move(topics), 10, 20, OtherTopicId));
    prpl.verifyEvents(
        RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            topicDisplayName("Other"),
            nullptr,
            std::vector<std::string>{
                topicPurpleName(OtherTopicId)}
        )
    );
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));

    const uint64_t finalRequest = tgl.verifyRequest(
        getForumTopics(
            groupChatId, "", 10, 20, OtherTopicId, 100));
    tgl.reply(finalRequest, makeForumTopicsPage(
        1, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(
        RoomlistInProgressEvent(roomlist, FALSE));
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicJoinTest, ForumDisableSuspendsUntilExplicitRejoin)
{
    loginWithForumSupergroup();
    cacheTopic("Child");
    joinTopic();
    expectTopicOpened("Child");

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));
    const int32_t purpleId = purple_conv_chat_get_id(
        purple_conversation_get_chat_data(conversation));

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
    EXPECT_EQ(
        nullptr,
        purple_blist_find_chat(
            account, topicPurpleName().c_str()));

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    tgl.reply(exactRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Child")));

    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, purpleId, topicPurpleName(),
            topicDisplayName("Child")),
        ConvSetTitleEvent(
            topicPurpleName(), topicDisplayName("Child")),
        PresentConversationEvent(topicPurpleName())
    );
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(ForumTopicJoinTest, SuspensionLeavesOnlyExactChildWithStaleId)
{
    loginWithForumSupergroup();
    cacheTopic("Child");
    serv_got_joined_chat(
        connection, 1, groupChatPurpleName.c_str());
    serv_got_joined_chat(
        connection, 1, topicPurpleName().c_str());
    PurpleConversation *general =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            groupChatPurpleName.c_str(), account);
    PurpleConversation *child =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, general);
    ASSERT_NE(nullptr, child);
    prpl.discardEvents();

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));

    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(general)));
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(child)));
}

TEST_F(ForumTopicJoinTest, MembershipLossSuspendsUntilExplicitRejoin)
{
    loginWithForumSupergroup();
    cacheTopic("Child");
    joinTopic();
    expectTopicOpened("Child");
    addSavedTopicBookmark(topicDisplayName("Child"));

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));
    const int32_t purpleId = purple_conv_chat_get_id(
        purple_conversation_get_chat_data(conversation));

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusLeft>(), 2)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
    EXPECT_NE(
        nullptr,
        purple_blist_find_chat(
            account, topicPurpleName().c_str()));

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, TopicId, "Late while ineligible")));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    joinTopic();
    prpl.verifyNoEvents();
    const uint64_t exactRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    tgl.reply(exactRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Child")));

    prpl.verifyEvents(
        ServGotJoinedChatEvent(
            connection, purpleId, topicPurpleName(),
            topicDisplayName("Child")),
        PresentConversationEvent(topicPurpleName())
    );
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(ForumTopicJoinTest, GeneralMetadataKeepsLegacyRoomIdentity)
{
    loginWithForumSupergroup();
    serv_got_joined_chat(
        connection, 1, groupChatPurpleName.c_str());
    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            groupChatPurpleName.c_str(), account);
    ASSERT_NE(nullptr, conversation);
    prpl.discardEvents();

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, ForumTopicId::general().value(),
            "General", true, true, true)));

    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
    EXPECT_FALSE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
    EXPECT_STREQ(
        groupChatPurpleName.c_str(),
        purple_conversation_get_name(conversation));
}

TEST_F(ForumTopicJoinTest, ChildAdministrationOperationsFailClosed)
{
    loginWithForumSupergroup();
    cacheTopic("Child");
    joinTopic();
    expectTopicOpened("Child");

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    const int32_t purpleId = purple_conv_chat_get_id(
        purple_conversation_get_chat_data(conversation));

    pluginInfo().set_chat_topic(
        connection, purpleId, "must not edit the parent");
    pluginInfo().chat_invite(
        connection, purpleId, nullptr,
        (userFirstNames[0] + " " + userLastNames[0]).c_str());
    prpl.runCommand(
        "kick", conversation, {purpleUserName(0)});
    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), "",
        "Group administration is unavailable from a topic room",
        PURPLE_MESSAGE_NO_LOG, 0));

#if PURPLE_VERSION_CHECK(2,14,0)
    ASSERT_NE(nullptr, pluginInfo().chat_can_receive_file);
    EXPECT_TRUE(
        pluginInfo().chat_can_receive_file(connection, purpleId));
    EXPECT_TRUE(
        pluginInfo().chat_can_receive_file(connection, 1));
#endif

    tgl.verifyNoRequests();
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            groupChatPurpleName.c_str(), account));
    prpl.verifyNoEvents();
}

TEST_F(ForumTopicJoinTest, StaleChildConversationIdCannotReachParent)
{
    loginWithForumSupergroup();
    serv_got_joined_chat(
        connection, 1, topicPurpleName().c_str());
    prpl.discardEvents();

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);

    EXPECT_LT(
        pluginInfo().chat_send(
            connection, 1, "must not reach General",
            PURPLE_MESSAGE_SEND),
        0);
    pluginInfo().set_chat_topic(
        connection, 1, "must not edit the parent");
    pluginInfo().chat_invite(
        connection, 1, nullptr,
        (userFirstNames[0] + " " + userLastNames[0]).c_str());
    prpl.runCommand(
        "kick", conversation, {purpleUserName(0)});
    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), "",
        "Group administration is unavailable from a topic room",
        PURPLE_MESSAGE_NO_LOG, 0));

#if PURPLE_VERSION_CHECK(2,14,0)
    EXPECT_FALSE(
        pluginInfo().chat_can_receive_file(connection, 1));
#endif

    tgl.verifyNoRequests();
}
