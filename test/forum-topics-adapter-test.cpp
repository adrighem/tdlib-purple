#include "forum-topics.h"

#include <gtest/gtest.h>

#include <limits>
#include <string>

using namespace td::td_api;

namespace {

forumTopicInfo makeInfo(int64_t chatId,
                        int32_t topicId,
                        const std::string &name,
                        bool isGeneral,
                        bool closed = false,
                        bool hidden = false)
{
    forumTopicInfo info;
    info.chat_id_ = chatId;
    info.forum_topic_id_ = topicId;
    info.name_ = name;
    info.is_general_ = isGeneral;
    info.is_closed_ = closed;
    info.is_hidden_ = hidden;
    return info;
}

}

TEST(ForumTopicAdapterTest, ConvertsTopicMetadataExactly)
{
    ForumTopicMetadata result;
    const forumTopicInfo info =
        makeInfo(-7000, 42, "Release planning", false, true, false);

    ASSERT_TRUE(adaptForumTopicInfo(info, result));
    EXPECT_EQ(ChatTarget::forumTopic(
                  ChatId::fromString("-7000"),
                  ForumTopicId::fromValue(42)),
              result.target);
    EXPECT_EQ("Release planning", result.name);
    EXPECT_TRUE(result.closed);
    EXPECT_FALSE(result.hidden);
}

TEST(ForumTopicAdapterTest, ConvertsGeneralTopicToInternalIdOne)
{
    ForumTopicMetadata result;
    const forumTopicInfo info =
        makeInfo(-7000, ForumTopicId::general().value(), "General",
                 true, true, true);

    ASSERT_TRUE(adaptForumTopicInfo(info, result));
    EXPECT_EQ(ChatTarget::forumTopic(
                  ChatId::fromString("-7000"),
                  ForumTopicId::general()),
              result.target);
    EXPECT_EQ("General", result.name);
    EXPECT_TRUE(result.closed);
    EXPECT_TRUE(result.hidden);
}

TEST(ForumTopicAdapterTest, AcceptsBoundaryIdentifiers)
{
    ForumTopicMetadata result;
    const std::string chatId =
        std::to_string(std::numeric_limits<int64_t>::max());
    const forumTopicInfo info =
        makeInfo(std::numeric_limits<int64_t>::max(),
                 std::numeric_limits<int32_t>::max(),
                 "", false);

    ASSERT_TRUE(adaptForumTopicInfo(info, result));
    EXPECT_EQ(ChatId::fromString(chatId.c_str()), result.target.chatId());
    EXPECT_EQ(std::numeric_limits<int32_t>::max(),
              result.target.forumTopicId().value());
    EXPECT_TRUE(result.name.empty());
}

TEST(ForumTopicAdapterTest, RejectsInvalidIdentifiers)
{
    const forumTopicInfo invalid[] = {
        makeInfo(0, 42, "Invalid chat", false),
        makeInfo(-7000, 0, "Zero topic", false),
        makeInfo(-7000, -1, "Negative topic", false)
    };

    for (const forumTopicInfo &info : invalid) {
        ForumTopicMetadata result;
        EXPECT_FALSE(adaptForumTopicInfo(info, result));
    }
}

TEST(ForumTopicAdapterTest, RejectsInconsistentGeneralMarker)
{
    ForumTopicMetadata result;

    EXPECT_FALSE(adaptForumTopicInfo(
        makeInfo(-7000, 42, "Marked General", true), result));
    EXPECT_FALSE(adaptForumTopicInfo(
        makeInfo(-7000, ForumTopicId::general().value(),
                 "Unmarked General", false),
        result));
}

TEST(ForumTopicAdapterTest, LeavesResultUnchangedOnFailure)
{
    ForumTopicMetadata result;
    ASSERT_TRUE(adaptForumTopicInfo(
        makeInfo(-7000, 42, "Existing", false, true, false), result));

    EXPECT_FALSE(adaptForumTopicInfo(
        makeInfo(0, 43, "Rejected", false, false, true), result));
    EXPECT_EQ(ChatTarget::forumTopic(
                  ChatId::fromString("-7000"),
                  ForumTopicId::fromValue(42)),
              result.target);
    EXPECT_EQ("Existing", result.name);
    EXPECT_TRUE(result.closed);
    EXPECT_FALSE(result.hidden);
}
