#include "purple-info.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

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
