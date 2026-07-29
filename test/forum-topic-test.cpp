#include "account-data.h"
#include "purple-info.h"
#include "test-transceiver.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>
#include <vector>

using namespace td::td_api;

TEST(ForumTopicIdentityTest, RepresentsOrdinaryAndForumTargetsExplicitly)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ForumTopicId topicId = ForumTopicId::fromValue(42);

    const ChatTarget ordinary = ChatTarget::chat(chatId);
    const ChatTarget topic = ChatTarget::forumTopic(chatId, topicId);

    ASSERT_TRUE(ordinary.valid());
    ASSERT_FALSE(ordinary.isForumTopic());
    ASSERT_EQ(chatId, ordinary.chatId());
    ASSERT_EQ(ForumTopicId::invalid, ordinary.forumTopicId());

    ASSERT_TRUE(topic.valid());
    ASSERT_TRUE(topic.isForumTopic());
    ASSERT_EQ(chatId, topic.chatId());
    ASSERT_EQ(topicId, topic.forumTopicId());
    ASSERT_NE(ordinary, topic);
}

TEST(ForumTopicIdentityTest, AcceptsOnlyPositiveTopicIds)
{
    ASSERT_FALSE(ForumTopicId::fromValue(-1).valid());
    ASSERT_FALSE(ForumTopicId::fromValue(0).valid());
    ASSERT_EQ(1, ForumTopicId::general().value());
    ASSERT_EQ(std::numeric_limits<int32_t>::max(),
              ForumTopicId::fromValue(std::numeric_limits<int32_t>::max()).value());
}

TEST(ForumTopicIdentityTest, FormatsCanonicalStableRoomNames)
{
    const ChatId chatId = ChatId::fromString("-7000");

    ASSERT_EQ("chat-7000", getPurpleChatName(ChatTarget::chat(chatId)));
    ASSERT_EQ("chat-7000",
              getPurpleChatName(ChatTarget::forumTopic(chatId, ForumTopicId::general())));
    ASSERT_EQ("forum-7000-topic42",
              getPurpleChatName(ChatTarget::forumTopic(chatId, ForumTopicId::fromValue(42))));
}

TEST(ForumTopicIdentityTest, RoundTripsBoundaryIds)
{
    const std::string minChatName = "forum" +
        std::to_string(std::numeric_limits<int64_t>::min()) + "-topic" +
        std::to_string(std::numeric_limits<int32_t>::max());
    const ChatTarget minTarget = parsePurpleChatName(minChatName.c_str());

    ASSERT_TRUE(minTarget.valid());
    ASSERT_TRUE(minTarget.isForumTopic());
    ASSERT_EQ(std::numeric_limits<int64_t>::min(), minTarget.chatId().value());
    ASSERT_EQ(std::numeric_limits<int32_t>::max(), minTarget.forumTopicId().value());
    ASSERT_EQ(minChatName, getPurpleChatName(minTarget));

    const std::string maxChatName =
        "chat" + std::to_string(std::numeric_limits<int64_t>::max());
    const ChatTarget maxTarget = parsePurpleChatName(maxChatName.c_str());

    ASSERT_TRUE(maxTarget.valid());
    ASSERT_FALSE(maxTarget.isForumTopic());
    ASSERT_EQ(std::numeric_limits<int64_t>::max(), maxTarget.chatId().value());
    ASSERT_EQ(maxChatName, getPurpleChatName(maxTarget));
}

TEST(ForumTopicIdentityTest, RejectsMalformedAndNonCanonicalRoomNames)
{
    const char *invalidNames[] = {
        nullptr,
        "",
        "chat",
        "chat0",
        "chat+1",
        "chat 1",
        "chat01",
        "chat-01",
        "chat1junk",
        "chat9223372036854775808",
        "chat-9223372036854775809",
        "forum1-topic",
        "forum1-topic0",
        "forum1-topic-1",
        "forum1-topic+2",
        "forum1-topic01",
        "forum1-topic1",
        "forum1-topic2147483648",
        "forum1-topic2junk",
        "forum9223372036854775808-topic2"
    };

    for (const char *name : invalidNames)
        ASSERT_FALSE(parsePurpleChatName(name).valid()) << (name ? name : "(null)");
}

TEST(ForumTopicIdentityTest, LegacyChatIdParserRejectsTopicAndTrailingText)
{
    ASSERT_EQ(ChatId::fromString("-7000"), getTdlibChatId("chat-7000"));
    ASSERT_EQ(ChatId::invalid, getTdlibChatId("chat-7000junk"));
    ASSERT_EQ(ChatId::invalid, getTdlibChatId("forum-7000-topic42"));
}

TEST(ForumTopicIdentityTest, ChatComponentsUseTheCompleteCompositeName)
{
    const ChatTarget target = ChatTarget::forumTopic(
        ChatId::fromString("-9223372036854775808"),
        ForumTopicId::fromValue(std::numeric_limits<int32_t>::max()));
    GHashTable *components = getChatComponents(target);

    ASSERT_STREQ("forum-9223372036854775808-topic2147483647",
                 getChatName(components));

    g_hash_table_destroy(components);
}

class ForumTopicRegistryTest: public testing::Test {
protected:
    TestTransceiver backend;
    TdTransceiver transceiver;
    TdAccountData accountData;

    ForumTopicRegistryTest()
        : transceiver(nullptr, nullptr, nullptr, &backend),
          accountData(nullptr, transceiver)
    {}

    void addSupergroupChat(int64_t chatIdValue, int32_t supergroupId,
                           const std::string &title)
    {
        accountData.addChat(makeChat(
            chatIdValue,
            make_object<chatTypeSupergroup>(supergroupId, false),
            title
        ));
    }

    void setSupergroupForum(int32_t supergroupId, bool isForum)
    {
        object_ptr<supergroup> group = makeSupergroup(
            supergroupId,
            make_object<chatMemberStatusMember>(),
            1,
            false
        );
        group->is_forum_ = isForum;
        accountData.updateSupergroup(std::move(group));
    }
};

TEST_F(ForumTopicRegistryTest, GeneralReusesParentPurpleId)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget general = ChatTarget::forumTopic(chatId, ForumTopicId::general());
    setSupergroupForum(700, true);
    addSupergroupChat(chatId.value(), 700, "Group");

    const int parentPurpleId = accountData.getPurpleChatId(chatId);
    TdAccountData::ForumTopicUpsertResult result =
        accountData.upsertForumTopic(general, "General", false, false, 1);

    ASSERT_NE(nullptr, result.state);
    EXPECT_TRUE(result.applied);
    EXPECT_EQ(parentPurpleId, accountData.getPurpleChatId(general));
    EXPECT_EQ(0, result.state->purpleId);
    EXPECT_EQ(general, accountData.getChatTargetByPurpleId(parentPurpleId));
    EXPECT_EQ(accountData.getChat(chatId),
              accountData.getChatByPurpleId(parentPurpleId));
}

TEST_F(ForumTopicRegistryTest, AllocatesUniqueIdsAcrossChatsAndTopics)
{
    const ChatId firstChat = ChatId::fromString("-7000");
    const ChatId secondChat = ChatId::fromString("-8000");
    const ChatTarget firstTopic = ChatTarget::forumTopic(
        firstChat, ForumTopicId::fromValue(42));
    const ChatTarget secondTopic = ChatTarget::forumTopic(
        firstChat, ForumTopicId::fromValue(43));
    const ChatTarget sameTopicOtherChat = ChatTarget::forumTopic(
        secondChat, ForumTopicId::fromValue(42));

    addSupergroupChat(firstChat.value(), 700, "First");
    accountData.upsertForumTopic(firstTopic, "One", false, false, 1);
    accountData.upsertForumTopic(secondTopic, "Two", false, false, 1);

    const int firstParentId = accountData.getPurpleChatId(firstChat);
    ASSERT_TRUE(accountData.setForumTopicSaved(firstTopic, true));
    const int firstTopicId = accountData.getPurpleChatId(firstTopic);
    const int secondTopicId = accountData.activateForumTopic(secondTopic);

    addSupergroupChat(secondChat.value(), 800, "Second");
    accountData.upsertForumTopic(sameTopicOtherChat, "One", false, false, 1);
    const int secondParentId = accountData.getPurpleChatId(secondChat);
    const int sameTopicOtherChatId = accountData.activateForumTopic(
        sameTopicOtherChat);

    const int ids[] = {
        firstParentId,
        firstTopicId,
        secondTopicId,
        secondParentId,
        sameTopicOtherChatId
    };
    for (size_t first = 0; first < G_N_ELEMENTS(ids); ++first) {
        ASSERT_GT(ids[first], 0);
        for (size_t second = first + 1; second < G_N_ELEMENTS(ids); ++second)
            EXPECT_NE(ids[first], ids[second]);
    }

    EXPECT_EQ(firstTopic, accountData.getChatTargetByPurpleId(firstTopicId));
    EXPECT_EQ(secondTopic, accountData.getChatTargetByPurpleId(secondTopicId));
    EXPECT_EQ(sameTopicOtherChat,
              accountData.getChatTargetByPurpleId(sameTopicOtherChatId));
    EXPECT_EQ(accountData.getChat(firstChat),
              accountData.getChatByPurpleId(firstTopicId));
}

TEST_F(ForumTopicRegistryTest, DiscoveryDoesNotAllocateOrPersistRooms)
{
    const ChatId firstChat = ChatId::fromString("-7000");
    const ChatId secondChat = ChatId::fromString("-8000");
    const ChatTarget topic = ChatTarget::forumTopic(
        firstChat, ForumTopicId::fromValue(42));

    addSupergroupChat(firstChat.value(), 700, "First");
    TdAccountData::ForumTopicUpsertResult result =
        accountData.upsertForumTopic(topic, "Discovered", false, false, 1);

    ASSERT_NE(nullptr, result.state);
    EXPECT_EQ(0, result.state->purpleId);
    EXPECT_FALSE(result.state->saved);
    EXPECT_FALSE(result.state->active);

    addSupergroupChat(secondChat.value(), 800, "Second");
    EXPECT_EQ(2, accountData.getPurpleChatId(secondChat));
    EXPECT_EQ(3, accountData.activateForumTopic(topic));
}

TEST_F(ForumTopicRegistryTest, UpsertIsIdempotentAndTitlesAreNotIdentity)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget firstTopic = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));
    const ChatTarget secondTopic = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(43));
    addSupergroupChat(chatId.value(), 700, "Group");

    TdAccountData::ForumTopicUpsertResult first =
        accountData.upsertForumTopic(firstTopic, "Duplicate", false, false, 1);
    TdAccountData::ForumTopicUpsertResult second =
        accountData.upsertForumTopic(secondTopic, "Duplicate", false, false, 1);
    accountData.setForumTopicSaved(firstTopic, true);
    const int firstPurpleId = accountData.activateForumTopic(firstTopic);
    const int secondPurpleId = accountData.activateForumTopic(secondTopic);

    TdAccountData::ForumTopicUpsertResult updated =
        accountData.upsertForumTopic(firstTopic, "Renamed", true, false, 2);
    TdAccountData::ForumTopicUpsertResult stale =
        accountData.upsertForumTopic(firstTopic, "Stale", false, true, 1);
    TdAccountData::ForumTopicUpsertResult repeated =
        accountData.upsertForumTopic(firstTopic, "Conflict", false, true, 2);
    std::vector<const TdAccountData::ForumTopicState *> topics;
    accountData.getForumTopics(chatId, topics);

    ASSERT_NE(nullptr, first.state);
    ASSERT_NE(nullptr, second.state);
    ASSERT_EQ(first.state, updated.state);
    EXPECT_TRUE(updated.applied);
    EXPECT_FALSE(stale.applied);
    EXPECT_FALSE(repeated.applied);
    EXPECT_NE(firstPurpleId, secondPurpleId);
    EXPECT_EQ(firstPurpleId, updated.state->purpleId);
    EXPECT_EQ("Renamed", updated.state->name);
    EXPECT_TRUE(updated.state->closed);
    EXPECT_FALSE(updated.state->hidden);
    EXPECT_TRUE(updated.state->saved);
    EXPECT_TRUE(updated.state->active);
    EXPECT_EQ(2U, topics.size());
}

TEST_F(ForumTopicRegistryTest, SavedTopicBeforeParentAllocatesAfterParent)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget topic = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));
    accountData.upsertForumTopic(topic, "Saved", false, false, 1);

    ASSERT_TRUE(accountData.setForumTopicSaved(topic, true));
    EXPECT_EQ(0, accountData.getPurpleChatId(topic));

    addSupergroupChat(chatId.value(), 700, "Group");

    EXPECT_EQ(1, accountData.getPurpleChatId(chatId));
    EXPECT_EQ(2, accountData.getPurpleChatId(topic));
    EXPECT_EQ(topic, accountData.getChatTargetByPurpleId(2));
}

TEST_F(ForumTopicRegistryTest, ForumCapabilityReclassifiesRootWithoutChangingId)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget ordinary = ChatTarget::chat(chatId);
    const ChatTarget general = ChatTarget::forumTopic(
        chatId, ForumTopicId::general());
    addSupergroupChat(chatId.value(), 700, "Group");
    setSupergroupForum(700, false);
    const int purpleId = accountData.getPurpleChatId(chatId);

    EXPECT_EQ(ordinary, accountData.getChatTargetByPurpleId(purpleId));

    accountData.upsertForumTopic(general, "General", false, false, 1);
    setSupergroupForum(700, true);

    EXPECT_EQ(purpleId, accountData.getPurpleChatId(general));
    EXPECT_EQ(general, accountData.getChatTargetByPurpleId(purpleId));
}

TEST_F(ForumTopicRegistryTest, ExpectedRejoinsAreExactTargets)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget general = ChatTarget::forumTopic(
        chatId, ForumTopicId::general());
    const ChatTarget topic = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));

    accountData.addExpectedChat(general);
    accountData.addExpectedChat(topic);
    accountData.addExpectedChat(topic);

    EXPECT_TRUE(accountData.isExpectedChat(general));
    EXPECT_TRUE(accountData.isExpectedChat(topic));
    EXPECT_FALSE(accountData.isExpectedChat(ChatTarget::chat(chatId)));

    accountData.removeExpectedChat(topic);
    EXPECT_TRUE(accountData.isExpectedChat(general));
    EXPECT_FALSE(accountData.isExpectedChat(topic));
}
