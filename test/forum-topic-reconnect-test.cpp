#include "libpurple-mock.h"
#include "purple-info.h"
#include "supergroup-test.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace td::td_api;

namespace {

constexpr int32_t CurrentTopicId = 42;
constexpr int32_t StaleTopicId = 84;
constexpr int32_t ReusedParentPurpleId = 2;
constexpr int32_t ReusedChildPurpleId = 3;
constexpr int64_t AllocationShiftChatId = 9000;
constexpr int64_t AllocationShiftUserId = 900;

object_ptr<sendMessage> expectedTopicTextSend(
    int64_t chatId, int32_t topicId,
    const std::string &text)
{
    return Mock_SendMessage(
        chatId, make_object<messageTopicForum>(topicId),
        nullptr, nullptr,
        Mock_InputMessageText(make_object<formattedText>(
            text, std::vector<object_ptr<textEntity>>())));
}

class ForumTopicReconnectTest : public SupergroupTest {
protected:
    ChatTarget topicTarget(int32_t topicId) const
    {
        const std::string chatId = std::to_string(groupChatId);
        return ChatTarget::forumTopic(
            ChatId::fromString(chatId.c_str()),
            ForumTopicId::fromValue(topicId));
    }

    std::string topicPurpleName(int32_t topicId) const
    {
        return getPurpleChatName(topicTarget(topicId));
    }

    void loginWithForumSupergroup()
    {
        loginWithSupergroup();
        tgl.update(make_object<updateSupergroup>(
            makeForumSupergroup(
                groupId,
                make_object<chatMemberStatusMember>(), 2)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    int32_t cacheAndOpenTopic(
        int32_t topicId, const std::string &name)
    {
        tgl.update(make_object<updateForumTopicInfo>(
            makeForumTopicInfo(
                groupChatId, topicId, name)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();

        GHashTable *components =
            getChatComponents(topicTarget(topicId));
        pluginInfo().join_chat(connection, components);
        g_hash_table_destroy(components);
        tgl.verifyNoRequests();
        prpl.discardEvents();

        PurpleConversation *conversation =
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT,
                topicPurpleName(topicId).c_str(), account);
        EXPECT_NE(nullptr, conversation);
        if (!conversation)
            return 0;

        PurpleConvChat *chat =
            purple_conversation_get_chat_data(conversation);
        EXPECT_NE(nullptr, chat);
        return chat ? purple_conv_chat_get_id(chat) : 0;
    }

    PurpleConversation *topicConversation(int32_t topicId) const
    {
        return purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName(topicId).c_str(), account);
    }

    void leaveTopicConversation(int32_t topicId)
    {
        PurpleConversation *conversation =
            topicConversation(topicId);
        ASSERT_NE(nullptr, conversation);
        PurpleConvChat *chat =
            purple_conversation_get_chat_data(conversation);
        ASSERT_NE(nullptr, chat);
        purple_conv_chat_left(chat);
    }

    void reconnectWithShiftedAllocationOrder()
    {
        loginWithForumSupergroup();
        ASSERT_EQ(
            ReusedParentPurpleId,
            cacheAndOpenTopic(CurrentTopicId, "Current"));
        ASSERT_EQ(
            ReusedChildPurpleId,
            cacheAndOpenTopic(StaleTopicId, "Stale"));
        leaveTopicConversation(CurrentTopicId);
        leaveTopicConversation(StaleTopicId);

        PurpleChat *parentBookmark =
            purple_blist_find_chat(
                account, groupChatPurpleName.c_str());
        ASSERT_NE(nullptr, parentBookmark);
        purple_blist_remove_chat(parentBookmark);
        prpl.discardEvents();

        pluginInfo().close(connection);
        ASSERT_EQ(
            nullptr,
            purple_connection_get_protocol_data(connection));
        purple_connection_set_state(
            connection, PURPLE_DISCONNECTED);
        prpl.discardEvents();

        login(
            make_vector<Object>(
                make_object<updateNewChat>(makeChat(
                    AllocationShiftChatId,
                    make_object<chatTypePrivate>(
                        AllocationShiftUserId),
                    "Allocation shift",
                    nullptr, 0, 0, 0)),
                make_object<updateSupergroup>(
                    makeForumSupergroup(
                        groupId,
                        make_object<chatMemberStatusMember>(),
                        2)),
                make_object<updateNewChat>(makeChat(
                    groupChatId,
                    make_object<chatTypeSupergroup>(
                        groupId, false),
                    groupChatTitle,
                    nullptr, 0, 0, 0))),
            make_object<users>(),
            make_object<error>(404, "Not Found"));

        PurpleConversation *oldCurrent =
            topicConversation(CurrentTopicId);
        PurpleConversation *oldStale =
            topicConversation(StaleTopicId);
        ASSERT_NE(nullptr, oldCurrent);
        ASSERT_NE(nullptr, oldStale);
        ASSERT_NE(
            nullptr,
            purple_conversation_get_chat_data(oldCurrent));
        ASSERT_NE(
            nullptr,
            purple_conversation_get_chat_data(oldStale));
        EXPECT_TRUE(purple_conv_chat_has_left(
            purple_conversation_get_chat_data(oldCurrent)));
        EXPECT_TRUE(purple_conv_chat_has_left(
            purple_conversation_get_chat_data(oldStale)));
        EXPECT_EQ(
            ReusedParentPurpleId,
            purple_conv_chat_get_id(
                purple_conversation_get_chat_data(
                    oldCurrent)));
        EXPECT_EQ(
            ReusedChildPurpleId,
            purple_conv_chat_get_id(
                purple_conversation_get_chat_data(
                    oldStale)));
    }
};

TEST_F(
    ForumTopicReconnectTest,
    LeftChildWithReusedParentIdDoesNotBlockAdministration)
{
    reconnectWithShiftedAllocationOrder();

    pluginInfo().set_chat_topic(
        connection, ReusedParentPurpleId,
        "Updated parent description");
    const uint64_t requestId = tgl.verifyRequest(
        setChatDescription(
            groupChatId, "Updated parent description"));
    tgl.reply(requestId, make_object<ok>());
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    serv_got_joined_chat(
        connection, ReusedParentPurpleId,
        topicPurpleName(CurrentTopicId).c_str());
    prpl.discardEvents();

    pluginInfo().set_chat_topic(
        connection, ReusedParentPurpleId,
        "must stay blocked");
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
}

TEST_F(
    ForumTopicReconnectTest,
    LeftMismatchedChildIdAllowsCurrentSendAndFileResolution)
{
    reconnectWithShiftedAllocationOrder();

    ASSERT_EQ(
        ReusedChildPurpleId,
        cacheAndOpenTopic(CurrentTopicId, "Current"));

    ASSERT_EQ(
        0,
        pluginInfo().chat_send(
            connection, ReusedChildPurpleId,
            "current topic", PURPLE_MESSAGE_SEND));
    tgl.verifyRequest(expectedTopicTextSend(
        groupChatId, CurrentTopicId,
        "current topic"));

#if PURPLE_VERSION_CHECK(2, 14, 0)
    ASSERT_NE(nullptr, pluginInfo().chat_can_receive_file);
    EXPECT_TRUE(pluginInfo().chat_can_receive_file(
        connection, ReusedChildPurpleId));
#endif

    serv_got_joined_chat(
        connection, ReusedChildPurpleId,
        topicPurpleName(StaleTopicId).c_str());
    prpl.discardEvents();

    EXPECT_LT(
        pluginInfo().chat_send(
            connection, ReusedChildPurpleId,
            "must not reach the wrong topic",
            PURPLE_MESSAGE_SEND),
        0);
#if PURPLE_VERSION_CHECK(2, 14, 0)
    EXPECT_FALSE(pluginInfo().chat_can_receive_file(
        connection, ReusedChildPurpleId));
#endif
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
}

} // namespace
