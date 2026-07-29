#include "account-data.h"
#include "client-utils.h"
#include "purple-info.h"
#include "test-transceiver.h"

#include <gtest/gtest.h>

#include <limits>
#include <set>
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

TEST(ForumTopicIdentityTest, DerivesLegacyTargetWithoutForumTopic)
{
    const int64_t chatIdValue = -7000;
    const ChatId chatId = ChatId::fromString("-7000");
    object_ptr<message> withoutTopic = makeMessage(
        1, 2, chatIdValue, false, 3, makeTextMessage("No topic"));
    object_ptr<message> threadTopic = makeMessage(
        2, 2, chatIdValue, false, 3, makeTextMessage("Thread"),
        make_object<messageTopicThread>(99));

    ASSERT_FALSE(withoutTopic->topic_id_);
    ASSERT_EQ(ChatTarget::chat(chatId), getChatTarget(*withoutTopic));
    ASSERT_EQ(ChatTarget::chat(chatId), getChatTarget(*threadTopic));
}

TEST(ForumTopicIdentityTest, DerivesExactForumTargetIncludingGeneral)
{
    const int64_t chatIdValue = -7000;
    const ChatId chatId = ChatId::fromString("-7000");
    object_ptr<message> general = makeMessage(
        1, 2, chatIdValue, false, 3, makeTextMessage("General"),
        make_object<messageTopicForum>(
            ForumTopicId::general().value()));
    object_ptr<message> child = makeMessage(
        2, 2, chatIdValue, false, 3, makeTextMessage("Child"),
        make_object<messageTopicForum>(42));

    ASSERT_EQ(
        ChatTarget::forumTopic(chatId, ForumTopicId::general()),
        getChatTarget(*general));
    ASSERT_EQ(
        ChatTarget::forumTopic(chatId, ForumTopicId::fromValue(42)),
        getChatTarget(*child));
}

TEST(ForumTopicIdentityTest, RejectsMalformedForumTopicIds)
{
    const int32_t invalidTopicIds[] = {0, -1};
    for (int32_t topicId : invalidTopicIds) {
        object_ptr<message> message = makeMessage(
            1, 2, -7000, false, 3, makeTextMessage("Invalid"),
            make_object<messageTopicForum>(topicId));

        ASSERT_EQ(ChatTarget(), getChatTarget(*message));
        ASSERT_FALSE(getChatTarget(*message).valid());
    }
}

TEST(ForumTopicIdentityTest, NormalizesForumTopicsOutsideSupergroupRooms)
{
    const int64_t chatIdValue = 1000;
    const ChatId chatId = ChatId::fromString("1000");
    object_ptr<message> message = makeMessage(
        1, 2, chatIdValue, false, 3, makeTextMessage("Topic"),
        make_object<messageTopicForum>(42));
    object_ptr<chat> privateChat = makeChat(
        chatIdValue, make_object<chatTypePrivate>(2), "Bot");
    object_ptr<chat> channel = makeChat(
        chatIdValue,
        make_object<chatTypeSupergroup>(700, true),
        "Channel");
    object_ptr<chat> supergroup = makeChat(
        chatIdValue,
        make_object<chatTypeSupergroup>(700, false),
        "Forum");

    EXPECT_EQ(
        ChatTarget::chat(chatId),
        getMessageRoomTarget(*privateChat, *message));
    EXPECT_EQ(
        ChatTarget::chat(chatId),
        getMessageRoomTarget(*channel, *message));
    EXPECT_EQ(
        ChatTarget::forumTopic(
            chatId, ForumTopicId::fromValue(42)),
        getMessageRoomTarget(*supergroup, *message));
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

TEST_F(
    ForumTopicRegistryTest,
    RememberedMessageRoutesAreBoundedPerChat)
{
    const ChatId busyChat =
        ChatId::fromString("-1000");
    const ChatId quietChat =
        ChatId::fromString("-2000");
    accountData.rememberMessageTarget(
        ChatTarget::chat(quietChat),
        MessageId::fromString("1"));

    for (int64_t messageId = 1;
         messageId <= 5000; ++messageId) {
        const std::string value =
            std::to_string(messageId);
        accountData.rememberMessageTarget(
            ChatTarget::chat(busyChat),
            MessageId::fromString(value.c_str()));
    }

    TdAccountData::DisplayedMessageConversation conversation;
    EXPECT_EQ(
        TdAccountData::DisplayedMessageLookupResult::
            UnknownMessage,
        accountData.findDisplayedMessageConversation(
            busyChat, MessageId::fromString("1"),
            conversation));
    EXPECT_EQ(
        TdAccountData::DisplayedMessageLookupResult::
            KnownConversationUnavailable,
        accountData.findDisplayedMessageConversation(
            busyChat, MessageId::fromString("5000"),
            conversation));
    EXPECT_EQ(
        TdAccountData::DisplayedMessageLookupResult::
            KnownConversationUnavailable,
        accountData.findDisplayedMessageConversation(
            quietChat, MessageId::fromString("1"),
            conversation));
}

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

TEST_F(ForumTopicRegistryTest, IncomingUnknownChildPreparesStableInactiveRoom)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget topic = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));
    addSupergroupChat(chatId.value(), 700, "Group");

    const int32_t firstPurpleId =
        accountData.prepareForumTopicForIncomingMessage(topic);
    const TdAccountData::ForumTopicState *firstState =
        accountData.findForumTopic(topic);

    ASSERT_GT(firstPurpleId, 0);
    ASSERT_NE(nullptr, firstState);
    EXPECT_NE(accountData.getPurpleChatId(chatId), firstPurpleId);
    EXPECT_EQ(firstPurpleId, firstState->purpleId);
    EXPECT_FALSE(firstState->active);
    EXPECT_FALSE(firstState->saved);
    EXPECT_FALSE(firstState->deleted);
    EXPECT_FALSE(firstState->metadataKnown);

    const int32_t repeatedPurpleId =
        accountData.prepareForumTopicForIncomingMessage(topic);
    const TdAccountData::ForumTopicState *repeatedState =
        accountData.findForumTopic(topic);

    EXPECT_EQ(firstPurpleId, repeatedPurpleId);
    EXPECT_EQ(firstState, repeatedState);
    EXPECT_FALSE(repeatedState->active);
    EXPECT_EQ(topic, accountData.getChatTargetByPurpleId(firstPurpleId));

    ASSERT_EQ(
        firstPurpleId,
        accountData.activateForumTopic(topic));
    EXPECT_EQ(
        firstPurpleId,
        accountData.prepareForumTopicForIncomingMessage(topic));
    EXPECT_TRUE(repeatedState->active);
}

TEST_F(
    ForumTopicRegistryTest,
    NewerLiveMessageProtectsTopicOnlyFromOlderListings)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget topic = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));
    addSupergroupChat(chatId.value(), 700, "Group");

    const uint64_t olderListingGeneration =
        accountData.reserveForumTopicGeneration();
    ASSERT_GT(
        accountData.prepareForumTopicForIncomingMessage(topic),
        0);

    const TdAccountData::ForumTopicState *state =
        accountData.findForumTopic(topic);
    ASSERT_NE(nullptr, state);
    EXPECT_EQ(0U, state->metadataGeneration);
    EXPECT_GT(
        state->lastLiveMessageGeneration,
        olderListingGeneration);

    EXPECT_TRUE(
        accountData.reconcileForumTopics(
            chatId, std::set<ChatTarget>(),
            olderListingGeneration)
            .empty());
    EXPECT_FALSE(state->active);
    EXPECT_FALSE(state->deleted);

    const uint64_t newerListingGeneration =
        accountData.reserveForumTopicGeneration();
    EXPECT_EQ(
        std::vector<ChatTarget>{topic},
        accountData.reconcileForumTopics(
            chatId, std::set<ChatTarget>(),
            newerListingGeneration));
    EXPECT_FALSE(state->active);
    EXPECT_TRUE(state->deleted);
}

TEST_F(ForumTopicRegistryTest,
       IncomingMessageRevivesTombstonedChildAndInvalidatesMetadata)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget topic = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));
    setSupergroupForum(700, true);
    addSupergroupChat(chatId.value(), 700, "Group");
    TdAccountData::ForumTopicUpsertResult discovered =
        accountData.upsertForumTopic(topic, "Known", false, false, 1);
    const int32_t originalPurpleId = accountData.activateForumTopic(topic);

    ASSERT_NE(nullptr, discovered.state);
    ASSERT_TRUE(discovered.state->metadataKnown);
    ASSERT_GT(originalPurpleId, 0);
    accountData.reconcileForumTopics(
        chatId, std::set<ChatTarget>(), 2);
    ASSERT_TRUE(discovered.state->deleted);
    ASSERT_FALSE(discovered.state->active);

    const int32_t revivedPurpleId =
        accountData.prepareForumTopicForIncomingMessage(topic);
    const TdAccountData::ForumTopicState *revived =
        accountData.findForumTopic(topic);

    ASSERT_NE(nullptr, revived);
    EXPECT_EQ(originalPurpleId, revivedPurpleId);
    EXPECT_EQ(discovered.state, revived);
    EXPECT_FALSE(revived->deleted);
    EXPECT_FALSE(revived->active);
    EXPECT_FALSE(revived->metadataKnown);
    EXPECT_EQ(2U, revived->metadataGeneration);
    ASSERT_NE(nullptr, accountData.getChat(chatId));
    EXPECT_EQ(
        "Group / Topic 42",
        getForumTopicDisplayTitle(
            *accountData.getChat(chatId), *revived));
}

TEST_F(ForumTopicRegistryTest, IncomingGeneralReusesParentPurpleId)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget general = ChatTarget::forumTopic(
        chatId, ForumTopicId::general());
    setSupergroupForum(700, true);
    addSupergroupChat(chatId.value(), 700, "Group");
    const int32_t parentPurpleId = accountData.getPurpleChatId(chatId);

    const int32_t generalPurpleId =
        accountData.prepareForumTopicForIncomingMessage(general);

    ASSERT_GT(parentPurpleId, 0);
    EXPECT_EQ(parentPurpleId, generalPurpleId);
    EXPECT_EQ(general,
              accountData.getChatTargetByPurpleId(generalPurpleId));
}

TEST_F(ForumTopicRegistryTest,
       IncomingPreparationRejectsInvalidAndNonForumTargets)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget ordinary = ChatTarget::chat(chatId);
    const ChatTarget invalidForumTopic = ChatTarget::forumTopic(
        chatId, ForumTopicId::invalid);
    addSupergroupChat(chatId.value(), 700, "Group");

    EXPECT_EQ(0, accountData.prepareForumTopicForIncomingMessage(
                     ChatTarget()));
    EXPECT_EQ(0, accountData.prepareForumTopicForIncomingMessage(
                     ordinary));
    EXPECT_EQ(0, accountData.prepareForumTopicForIncomingMessage(
                     invalidForumTopic));
    EXPECT_EQ(nullptr, accountData.findForumTopic(ordinary));
    EXPECT_EQ(nullptr, accountData.findForumTopic(invalidForumTopic));
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

TEST_F(ForumTopicRegistryTest, ReconciliationTombstonesOnlyMissingOlderTopics)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatId otherChatId = ChatId::fromString("-8000");
    const ChatTarget general = ChatTarget::forumTopic(
        chatId, ForumTopicId::general());
    const ChatTarget seen = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));
    const ChatTarget missing = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(43));
    const ChatTarget newer = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(44));
    const ChatTarget otherChatTopic = ChatTarget::forumTopic(
        otherChatId, ForumTopicId::fromValue(43));
    addSupergroupChat(chatId.value(), 700, "Group");
    addSupergroupChat(otherChatId.value(), 800, "Other");

    accountData.upsertForumTopic(general, "General", false, false, 5);
    accountData.upsertForumTopic(seen, "Seen", false, false, 5);
    accountData.upsertForumTopic(missing, "Missing", false, false, 5);
    accountData.upsertForumTopic(newer, "Live", false, false, 7);
    accountData.upsertForumTopic(
        otherChatTopic, "Other", false, false, 5);
    ASSERT_TRUE(accountData.setForumTopicSaved(missing, true));
    const int32_t missingPurpleId =
        accountData.activateForumTopic(missing);
    ASSERT_GT(missingPurpleId, 0);
    ASSERT_GT(accountData.activateForumTopic(newer), 0);

    const std::vector<ChatTarget> tombstoned =
        accountData.reconcileForumTopics(
            chatId, std::set<ChatTarget>{seen}, 6);

    EXPECT_EQ(std::vector<ChatTarget>{missing}, tombstoned);
    const TdAccountData::ForumTopicState *generalState =
        accountData.findForumTopic(general);
    const TdAccountData::ForumTopicState *seenState =
        accountData.findForumTopic(seen);
    const TdAccountData::ForumTopicState *missingState =
        accountData.findForumTopic(missing);
    const TdAccountData::ForumTopicState *newerState =
        accountData.findForumTopic(newer);
    const TdAccountData::ForumTopicState *otherState =
        accountData.findForumTopic(otherChatTopic);
    ASSERT_NE(nullptr, generalState);
    ASSERT_NE(nullptr, seenState);
    ASSERT_NE(nullptr, missingState);
    ASSERT_NE(nullptr, newerState);
    ASSERT_NE(nullptr, otherState);
    EXPECT_FALSE(generalState->deleted);
    EXPECT_FALSE(seenState->deleted);
    EXPECT_TRUE(missingState->deleted);
    EXPECT_FALSE(missingState->active);
    EXPECT_TRUE(missingState->saved);
    EXPECT_EQ(missingPurpleId, missingState->purpleId);
    EXPECT_EQ(6U, missingState->metadataGeneration);
    EXPECT_FALSE(newerState->deleted);
    EXPECT_TRUE(newerState->active);
    EXPECT_EQ(7U, newerState->metadataGeneration);
    EXPECT_FALSE(otherState->deleted);

    const std::vector<ChatTarget> repeated =
        accountData.reconcileForumTopics(
            chatId, std::set<ChatTarget>{seen}, 7);
    EXPECT_TRUE(repeated.empty());
}

TEST_F(ForumTopicRegistryTest, OnlyNewerMetadataRevivesTombstonedTopic)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget topic = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));
    addSupergroupChat(chatId.value(), 700, "Group");
    accountData.upsertForumTopic(topic, "Before", false, false, 5);
    ASSERT_GT(accountData.activateForumTopic(topic), 0);

    accountData.reconcileForumTopics(chatId, std::set<ChatTarget>(), 6);
    TdAccountData::ForumTopicUpsertResult equal =
        accountData.upsertForumTopic(topic, "Equal", true, true, 6);
    TdAccountData::ForumTopicUpsertResult stale =
        accountData.upsertForumTopic(topic, "Stale", true, true, 5);

    ASSERT_NE(nullptr, equal.state);
    EXPECT_FALSE(equal.applied);
    EXPECT_FALSE(stale.applied);
    EXPECT_TRUE(equal.state->deleted);
    EXPECT_EQ("Before", equal.state->name);

    TdAccountData::ForumTopicUpsertResult revived =
        accountData.upsertForumTopic(topic, "After", true, false, 7);

    ASSERT_NE(nullptr, revived.state);
    EXPECT_TRUE(revived.applied);
    EXPECT_FALSE(revived.state->deleted);
    EXPECT_FALSE(revived.state->active);
    EXPECT_EQ("After", revived.state->name);
    EXPECT_TRUE(revived.state->closed);
    EXPECT_FALSE(revived.state->hidden);
    EXPECT_EQ(7U, revived.state->metadataGeneration);
}

TEST_F(ForumTopicRegistryTest, DeletingChatTombstonesAndDeactivatesItsTopics)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget general = ChatTarget::forumTopic(
        chatId, ForumTopicId::general());
    const ChatTarget topic = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));
    addSupergroupChat(chatId.value(), 700, "Group");
    accountData.upsertForumTopic(general, "General", false, false, 3);
    accountData.upsertForumTopic(topic, "Saved", false, false, 3);
    ASSERT_TRUE(accountData.setForumTopicSaved(topic, true));
    const int32_t purpleId = accountData.activateForumTopic(topic);
    ASSERT_GT(purpleId, 0);
    ASSERT_GT(accountData.activateForumTopic(general), 0);
    const uint64_t listingGeneration =
        accountData.reserveForumTopicGeneration();

    accountData.deleteChat(chatId);

    const TdAccountData::ForumTopicState *generalState =
        accountData.findForumTopic(general);
    const TdAccountData::ForumTopicState *topicState =
        accountData.findForumTopic(topic);
    ASSERT_NE(nullptr, generalState);
    ASSERT_NE(nullptr, topicState);
    EXPECT_EQ(nullptr, accountData.getChat(chatId));
    EXPECT_TRUE(generalState->deleted);
    EXPECT_FALSE(generalState->active);
    EXPECT_TRUE(topicState->deleted);
    EXPECT_FALSE(topicState->active);
    EXPECT_TRUE(topicState->saved);
    EXPECT_EQ(purpleId, topicState->purpleId);

    TdAccountData::ForumTopicUpsertResult oldListing =
        accountData.upsertForumTopic(
            topic, "Old listing", false, false, listingGeneration);
    EXPECT_FALSE(oldListing.applied);
    EXPECT_TRUE(oldListing.state->deleted);

    const uint64_t revivalGeneration =
        accountData.reserveForumTopicGeneration();
    TdAccountData::ForumTopicUpsertResult revived =
        accountData.upsertForumTopic(
            topic, "Revived", false, false, revivalGeneration);
    ASSERT_TRUE(revived.applied);
    EXPECT_FALSE(revived.state->deleted);
    EXPECT_FALSE(revived.state->active);
    EXPECT_TRUE(revived.state->saved);
    EXPECT_EQ(purpleId, revived.state->purpleId);
}

TEST_F(ForumTopicRegistryTest, DeletedSavedTopicAllocatesOnlyAfterRevival)
{
    const ChatId chatId = ChatId::fromString("-7000");
    const ChatTarget topic = ChatTarget::forumTopic(
        chatId, ForumTopicId::fromValue(42));
    accountData.upsertForumTopic(topic, "Saved", false, false, 1);
    ASSERT_TRUE(accountData.setForumTopicSaved(topic, true));
    ASSERT_EQ(0, accountData.getPurpleChatId(topic));
    const uint64_t listingGeneration =
        accountData.reserveForumTopicGeneration();

    accountData.deleteChat(chatId);
    addSupergroupChat(chatId.value(), 700, "Group");

    const TdAccountData::ForumTopicState *deleted =
        accountData.findForumTopic(topic);
    ASSERT_NE(nullptr, deleted);
    EXPECT_TRUE(deleted->deleted);
    EXPECT_EQ(0, deleted->purpleId);
    EXPECT_FALSE(accountData.upsertForumTopic(
        topic, "Old listing", false, false, listingGeneration).applied);
    EXPECT_EQ(0, accountData.getPurpleChatId(topic));

    const uint64_t revivalGeneration =
        accountData.reserveForumTopicGeneration();
    TdAccountData::ForumTopicUpsertResult revived =
        accountData.upsertForumTopic(
            topic, "Revived", false, false, revivalGeneration);

    ASSERT_TRUE(revived.applied);
    EXPECT_FALSE(revived.state->deleted);
    EXPECT_TRUE(revived.state->saved);
    EXPECT_GT(revived.state->purpleId, 0);
    EXPECT_EQ(revived.state->purpleId, accountData.getPurpleChatId(topic));
}
