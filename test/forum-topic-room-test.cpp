#include "account-data.h"
#include "forum-topics.h"
#include "supergroup-test.h"
#include "purple-info.h"

#include <gtest/gtest.h>

using namespace td::td_api;

class ForumTopicRoomTest: public SupergroupTest {
protected:
    void loginWithForumSupergroup()
    {
        loginWithSupergroup();
        tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
            groupId, make_object<chatMemberStatusMember>(), 2)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    uint64_t startRoomList(PurpleRoomlist *&roomlist)
    {
        roomlist = pluginInfo().roomlist_get_list(connection);
        EXPECT_NE(nullptr, roomlist);
        if (!roomlist)
            return 0;

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
        return tgl.verifyRequest(getForumTopics(
            groupChatId, "", 0, 0, 0, 100));
    }

    PurpleRoomlist *startPostLoginRoomListWithMissingMetadata(
        object_ptr<ChatType> chatType)
    {
        login();

        tgl.update(make_object<updateNewChat>(makeChat(
            groupChatId, std::move(chatType), groupChatTitle
        )));
        tgl.update(makeUpdateChatListMain(groupChatId));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
        EXPECT_EQ(nullptr, purple_blist_find_chat(
            account, groupChatPurpleName.c_str()));

        PurpleRoomlist *roomlist =
            pluginInfo().roomlist_get_list(connection);
        EXPECT_NE(nullptr, roomlist);
        if (!roomlist)
            return nullptr;

        prpl.verifyEvents(RoomlistInProgressEvent(roomlist, TRUE));
        tgl.verifyNoRequests();
        EXPECT_EQ(2U, roomlist->ref);
        EXPECT_NE(nullptr, roomlist->proto_data);
        EXPECT_EQ(nullptr, roomlist->rooms);
        return roomlist;
    }

    object_ptr<forumTopic> topic(int32_t topicId, const std::string &name,
                                 bool isGeneral = false)
    {
        return makeForumTopic(makeForumTopicInfo(
            groupChatId, topicId, name, isGeneral));
    }

    std::string topicPurpleName(int32_t topicId)
    {
        return getPurpleChatName(ChatTarget::forumTopic(
            ChatId::fromString(std::to_string(groupChatId).c_str()),
            ForumTopicId::fromValue(topicId)));
    }

    void expectTopicRoom(PurpleRoomlist *roomlist, int32_t topicId,
                         const std::string &name)
    {
        prpl.verifyEvents(RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle + " / " + name,
            nullptr,
            std::vector<std::string>{topicPurpleName(topicId)}
        ));
    }

    void expectNoTopicSideEffects(int32_t topicId)
    {
        const std::string purpleName = topicPurpleName(topicId);
        EXPECT_EQ(nullptr, purple_blist_find_chat(account, purpleName.c_str()));
        EXPECT_EQ(
            nullptr,
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT, purpleName.c_str(), account)
        );
    }
};

TEST_F(ForumTopicRoomTest, RoomListWithoutClientReturnsNull)
{
    PurpleRoomlist *roomlist = pluginInfo().roomlist_get_list(connection);
    if (roomlist)
        purple_roomlist_unref(roomlist);
    EXPECT_EQ(nullptr, roomlist);
}

TEST_F(ForumTopicRoomTest, MetadataBeforeParentIsRetainedWithoutPreprojection)
{
    TestTransceiver backend;
    TdTransceiver transceiver(nullptr, nullptr, nullptr, &backend);
    TdAccountData accountData(account, transceiver);
    ForumTopicsAdapter adapter(transceiver, accountData);

    object_ptr<updateForumTopicInfo> topicUpdate =
        make_object<updateForumTopicInfo>(
            makeForumTopicInfo(groupChatId, 42, "Early"));
    adapter.processUpdate(*topicUpdate);
    const TdAccountData::ForumTopicState *storedTopic =
        accountData.findForumTopic(ChatTarget::forumTopic(
            ChatId::fromString(std::to_string(groupChatId).c_str()),
            ForumTopicId::fromValue(42)));
    ASSERT_NE(nullptr, storedTopic);
    EXPECT_TRUE(storedTopic->metadataKnown);
    EXPECT_EQ("Early", storedTopic->name);

    accountData.updateSupergroup(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2));
    accountData.addChat(makeChat(
        groupChatId,
        make_object<chatTypeSupergroup>(groupId, false),
        groupChatTitle
    ));
    adapter.markRoomListsReady();

    PurpleRoomlist *roomlist = purple_roomlist_new(account);
    adapter.startRoomList(roomlist);
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
    backend.verifyRequest(
        getForumTopics(groupChatId, "", 0, 0, 0, 100));

    adapter.cancelRoomList(roomlist);
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    EXPECT_EQ(1U, roomlist->ref);
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, SavedCachedTopicWaitsForAuthoritativeRefresh)
{
    login();

    TestTransceiver backend;
    PurpleTdClient *client = static_cast<PurpleTdClient *>(
        purple_connection_get_protocol_data(connection));
    ASSERT_NE(nullptr, client);
    TdTransceiver transceiver(client, account, nullptr, &backend);
    TdAccountData accountData(account, transceiver);
    ForumTopicsAdapter adapter(transceiver, accountData);
    const ChatId chatId =
        ChatId::fromString(std::to_string(groupChatId).c_str());
    const ChatTarget target = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));

    accountData.updateSupergroup(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2));
    accountData.addChat(makeChat(
        groupChatId,
        make_object<chatTypeSupergroup>(groupId, false),
        groupChatTitle
    ));
    accountData.upsertForumTopic(target, "Saved", false, false, 1);
    ASSERT_TRUE(accountData.setForumTopicSaved(target, true));
    const TdAccountData::ForumTopicState *saved =
        accountData.findForumTopic(target);
    ASSERT_NE(nullptr, saved);
    ASSERT_TRUE(saved->saved);
    ASSERT_GT(saved->purpleId, 0);
    const int32_t savedPurpleId = saved->purpleId;
    adapter.markRoomListsReady();

    PurpleRoomlist *roomlist = purple_roomlist_new(account);
    adapter.startRoomList(roomlist);
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
    const uint64_t requestId = backend.verifyRequest(getForumTopics(
        groupChatId, "", 0, 0, 0, 100));
    EXPECT_EQ(1U, g_list_length(roomlist->rooms));

    backend.reply(requestId, makeForumTopicsPage(
        0, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);

    saved = accountData.findForumTopic(target);
    ASSERT_NE(nullptr, saved);
    EXPECT_TRUE(saved->deleted);
    EXPECT_TRUE(saved->saved);
    EXPECT_EQ(savedPurpleId, saved->purpleId);
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, OnlyCompleteValidListingsReconcile)
{
    login();

    TestTransceiver backend;
    PurpleTdClient *client = static_cast<PurpleTdClient *>(
        purple_connection_get_protocol_data(connection));
    ASSERT_NE(nullptr, client);
    TdTransceiver transceiver(client, account, nullptr, &backend);
    TdAccountData accountData(account, transceiver);
    ForumTopicsAdapter adapter(transceiver, accountData);
    const ChatId chatId =
        ChatId::fromString(std::to_string(groupChatId).c_str());
    const ChatTarget cachedTarget = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));

    accountData.updateSupergroup(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2));
    accountData.addChat(makeChat(
        groupChatId,
        make_object<chatTypeSupergroup>(groupId, false),
        groupChatTitle
    ));
    accountData.upsertForumTopic(
        cachedTarget, "Cached", false, false, 1);
    adapter.markRoomListsReady();

    const auto startDirectList =
        [&](PurpleRoomlist *&roomlist) -> uint64_t {
            roomlist = purple_roomlist_new(account);
            adapter.startRoomList(roomlist);
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
            return backend.verifyRequest(getForumTopics(
                groupChatId, "", 0, 0, 0, 100));
        };
    const auto expectCachedTopicRetained = [&]() {
        const TdAccountData::ForumTopicState *cached =
            accountData.findForumTopic(cachedTarget);
        ASSERT_NE(nullptr, cached);
        EXPECT_FALSE(cached->deleted);
        EXPECT_EQ("Cached", cached->name);
    };

    PurpleRoomlist *errorList = nullptr;
    uint64_t requestId = startDirectList(errorList);
    backend.reply(requestId, make_object<error>(
        500, "Temporary failure"));
    prpl.verifyEvents(RoomlistInProgressEvent(errorList, FALSE));
    expectCachedTopicRetained();
    purple_roomlist_unref(errorList);

    PurpleRoomlist *cycleList = nullptr;
    requestId = startDirectList(cycleList);
    std::vector<object_ptr<forumTopic>> cyclePage;
    cyclePage.push_back(topic(43, "Other"));
    backend.reply(requestId, makeForumTopicsPage(
        1, std::move(cyclePage)));
    prpl.verifyEvents(
        RoomlistAddRoomEvent(
            cycleList,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle + " / Other",
            nullptr,
            std::vector<std::string>{topicPurpleName(43)}
        ),
        RoomlistInProgressEvent(cycleList, FALSE)
    );
    expectCachedTopicRetained();
    purple_roomlist_unref(cycleList);

    PurpleRoomlist *malformedList = nullptr;
    requestId = startDirectList(malformedList);
    std::vector<object_ptr<forumTopic>> malformedPage;
    malformedPage.push_back(nullptr);
    backend.reply(requestId, makeForumTopicsPage(
        1, std::move(malformedPage), 50, 500, 43));
    prpl.verifyNoEvents();
    requestId = backend.verifyRequest(getForumTopics(
        groupChatId, "", 50, 500, 43, 100));
    backend.reply(requestId, makeForumTopicsPage(
        1, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(RoomlistInProgressEvent(
        malformedList, FALSE));
    expectCachedTopicRetained();
    purple_roomlist_unref(malformedList);

    PurpleRoomlist *cancelledList = nullptr;
    requestId = startDirectList(cancelledList);
    adapter.cancelRoomList(cancelledList);
    prpl.verifyEvents(RoomlistInProgressEvent(
        cancelledList, FALSE));
    backend.reply(requestId, makeForumTopicsPage(
        0, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyNoEvents();
    expectCachedTopicRetained();
    purple_roomlist_unref(cancelledList);
}

TEST_F(ForumTopicRoomTest, DelayedForumMetadataKeepsRoomListPending)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    PurpleRoomlist *roomlist = pluginInfo().roomlist_get_list(connection);
    ASSERT_NE(nullptr, roomlist);
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, TRUE));
    ASSERT_EQ(2U, roomlist->ref);
    ASSERT_NE(nullptr, roomlist->proto_data);

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitTdlibParameters>()));
    std::vector<uint64_t> parameterRequests = tgl.verifyRequestsV(
        make_object<disableProxy>(),
        make_object<getProxies>(),
        makeDefaultParams()
    );
    ASSERT_EQ(3U, parameterRequests.size());
    tgl.reply(parameterRequests[0], make_object<ok>());
    tgl.reply(parameterRequests[1], make_object<addedProxies>(
        std::vector<object_ptr<addedProxy>>()));
    tgl.reply(parameterRequests[2], make_object<ok>());

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateReady>()));
    prpl.verifyEvents(ConnectionSetStateEvent(
        connection, PURPLE_CONNECTED));
    const uint64_t contactRequest = tgl.verifyRequest(getContacts());

    tgl.update(make_object<updateUser>(makeUser(
        selfId,
        selfFirstName,
        selfLastName,
        selfPhoneNumber,
        make_object<userStatusOffline>()
    )));
    tgl.update(make_object<updateConnectionState>(
        make_object<connectionStateReady>()));
    tgl.reply(contactRequest, make_object<users>());
    const uint64_t chatListRequest = tgl.verifyRequest(getChatsRequest());

    tgl.update(make_object<updateNewChat>(makeChat(
        groupChatId,
        make_object<chatTypeSupergroup>(groupId, false),
        groupChatTitle
    )));
    tgl.update(makeUpdateChatListMain(groupChatId));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    EXPECT_EQ(nullptr, purple_blist_find_chat(
        account, groupChatPurpleName.c_str()));

    tgl.reply(chatListRequest, getChatsNoChatsResponse());
    prpl.verifyEvents(
        AccountSetAliasEvent(
            account, selfFirstName + " " + selfLastName),
        ShowAccountEvent(account)
    );
    tgl.verifyNoRequests();
    EXPECT_EQ(2U, roomlist->ref);
    EXPECT_NE(nullptr, roomlist->proto_data);
    EXPECT_EQ(nullptr, roomlist->rooms);

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyEvents(
        AddChatEvent(
            groupChatPurpleName, groupChatTitle, account,
            nullptr, nullptr
        ),
        RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle,
            nullptr,
            std::vector<std::string>{groupChatPurpleName}
        )
    );
    std::vector<uint64_t> discoveryRequests = tgl.verifyRequestsV(
        make_object<getSupergroupFullInfo>(groupId),
        make_object<getSupergroupMembers>(
            groupId,
            make_object<supergroupMembersFilterRecent>(),
            0,
            200
        ),
        make_object<getForumTopics>(
            groupChatId, "", 0, 0, 0, 100)
    );
    ASSERT_EQ(3U, discoveryRequests.size());

    tgl.reply(discoveryRequests[0], make_object<error>(
        404, "No full info"));
    tgl.reply(discoveryRequests[1], make_object<error>(
        404, "No member list"));
    tgl.reply(discoveryRequests[2], makeForumTopicsPage(
        0, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);
    EXPECT_NE(nullptr, purple_blist_find_chat(
        account, groupChatPurpleName.c_str()));
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, groupChatPurpleName.c_str(), account)
    );

    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, PostLoginDelayedMetadataHoldsNewRoomList)
{
    PurpleRoomlist *roomlist =
        startPostLoginRoomListWithMissingMetadata(
            make_object<chatTypeSupergroup>(groupId, false));
    ASSERT_NE(nullptr, roomlist);

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyEvents(
        AddChatEvent(
            groupChatPurpleName, groupChatTitle, account,
            nullptr, nullptr
        ),
        RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle,
            nullptr,
            std::vector<std::string>{groupChatPurpleName}
        )
    );
    std::vector<uint64_t> discoveryRequests = tgl.verifyRequestsV(
        make_object<getSupergroupFullInfo>(groupId),
        make_object<getSupergroupMembers>(
            groupId,
            make_object<supergroupMembersFilterRecent>(),
            0,
            200
        ),
        make_object<getForumTopics>(
            groupChatId, "", 0, 0, 0, 100)
    );
    ASSERT_EQ(3U, discoveryRequests.size());

    tgl.reply(discoveryRequests[0], make_object<error>(
        404, "No full info"));
    tgl.reply(discoveryRequests[1], make_object<error>(
        404, "No member list"));
    tgl.reply(discoveryRequests[2], makeForumTopicsPage(
        0, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    tgl.verifyNoRequests();
    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, DelayedOrdinarySupergroupPreservesLegacyRoom)
{
    PurpleRoomlist *roomlist =
        startPostLoginRoomListWithMissingMetadata(
            make_object<chatTypeSupergroup>(groupId, false));
    ASSERT_NE(nullptr, roomlist);

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    prpl.verifyEvents(
        AddChatEvent(
            groupChatPurpleName, groupChatTitle, account,
            nullptr, nullptr
        ),
        RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle,
            nullptr,
            std::vector<std::string>{groupChatPurpleName}
        ),
        RoomlistInProgressEvent(roomlist, FALSE)
    );
    std::vector<uint64_t> groupRequests = tgl.verifyRequestsV(
        make_object<getSupergroupFullInfo>(groupId),
        make_object<getSupergroupMembers>(
            groupId,
            make_object<supergroupMembersFilterRecent>(),
            0,
            200
        )
    );
    ASSERT_EQ(2U, groupRequests.size());
    tgl.reply(groupRequests[0], make_object<error>(
        404, "No full info"));
    tgl.reply(groupRequests[1], make_object<error>(
        404, "No member list"));
    tgl.verifyNoRequests();
    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);
    EXPECT_NE(nullptr, purple_blist_find_chat(
        account, groupChatPurpleName.c_str()));
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, DelayedBasicGroupPreservesLegacyRoom)
{
    PurpleRoomlist *roomlist =
        startPostLoginRoomListWithMissingMetadata(
            make_object<chatTypeBasicGroup>(groupId));
    ASSERT_NE(nullptr, roomlist);

    tgl.update(make_object<updateBasicGroup>(make_object<basicGroup>(
        groupId,
        2,
        make_object<chatMemberStatusMember>(),
        true,
        0
    )));
    prpl.verifyEvents(
        AddChatEvent(
            groupChatPurpleName, groupChatTitle, account,
            nullptr, nullptr
        ),
        RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle,
            nullptr,
            std::vector<std::string>{groupChatPurpleName}
        ),
        RoomlistInProgressEvent(roomlist, FALSE)
    );
    const uint64_t groupRequest = tgl.verifyRequest(
        getBasicGroupFullInfo(groupId));
    tgl.reply(groupRequest, make_object<error>(
        404, "No full info"));
    tgl.verifyNoRequests();
    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);
    EXPECT_NE(nullptr, purple_blist_find_chat(
        account, groupChatPurpleName.c_str()));
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, DelayedNonMemberGroupIsNotProjected)
{
    PurpleRoomlist *roomlist =
        startPostLoginRoomListWithMissingMetadata(
            make_object<chatTypeSupergroup>(groupId, false));
    ASSERT_NE(nullptr, roomlist);

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusLeft>(), 2)));
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    tgl.verifyNoRequests();
    EXPECT_EQ(nullptr, roomlist->rooms);
    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);
    EXPECT_EQ(nullptr, purple_blist_find_chat(
        account, groupChatPurpleName.c_str()));
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, CancellationBeforeChatListReadyReleasesReference)
{
    TestTransceiver backend;
    TdTransceiver transceiver(nullptr, nullptr, nullptr, &backend);
    TdAccountData accountData(account, transceiver);
    ForumTopicsAdapter adapter(transceiver, accountData);

    PurpleRoomlist *roomlist = purple_roomlist_new(account);
    adapter.startRoomList(roomlist);
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, TRUE));
    ASSERT_EQ(2U, roomlist->ref);
    ASSERT_NE(nullptr, roomlist->proto_data);

    adapter.cancelRoomList(roomlist);
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);

    adapter.cancelRoomList(roomlist);
    prpl.verifyNoEvents();

    object_ptr<updateForumTopicInfo> cachedUpdate =
        make_object<updateForumTopicInfo>(
            makeForumTopicInfo(groupChatId, 42, "Cached"));
    adapter.processUpdate(*cachedUpdate);
    accountData.updateSupergroup(makeForumSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2));
    accountData.addChat(makeChat(
        groupChatId,
        make_object<chatTypeSupergroup>(groupId, false),
        groupChatTitle
    ));
    adapter.markRoomListsReady();

    object_ptr<updateForumTopicInfo> liveUpdate =
        make_object<updateForumTopicInfo>(
            makeForumTopicInfo(groupChatId, 43, "Live"));
    adapter.processUpdate(*liveUpdate);
    backend.verifyNoRequests();
    prpl.verifyNoEvents();
    EXPECT_EQ(nullptr, roomlist->rooms);
    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);

    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, PaginatesShortPagesUntilEmptyWithoutSideEffects)
{
    loginWithForumSupergroup();
    PurpleChat *generalNode = purple_blist_find_chat(
        account, groupChatPurpleName.c_str());
    ASSERT_NE(nullptr, generalNode);

    PurpleRoomlist *roomlist = nullptr;
    uint64_t requestId = startRoomList(roomlist);
    ASSERT_NE(0U, requestId);

    std::vector<object_ptr<forumTopic>> firstPage;
    firstPage.push_back(topic(ForumTopicId::general().value(), "General", true));
    firstPage.push_back(topic(42, "Same"));
    tgl.reply(requestId, makeForumTopicsPage(
        999, std::move(firstPage), 200, 2000, 42));
    expectTopicRoom(roomlist, 42, "Same");
    requestId = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 200, 2000, 42, 100));

    std::vector<object_ptr<forumTopic>> secondPage;
    secondPage.push_back(topic(43, "Same"));
    tgl.reply(requestId, makeForumTopicsPage(
        999, std::move(secondPage), 100, 1000, 43));
    expectTopicRoom(roomlist, 43, "Same");
    requestId = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 100, 1000, 43, 100));

    tgl.reply(requestId, makeForumTopicsPage(
        999, std::vector<object_ptr<forumTopic>>(), 1, 1, 99));
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    tgl.verifyNoRequests();

    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);
    EXPECT_EQ(generalNode, purple_blist_find_chat(
        account, groupChatPurpleName.c_str()));
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, groupChatPurpleName.c_str(), account)
    );
    expectNoTopicSideEffects(42);
    expectNoTopicSideEffects(43);
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, StopsAfterProcessingRepeatedOffset)
{
    loginWithForumSupergroup();
    PurpleRoomlist *roomlist = nullptr;
    uint64_t requestId = startRoomList(roomlist);
    ASSERT_NE(0U, requestId);

    std::vector<object_ptr<forumTopic>> firstPage;
    firstPage.push_back(topic(42, "First"));
    tgl.reply(requestId, makeForumTopicsPage(
        2, std::move(firstPage), 50, 500, 42));
    expectTopicRoom(roomlist, 42, "First");
    requestId = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 50, 500, 42, 100));

    std::vector<object_ptr<forumTopic>> secondPage;
    secondPage.push_back(topic(43, "Second"));
    tgl.reply(requestId, makeForumTopicsPage(
        3, std::move(secondPage), 40, 400, 43));
    expectTopicRoom(roomlist, 43, "Second");
    requestId = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 40, 400, 43, 100));

    std::vector<object_ptr<forumTopic>> thirdPage;
    thirdPage.push_back(topic(44, "Third"));
    tgl.reply(requestId, makeForumTopicsPage(
        3, std::move(thirdPage), 50, 500, 42));
    prpl.verifyEvents(
        RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle + " / Third",
            nullptr,
            std::vector<std::string>{topicPurpleName(44)}
        ),
        RoomlistInProgressEvent(roomlist, FALSE)
    );
    tgl.verifyNoRequests();

    expectNoTopicSideEffects(42);
    expectNoTopicSideEffects(43);
    expectNoTopicSideEffects(44);
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, FailureCompletesAndKeepsPartialList)
{
    loginWithForumSupergroup();
    PurpleRoomlist *roomlist = nullptr;
    uint64_t requestId = startRoomList(roomlist);
    ASSERT_NE(0U, requestId);

    std::vector<object_ptr<forumTopic>> firstPage;
    firstPage.push_back(topic(42, "Partial"));
    tgl.reply(requestId, makeForumTopicsPage(
        2, std::move(firstPage), 50, 500, 42));
    expectTopicRoom(roomlist, 42, "Partial");
    requestId = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 50, 500, 42, 100));

    tgl.reply(requestId, make_object<error>(500, "Temporary failure"));
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    tgl.verifyNoRequests();

    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(2U, g_list_length(roomlist->rooms));
    expectNoTopicSideEffects(42);
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, ForumParentsResetPaginationAndContinueAfterError)
{
    constexpr int64_t secondGroupId = 600;
    constexpr int64_t secondChatId = -6000;
    const std::string secondTitle = "Second parent";
    const ChatId secondChat = ChatId::fromString("-6000");
    const ChatTarget secondTopic = ChatTarget::forumTopic(
        secondChat, ForumTopicId::fromValue(42));
    const std::string secondPurpleName = getPurpleChatName(secondTopic);

    loginWithForumSupergroup();
    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        secondGroupId, make_object<chatMemberStatusMember>(), 2)));
    object_ptr<chat> secondParent = makeChat(
        secondChatId,
        make_object<chatTypeSupergroup>(secondGroupId, false),
        secondTitle
    );
    addChatPosition(secondParent, make_object<chatListMain>());
    tgl.update(make_object<updateNewChat>(std::move(secondParent)));
    prpl.verifyEvents(AddChatEvent(
        getPurpleChatName(ChatTarget::chat(secondChat)),
        secondTitle,
        account,
        nullptr,
        nullptr
    ));
    std::vector<uint64_t> discoveryRequests = tgl.verifyRequestsV(
        make_object<getSupergroupFullInfo>(secondGroupId),
        make_object<getSupergroupMembers>(
            secondGroupId,
            make_object<supergroupMembersFilterRecent>(),
            0,
            200
        )
    );
    ASSERT_EQ(2U, discoveryRequests.size());
    tgl.reply(discoveryRequests[0], make_object<supergroupFullInfo>());
    tgl.reply(discoveryRequests[1], make_object<chatMembers>());
    const uint64_t administratorsRequest = tgl.verifyRequest(
        make_object<getSupergroupMembers>(
            secondGroupId,
            make_object<supergroupMembersFilterAdministrators>(),
            0,
            200
        )
    );
    tgl.reply(administratorsRequest, make_object<chatMembers>());
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    PurpleRoomlist *roomlist = pluginInfo().roomlist_get_list(connection);
    ASSERT_NE(nullptr, roomlist);
    prpl.verifyEvents(
        RoomlistInProgressEvent(roomlist, TRUE),
        RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle,
            nullptr,
            std::vector<std::string>{groupChatPurpleName}
        ),
        RoomlistAddRoomEvent(
            roomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            secondTitle,
            nullptr,
            std::vector<std::string>{
                getPurpleChatName(ChatTarget::forumTopic(
                    secondChat, ForumTopicId::general()))
            }
        )
    );

    uint64_t requestId = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 0, 0, 0, 100));
    std::vector<object_ptr<forumTopic>> firstPage;
    firstPage.push_back(makeForumTopic(makeForumTopicInfo(
        groupChatId, 42, "First")));
    tgl.reply(requestId, makeForumTopicsPage(
        2, std::move(firstPage), 50, 500, 42));
    expectTopicRoom(roomlist, 42, "First");
    requestId = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 50, 500, 42, 100));

    tgl.reply(requestId, make_object<error>(
        500, "First forum unavailable"));
    prpl.verifyNoEvents();
    requestId = tgl.verifyRequest(getForumTopics(
        secondChatId, "", 0, 0, 0, 100));

    std::vector<object_ptr<forumTopic>> secondPage;
    secondPage.push_back(makeForumTopic(makeForumTopicInfo(
        secondChatId, 42, "Second")));
    tgl.reply(requestId, makeForumTopicsPage(
        1, std::move(secondPage), 75, 750, 42));
    prpl.verifyEvents(RoomlistAddRoomEvent(
        roomlist,
        PURPLE_ROOMLIST_ROOMTYPE_ROOM,
        secondTitle + " / Second",
        nullptr,
        std::vector<std::string>{secondPurpleName}
    ));
    requestId = tgl.verifyRequest(getForumTopics(
        secondChatId, "", 75, 750, 42, 100));

    tgl.reply(requestId, makeForumTopicsPage(
        1, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    tgl.verifyNoRequests();
    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, LiveMetadataWinsOverOlderPageResponse)
{
    loginWithForumSupergroup();
    PurpleRoomlist *roomlist = nullptr;
    uint64_t requestId = startRoomList(roomlist);
    ASSERT_NE(0U, requestId);

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(groupChatId, 42, "Current")));
    tgl.verifyNoRequests();
    expectTopicRoom(roomlist, 42, "Current");

    std::vector<object_ptr<forumTopic>> stalePage;
    stalePage.push_back(topic(42, "Stale"));
    tgl.reply(requestId, makeForumTopicsPage(
        1, std::move(stalePage), 50, 500, 42));
    prpl.verifyNoEvents();
    requestId = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 50, 500, 42, 100));

    tgl.reply(requestId, makeForumTopicsPage(
        1, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));

    PurpleRoomlist *secondRoomlist =
        pluginInfo().roomlist_get_list(connection);
    ASSERT_NE(nullptr, secondRoomlist);
    prpl.verifyEvents(
        RoomlistInProgressEvent(secondRoomlist, TRUE),
        RoomlistAddRoomEvent(
            secondRoomlist,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle,
            nullptr,
            std::vector<std::string>{groupChatPurpleName}
        )
    );
    const uint64_t secondRequest = tgl.verifyRequest(
        getForumTopics(groupChatId, "", 0, 0, 0, 100));
    tgl.reply(secondRequest, makeForumTopicsPage(
        0, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(RoomlistInProgressEvent(secondRoomlist, FALSE));

    expectNoTopicSideEffects(42);
    purple_roomlist_unref(roomlist);
    purple_roomlist_unref(secondRoomlist);
}

TEST_F(ForumTopicRoomTest, RoomListRowsAreStableSnapshots)
{
    loginWithForumSupergroup();
    PurpleRoomlist *roomlist = nullptr;
    uint64_t requestId = startRoomList(roomlist);
    ASSERT_NE(0U, requestId);

    std::vector<object_ptr<forumTopic>> firstPage;
    firstPage.push_back(topic(42, "Before rename"));
    tgl.reply(requestId, makeForumTopicsPage(
        1, std::move(firstPage), 50, 500, 42));
    expectTopicRoom(roomlist, 42, "Before rename");
    requestId = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 50, 500, 42, 100));

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(groupChatId, 42, "After rename")));
    prpl.verifyNoEvents();
    tgl.reply(requestId, makeForumTopicsPage(
        1, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));

    PurpleRoomlist *refreshedList =
        pluginInfo().roomlist_get_list(connection);
    ASSERT_NE(nullptr, refreshedList);
    prpl.verifyEvents(
        RoomlistInProgressEvent(refreshedList, TRUE),
        RoomlistAddRoomEvent(
            refreshedList,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle,
            nullptr,
            std::vector<std::string>{groupChatPurpleName}
        )
    );
    requestId = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 0, 0, 0, 100));

    std::vector<object_ptr<forumTopic>> refreshedPage;
    refreshedPage.push_back(topic(42, "After rename"));
    tgl.reply(requestId, makeForumTopicsPage(
        1, std::move(refreshedPage)));
    prpl.verifyEvents(
        RoomlistAddRoomEvent(
            refreshedList,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle + " / After rename",
            nullptr,
            std::vector<std::string>{topicPurpleName(42)}
        ),
        RoomlistInProgressEvent(refreshedList, FALSE)
    );

    EXPECT_EQ(2U, g_list_length(roomlist->rooms));
    EXPECT_EQ(2U, g_list_length(refreshedList->rooms));
    expectNoTopicSideEffects(42);
    purple_roomlist_unref(roomlist);
    purple_roomlist_unref(refreshedList);
}

TEST_F(ForumTopicRoomTest, CancellationIgnoresLateReplyAndReleasesReference)
{
    loginWithForumSupergroup();
    PurpleRoomlist *roomlist = nullptr;
    uint64_t requestId = startRoomList(roomlist);
    ASSERT_NE(0U, requestId);
    ASSERT_EQ(2U, roomlist->ref);
    ASSERT_NE(nullptr, roomlist->proto_data);

    pluginInfo().roomlist_cancel(roomlist);
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    EXPECT_EQ(1U, roomlist->ref);
    EXPECT_EQ(nullptr, roomlist->proto_data);

    pluginInfo().roomlist_cancel(roomlist);
    prpl.verifyNoEvents();

    std::vector<object_ptr<forumTopic>> latePage;
    latePage.push_back(topic(42, "Late"));
    tgl.reply(requestId, makeForumTopicsPage(
        1, std::move(latePage), 0, 0, 0));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    expectNoTopicSideEffects(42);
    purple_roomlist_unref(roomlist);
}

TEST_F(ForumTopicRoomTest, ConcurrentListsCancelIndependently)
{
    loginWithForumSupergroup();
    PurpleRoomlist *cancelledList = nullptr;
    const uint64_t cancelledRequest = startRoomList(cancelledList);
    ASSERT_NE(0U, cancelledRequest);

    PurpleRoomlist *activeList = nullptr;
    uint64_t activeRequest = startRoomList(activeList);
    ASSERT_NE(0U, activeRequest);

    pluginInfo().roomlist_cancel(cancelledList);
    prpl.verifyEvents(RoomlistInProgressEvent(cancelledList, FALSE));

    std::vector<object_ptr<forumTopic>> cancelledPage;
    cancelledPage.push_back(topic(41, "Ignored"));
    tgl.reply(cancelledRequest, makeForumTopicsPage(
        1, std::move(cancelledPage), 50, 500, 41));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    std::vector<object_ptr<forumTopic>> activePage;
    activePage.push_back(topic(42, "Active"));
    tgl.reply(activeRequest, makeForumTopicsPage(
        1, std::move(activePage), 50, 500, 42));
    expectTopicRoom(activeList, 42, "Active");
    activeRequest = tgl.verifyRequest(getForumTopics(
        groupChatId, "", 50, 500, 42, 100));

    tgl.reply(activeRequest, makeForumTopicsPage(
        1, std::vector<object_ptr<forumTopic>>()));
    prpl.verifyEvents(RoomlistInProgressEvent(activeList, FALSE));

    EXPECT_EQ(1U, cancelledList->ref);
    EXPECT_EQ(1U, activeList->ref);
    expectNoTopicSideEffects(41);
    expectNoTopicSideEffects(42);
    purple_roomlist_unref(cancelledList);
    purple_roomlist_unref(activeList);
}

TEST_F(ForumTopicRoomTest, ConcurrentListsRejectOlderPageMetadata)
{
    loginWithForumSupergroup();

    PurpleRoomlist *olderList = nullptr;
    const uint64_t olderRequest = startRoomList(olderList);
    ASSERT_NE(0U, olderRequest);

    PurpleRoomlist *newerList = nullptr;
    const uint64_t newerRequest = startRoomList(newerList);
    ASSERT_NE(0U, newerRequest);

    std::vector<object_ptr<forumTopic>> newerPage;
    newerPage.push_back(topic(42, "Current"));
    tgl.reply(newerRequest, makeForumTopicsPage(
        1, std::move(newerPage)));
    prpl.verifyEvents(
        RoomlistAddRoomEvent(
            newerList,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle + " / Current",
            nullptr,
            std::vector<std::string>{topicPurpleName(42)}
        ),
        RoomlistInProgressEvent(newerList, FALSE)
    );

    std::vector<object_ptr<forumTopic>> olderPage;
    olderPage.push_back(topic(42, "Stale"));
    tgl.reply(olderRequest, makeForumTopicsPage(
        1, std::move(olderPage)));
    prpl.verifyEvents(
        RoomlistAddRoomEvent(
            olderList,
            PURPLE_ROOMLIST_ROOMTYPE_ROOM,
            groupChatTitle + " / Current",
            nullptr,
            std::vector<std::string>{topicPurpleName(42)}
        ),
        RoomlistInProgressEvent(olderList, FALSE)
    );
    tgl.verifyNoRequests();

    EXPECT_EQ(2U, g_list_length(newerList->rooms));
    EXPECT_EQ(2U, g_list_length(olderList->rooms));
    EXPECT_EQ(1U, newerList->ref);
    EXPECT_EQ(1U, olderList->ref);
    EXPECT_EQ(nullptr, newerList->proto_data);
    EXPECT_EQ(nullptr, olderList->proto_data);
    expectNoTopicSideEffects(42);
    purple_roomlist_unref(newerList);
    purple_roomlist_unref(olderList);
}

TEST_F(ForumTopicRoomTest, DisconnectCompletesListAndIgnoresLateReply)
{
    loginWithForumSupergroup();
    PurpleRoomlist *roomlist = nullptr;
    uint64_t requestId = startRoomList(roomlist);
    ASSERT_NE(0U, requestId);
    ASSERT_EQ(2U, roomlist->ref);

    pluginInfo().close(connection);
    prpl.verifyEvents(RoomlistInProgressEvent(roomlist, FALSE));
    EXPECT_EQ(nullptr, purple_connection_get_protocol_data(connection));
    EXPECT_EQ(nullptr, roomlist->proto_data);
    EXPECT_EQ(1U, roomlist->ref);

    std::vector<object_ptr<forumTopic>> latePage;
    latePage.push_back(topic(42, "Late"));
    tgl.reply(requestId, makeForumTopicsPage(
        1, std::move(latePage), 0, 0, 0));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    expectNoTopicSideEffects(42);
    purple_roomlist_unref(roomlist);
}
