#include "account-data.h"
#include "libpurple-mock.h"
#include "purple-info.h"
#include "supergroup-test.h"
#include "td-client.h"

#include <cstdint>
#include <cstdio>
#include <fmt/format.h>
#include <gtest/gtest.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

using namespace td::td_api;

namespace {

constexpr int32_t TopicId = 42;
constexpr int64_t PendingMessageId = 10;
constexpr int64_t FailedMessageId = 20;
constexpr int32_t FailureDate = 12345;
const char *const NotificationWho = " ";

gboolean receiptConversationFocused(PurpleConversation *)
{
    return TRUE;
}

gboolean receiptConversationUnfocused(PurpleConversation *)
{
    return FALSE;
}

PurpleConversationUiOps *receiptUiOps(bool focused)
{
    static PurpleConversationUiOps focusedOps = {};
    static PurpleConversationUiOps unfocusedOps = {};
    focusedOps.has_focus = receiptConversationFocused;
    unfocusedOps.has_focus = receiptConversationUnfocused;
    return focused ? &focusedOps : &unfocusedOps;
}

class TempFileCleanup {
public:
    explicit TempFileCleanup(std::string path)
        : m_path(std::move(path))
    {}

    ~TempFileCleanup()
    {
        if (!m_path.empty())
            std::remove(m_path.c_str());
    }

private:
    std::string m_path;
};

object_ptr<sendMessage> expectedTextSend(
    int64_t chatId, object_ptr<MessageTopic> topic,
    const std::string &text)
{
    // Purple 2.x doesn't expose an outgoing reply target, so reply_to stays
    // null for every send path covered here.
    return Mock_SendMessage(
        chatId, std::move(topic), nullptr, nullptr,
        Mock_InputMessageText(make_object<formattedText>(
            text, std::vector<object_ptr<textEntity>>())));
}

std::string createTestTempFile()
{
    gchar *path = nullptr;
    GError *error = nullptr;
    const int fd = g_file_open_tmp(
        "tdlib-purple-topic-send-XXXXXX", &path, &error);
    if (fd < 0) {
        ADD_FAILURE() << "Failed to create test temporary file: "
                      << (error ? error->message : "unknown error");
    } else {
        ::close(fd);
    }
    if (error)
        g_error_free(error);

    const std::string result = path ? path : "";
    g_free(path);
    return result;
}

class ForumTopicSendingTest : public SupergroupTest {
protected:
    void loginWithForumSupergroup()
    {
        loginWithSupergroup();
        tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
            groupId, make_object<chatMemberStatusMember>(), 2)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    ChatTarget generalTarget() const
    {
        return ChatTarget::forumTopic(
            ChatId::fromString(std::to_string(groupChatId).c_str()),
            ForumTopicId::general());
    }

    ChatTarget topicTarget() const
    {
        return ChatTarget::forumTopic(
            ChatId::fromString(std::to_string(groupChatId).c_str()),
            ForumTopicId::fromValue(TopicId));
    }

    std::string topicPurpleName() const
    {
        return getPurpleChatName(topicTarget());
    }

    void cacheTopic(bool closed = false)
    {
        tgl.update(make_object<updateForumTopicInfo>(
            makeForumTopicInfo(
                groupChatId, TopicId, "Support",
                false, closed)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    int32_t openTarget(ChatTarget target)
    {
        GHashTable *components = getChatComponents(target);
        pluginInfo().join_chat(connection, components);
        g_hash_table_destroy(components);
        tgl.verifyNoRequests();
        prpl.discardEvents();

        const std::string name = getPurpleChatName(target);
        PurpleConversation *conversation =
            purple_find_conversation_with_account(
                PURPLE_CONV_TYPE_CHAT, name.c_str(), account);
        EXPECT_NE(nullptr, conversation);
        if (!conversation)
            return 0;
        PurpleConvChat *chat =
            purple_conversation_get_chat_data(conversation);
        EXPECT_NE(nullptr, chat);
        return chat ? purple_conv_chat_get_id(chat) : 0;
    }

    int32_t openGeneral()
    {
        return openTarget(generalTarget());
    }

    int32_t openTopic(bool closed = false)
    {
        cacheTopic(closed);
        return openTarget(topicTarget());
    }

    PurpleConversation *topicConversation() const
    {
        return purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    }

    PurpleConversation *targetConversation(
        ChatTarget target) const
    {
        const std::string name = getPurpleChatName(target);
        return purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT, name.c_str(), account);
    }

    void setTargetFocused(ChatTarget target, bool focused)
    {
        PurpleConversation *conversation =
            targetConversation(target);
        ASSERT_NE(nullptr, conversation);
        conversation->ui_ops = receiptUiOps(focused);
    }

    void flushTargetReadReceipts(ChatTarget target)
    {
        PurpleConversation *conversation =
            targetConversation(target);
        ASSERT_NE(nullptr, conversation);
        PurpleTdClient *client = getTdClient(account);
        ASSERT_NE(nullptr, client);
        client->sendReadReceipts(conversation);
    }

    void queuePendingOutgoingReadReceipt(
        int32_t purpleId, const std::string &text)
    {
        setTargetFocused(topicTarget(), false);
        ASSERT_EQ(
            0, pluginInfo().chat_send(
                   connection, purpleId, text.c_str(),
                   PURPLE_MESSAGE_SEND));
        const uint64_t requestId =
            tgl.verifyRequest(expectedTextSend(
                groupChatId,
                make_object<messageTopicForum>(TopicId),
                text));

        tgl.reply(requestId, makeMessage(
            PendingMessageId, selfId, groupChatId, true, 1,
            makeTextMessage(text),
            make_object<messageTopicForum>(TopicId)));
        prpl.verifyNoEvents();

        tgl.update(make_object<updateNewMessage>(makeMessage(
            PendingMessageId, selfId, groupChatId, true, 1,
            makeTextMessage(text),
            make_object<messageTopicForum>(TopicId))));
        tgl.verifyNoRequests();
        prpl.verifyEvents(ConversationWriteEvent(
            topicPurpleName(),
            selfFirstName + " " + selfLastName,
            text, PURPLE_MESSAGE_SEND, 1));
    }

    void deleteTopicAuthoritatively()
    {
        PurpleRoomlist *roomlist =
            pluginInfo().roomlist_get_list(connection);
        ASSERT_NE(nullptr, roomlist);
        prpl.discardEvents();

        const uint64_t requestId = tgl.verifyRequest(
            getForumTopics(groupChatId, "", 0, 0, 0, 100));
        tgl.reply(requestId, makeForumTopicsPage(
            0, std::vector<object_ptr<forumTopic>>()));
        prpl.discardEvents();
        tgl.verifyNoRequests();
        purple_roomlist_unref(roomlist);
    }

    void expectChildSendRejected(int32_t purpleId)
    {
        EXPECT_LT(
            pluginInfo().chat_send(
                connection, purpleId, "must not reach General",
                PURPLE_MESSAGE_SEND),
            0);
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }
};

TEST_F(ForumTopicSendingTest, GeneralSendUsesExplicitGeneralTopic)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openGeneral();
    ASSERT_GT(purpleId, 0);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "general",
               PURPLE_MESSAGE_SEND));

    tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(
            ForumTopicId::general().value()),
        "general"));
    prpl.verifyNoEvents();
}

TEST_F(ForumTopicSendingTest, ActiveChildSendUsesExactTopic)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "child",
               PURPLE_MESSAGE_SEND));

    tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        "child"));
    prpl.verifyNoEvents();
}

TEST_F(
    ForumTopicSendingTest,
    ChildSendRejectsCollidingBaseConversationId)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    serv_got_joined_chat(
        connection, purpleId,
        groupChatPurpleName.c_str());
    prpl.discardEvents();

    expectChildSendRejected(purpleId);
}

TEST_F(ForumTopicSendingTest, TelegramClosedChildRemainsSendable)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic(true);
    ASSERT_GT(purpleId, 0);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "admin may send",
               PURPLE_MESSAGE_SEND));

    tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        "admin may send"));
    prpl.verifyNoEvents();
}

TEST_F(
    ForumTopicSendingTest,
    EverySplitAndInlineImagePartKeepsExactChildTopic)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    tgl.update(make_object<td::td_api::updateOption>(
        "message_text_length_max",
        make_object<optionValueInteger>(9)));

    uint8_t imageData[] = {1, 2, 3, 4, 5};
    const int imageId = purple_imgstore_add_with_id(
        arrayDup(imageData, sizeof(imageData)),
        sizeof(imageData), "topic-image");
    const std::string message = fmt::format(
        "123456789ABCDEFGHI<img id=\"{}\">caption",
        imageId);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, message.c_str(),
               PURPLE_MESSAGE_SEND));

    tgl.verifyRequestsV(
        expectedTextSend(
            groupChatId,
            make_object<messageTopicForum>(TopicId),
            "123456789"),
        expectedTextSend(
            groupChatId,
            make_object<messageTopicForum>(TopicId),
            "ABCDEFGHI"),
        Mock_SendMessage(
            groupChatId,
            make_object<messageTopicForum>(TopicId),
            nullptr, nullptr,
            Mock_InputMessagePhoto(
                make_object<inputFileLocal>(),
                nullptr, std::vector<int32_t>(), 0, 0,
                make_object<formattedText>(
                    "caption",
                    std::vector<object_ptr<textEntity>>()))));

    ASSERT_FALSE(tgl.getInputPhotoPath(0).empty());
    TempFileCleanup cleanup(tgl.getInputPhotoPath(0));
    EXPECT_TRUE(g_file_test(
        tgl.getInputPhotoPath(0).c_str(),
        G_FILE_TEST_EXISTS));
    prpl.verifyNoEvents();
}

TEST_F(
    ForumTopicSendingTest,
    ImmediateInlineImageFailureStaysInChildAndRemovesTempFile)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    uint8_t imageData[] = {1, 2, 3, 4, 5};
    const int imageId = purple_imgstore_add_with_id(
        arrayDup(imageData, sizeof(imageData)),
        sizeof(imageData), "topic-image");
    const std::string message = fmt::format(
        "<img id=\"{}\">caption", imageId);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, message.c_str(),
               PURPLE_MESSAGE_SEND));
    const uint64_t requestId = tgl.verifyRequest(
        Mock_SendMessage(
            groupChatId,
            make_object<messageTopicForum>(TopicId),
            nullptr, nullptr,
            Mock_InputMessagePhoto(
                make_object<inputFileLocal>(),
                nullptr, std::vector<int32_t>(), 0, 0,
                make_object<formattedText>(
                    "caption",
                    std::vector<object_ptr<textEntity>>()))));

    ASSERT_FALSE(tgl.getInputPhotoPath(0).empty());
    const std::string tempPath = tgl.getInputPhotoPath(0);
    TempFileCleanup cleanup(tempPath);
    ASSERT_TRUE(g_file_test(
        tempPath.c_str(), G_FILE_TEST_EXISTS));

    tgl.reply(
        requestId,
        make_object<error>(100, "image rejected"));

    EXPECT_FALSE(g_file_test(
        tempPath.c_str(), G_FILE_TEST_EXISTS));
    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), NotificationWho,
        "Failed to send message: code 100 (image rejected)",
        PURPLE_MESSAGE_SYSTEM, 0));
}

TEST_F(
    ForumTopicSendingTest,
    ImmediateTextFailureUsesRequestedChildWhenGeneralIsOpen)
{
    loginWithForumSupergroup();
    ASSERT_GT(openGeneral(), 0);
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "fails immediately",
               PURPLE_MESSAGE_SEND));
    const uint64_t requestId = tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        "fails immediately"));

    tgl.reply(
        requestId,
        make_object<error>(100, "immediate rejection"));

    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), NotificationWho,
        "Failed to send message: code 100 (immediate rejection)",
        PURPLE_MESSAGE_SYSTEM, 0));
}

TEST_F(
    ForumTopicSendingTest,
    ImmediateFailureAfterTopicDeletionUsesExistingLeftChild)
{
    loginWithForumSupergroup();
    ASSERT_GT(openGeneral(), 0);
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    PurpleConversation *conversation = topicConversation();
    ASSERT_NE(nullptr, conversation);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "deleted while sending",
               PURPLE_MESSAGE_SEND));
    const uint64_t requestId = tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        "deleted while sending"));

    deleteTopicAuthoritatively();
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));
    ASSERT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));

    tgl.reply(
        requestId,
        make_object<error>(100, "deleted topic rejection"));

    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), NotificationWho,
        "Failed to send message: code 100 "
        "(deleted topic rejection)",
        PURPLE_MESSAGE_SYSTEM, 0));
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(
    ForumTopicSendingTest,
    ImmediateFailureAfterExactChildWasDestroyedDoesNotFallback)
{
    loginWithForumSupergroup();
    ASSERT_GT(openGeneral(), 0);
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    PurpleConversation *conversation = topicConversation();
    ASSERT_NE(nullptr, conversation);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "closed while sending",
               PURPLE_MESSAGE_SEND));
    const uint64_t requestId = tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        "closed while sending"));

    purple_conversation_destroy(conversation);
    prpl.verifyNoEvents();
    ASSERT_EQ(nullptr, topicConversation());
    ASSERT_NE(nullptr, targetConversation(generalTarget()));

    tgl.reply(
        requestId,
        make_object<error>(100, "closed topic rejection"));

    prpl.verifyNoEvents();
    EXPECT_EQ(nullptr, topicConversation());
    EXPECT_NE(nullptr, targetConversation(generalTarget()));
}

TEST_F(
    ForumTopicSendingTest,
    PendingInlineImageTempFileIsRemovedAtDisconnect)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    uint8_t imageData[] = {1, 2, 3, 4, 5};
    const int imageId = purple_imgstore_add_with_id(
        arrayDup(imageData, sizeof(imageData)),
        sizeof(imageData), "topic-image");
    const std::string message = fmt::format(
        "<img id=\"{}\">caption", imageId);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, message.c_str(),
               PURPLE_MESSAGE_SEND));
    tgl.verifyRequest(Mock_SendMessage(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        nullptr, nullptr,
        Mock_InputMessagePhoto(
            make_object<inputFileLocal>(),
            nullptr, std::vector<int32_t>(), 0, 0,
            make_object<formattedText>(
                "caption",
                std::vector<object_ptr<textEntity>>()))));

    ASSERT_FALSE(tgl.getInputPhotoPath(0).empty());
    const std::string tempPath = tgl.getInputPhotoPath(0);
    TempFileCleanup cleanup(tempPath);
    ASSERT_TRUE(g_file_test(
        tempPath.c_str(), G_FILE_TEST_EXISTS));

    pluginInfo().close(connection);

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    EXPECT_FALSE(g_file_test(
        tempPath.c_str(), G_FILE_TEST_EXISTS));
    prpl.verifyNoEvents();
}

TEST_F(
    ForumTopicSendingTest,
    DeletedPendingInlineImageRemovesTempFileWithoutTopicFallback)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    uint8_t imageData[] = {1, 2, 3, 4, 5};
    const int imageId = purple_imgstore_add_with_id(
        arrayDup(imageData, sizeof(imageData)),
        sizeof(imageData), "topic-image");
    const std::string message = fmt::format(
        "<img id=\"{}\">caption", imageId);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, message.c_str(),
               PURPLE_MESSAGE_SEND));
    const uint64_t requestId = tgl.verifyRequest(
        Mock_SendMessage(
            groupChatId,
            make_object<messageTopicForum>(TopicId),
            nullptr, nullptr,
            Mock_InputMessagePhoto(
                make_object<inputFileLocal>(),
                nullptr, std::vector<int32_t>(), 0, 0,
                make_object<formattedText>(
                    "caption",
                    std::vector<object_ptr<textEntity>>()))));

    ASSERT_FALSE(tgl.getInputPhotoPath(0).empty());
    const std::string tempPath = tgl.getInputPhotoPath(0);
    TempFileCleanup cleanup(tempPath);
    ASSERT_TRUE(g_file_test(
        tempPath.c_str(), G_FILE_TEST_EXISTS));

    constexpr int32_t FileId = 123;
    tgl.reply(requestId, makeMessage(
        PendingMessageId, selfId, groupChatId, true, 1,
        makeMessagePhoto(
            makePhotoUploading(
                FileId, sizeof(imageData), 0,
                tempPath, 0, 0),
            make_object<formattedText>(
                "caption",
                std::vector<object_ptr<textEntity>>()),
            false),
        make_object<messageTopicForum>(TopicId)));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateDeleteMessages>(
        groupChatId,
        std::vector<int64_t>{PendingMessageId},
        false, false));

    EXPECT_FALSE(g_file_test(
        tempPath.c_str(), G_FILE_TEST_EXISTS));
    prpl.verifyNoEvents();
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            groupChatPurpleName.c_str(), account));
}

TEST_F(ForumTopicSendingTest, AsyncSendFailureStaysInExactChild)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "fails later",
               PURPLE_MESSAGE_SEND));
    const uint64_t requestId = tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        "fails later"));

    tgl.reply(requestId, makeMessage(
        PendingMessageId, selfId, groupChatId, true, 1,
        makeTextMessage("fails later"),
        make_object<messageTopicForum>(TopicId)));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateMessageSendFailed>(
        makeMessage(
            FailedMessageId, selfId, groupChatId, true,
            FailureDate, makeTextMessage("fails later"),
            make_object<messageTopicForum>(TopicId)),
        PendingMessageId,
        make_object<error>(100, "later rejection")));

    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), NotificationWho,
        "Failed to send message: code 100 (later rejection)",
        PURPLE_MESSAGE_SYSTEM, 0));
}

TEST_F(
    ForumTopicSendingTest,
    AsyncFailureAfterForumDisableUsesExistingLeftChild)
{
    loginWithForumSupergroup();
    ASSERT_GT(openGeneral(), 0);
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    PurpleConversation *conversation = topicConversation();
    ASSERT_NE(nullptr, conversation);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "forum disabled later",
               PURPLE_MESSAGE_SEND));
    const uint64_t requestId = tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        "forum disabled later"));
    tgl.reply(requestId, makeMessage(
        PendingMessageId, selfId, groupChatId, true, 1,
        makeTextMessage("forum disabled later"),
        make_object<messageTopicForum>(TopicId)));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateSupergroup>(makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));
    ASSERT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));

    tgl.update(make_object<updateMessageSendFailed>(
        makeMessage(
            FailedMessageId, selfId, groupChatId, true,
            FailureDate, makeTextMessage("forum disabled later"),
            make_object<messageTopicForum>(TopicId)),
        PendingMessageId,
        make_object<error>(100, "forum disabled rejection")));

    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), NotificationWho,
        "Failed to send message: code 100 "
        "(forum disabled rejection)",
        PURPLE_MESSAGE_SYSTEM, 0));
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(
    ForumTopicSendingTest,
    AsyncFailureAfterMembershipLossUsesExistingLeftChild)
{
    loginWithForumSupergroup();
    ASSERT_GT(openGeneral(), 0);
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    PurpleConversation *conversation = topicConversation();
    ASSERT_NE(nullptr, conversation);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "membership lost later",
               PURPLE_MESSAGE_SEND));
    const uint64_t requestId = tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        "membership lost later"));
    tgl.reply(requestId, makeMessage(
        PendingMessageId, selfId, groupChatId, true, 1,
        makeTextMessage("membership lost later"),
        make_object<messageTopicForum>(TopicId)));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateSupergroup>(makeForumSupergroup(
        groupId, make_object<chatMemberStatusLeft>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));
    ASSERT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));

    tgl.update(make_object<updateMessageSendFailed>(
        makeMessage(
            FailedMessageId, selfId, groupChatId, true,
            FailureDate, makeTextMessage("membership lost later"),
            make_object<messageTopicForum>(TopicId)),
        PendingMessageId,
        make_object<error>(100, "membership lost rejection")));

    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), NotificationWho,
        "Failed to send message: code 100 "
        "(membership lost rejection)",
        PURPLE_MESSAGE_SYSTEM, 0));
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
}

TEST_F(
    ForumTopicSendingTest,
    PendingReadReceiptUsesFinalMessageIdInSameTopic)
{
    constexpr int64_t FinalMessageId = 11;

    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    queuePendingOutgoingReadReceipt(
        purpleId, "queued receipt");

    object_ptr<message> finalMessage = makeMessage(
        FinalMessageId, selfId, groupChatId, true, 2,
        makeTextMessage("queued receipt"),
        make_object<messageTopicForum>(TopicId));
    finalMessage->sending_state_ = nullptr;
    tgl.update(make_object<updateMessageSendSucceeded>(
        std::move(finalMessage), PendingMessageId));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    setTargetFocused(topicTarget(), true);
    flushTargetReadReceipts(topicTarget());
    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId, {FinalMessageId}, true,
        make_object<messageSourceForumTopicHistory>()));
    tgl.verifyNoRequests();
}

TEST_F(
    ForumTopicSendingTest,
    SentPendingReceiptIsNotRecreatedForFinalMessageId)
{
    constexpr int64_t FinalMessageId = 11;

    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    queuePendingOutgoingReadReceipt(
        purpleId, "already read");

    setTargetFocused(topicTarget(), true);
    flushTargetReadReceipts(topicTarget());
    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId, {PendingMessageId}, true,
        make_object<messageSourceForumTopicHistory>()));
    tgl.verifyNoRequests();

    object_ptr<message> finalMessage = makeMessage(
        FinalMessageId, selfId, groupChatId, true, 2,
        makeTextMessage("already read"),
        make_object<messageTopicForum>(TopicId));
    finalMessage->sending_state_ = nullptr;
    tgl.update(make_object<updateMessageSendSucceeded>(
        std::move(finalMessage), PendingMessageId));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    setTargetFocused(topicTarget(), false);
    object_ptr<message> finalEcho = makeMessage(
        FinalMessageId, selfId, groupChatId, true, 3,
        makeTextMessage("already read"),
        make_object<messageTopicForum>(TopicId));
    finalEcho->sending_state_ = nullptr;
    tgl.update(make_object<updateNewMessage>(
        std::move(finalEcho)));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(),
        selfFirstName + " " + selfLastName,
        "already read",
        static_cast<PurpleMessageFlags>(
            PURPLE_MESSAGE_SEND |
            PURPLE_MESSAGE_REMOTE_SEND),
        3));

    setTargetFocused(topicTarget(), true);
    flushTargetReadReceipts(topicTarget());
    tgl.verifyNoRequests();
}

TEST_F(
    ForumTopicSendingTest,
    PendingReadReceiptIsDiscardedWhenFinalTopicChanges)
{
    constexpr int64_t FinalMessageId = 11;

    loginWithForumSupergroup();
    ASSERT_GT(openGeneral(), 0);
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    queuePendingOutgoingReadReceipt(
        purpleId, "changed topic");

    object_ptr<message> finalMessage = makeMessage(
        FinalMessageId, selfId, groupChatId, true, 2,
        makeTextMessage("changed topic"),
        make_object<messageTopicForum>(
            ForumTopicId::general().value()));
    finalMessage->sending_state_ = nullptr;
    tgl.update(make_object<updateMessageSendSucceeded>(
        std::move(finalMessage), PendingMessageId));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    setTargetFocused(topicTarget(), true);
    flushTargetReadReceipts(topicTarget());
    tgl.verifyNoRequests();

    setTargetFocused(generalTarget(), true);
    flushTargetReadReceipts(generalTarget());
    tgl.verifyNoRequests();
}

TEST_F(
    ForumTopicSendingTest,
    ResponseAndFinalTargetMismatchKeepRequestedChildAsFailureOrigin)
{
    loginWithForumSupergroup();
    ASSERT_GT(openGeneral(), 0);
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "mismatched response",
               PURPLE_MESSAGE_SEND));
    const uint64_t requestId = tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        "mismatched response"));

    tgl.reply(requestId, makeMessage(
        PendingMessageId, selfId, groupChatId, true, 1,
        makeTextMessage("mismatched response"),
        make_object<messageTopicForum>(
            ForumTopicId::general().value())));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateMessageSendFailed>(
        makeMessage(
            FailedMessageId, selfId, groupChatId, true,
            FailureDate, makeTextMessage("mismatched response"),
            make_object<messageTopicForum>(
                ForumTopicId::general().value())),
        PendingMessageId,
        make_object<error>(100, "later rejection")));

    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), NotificationWho,
        "Failed to send message: code 100 (later rejection)",
        PURPLE_MESSAGE_SYSTEM, 0));
}

TEST_F(
    ForumTopicSendingTest,
    FinalChatMismatchKeepsOriginAndRemovesInlineTempFile)
{
    constexpr int64_t OtherChatId = -8000;

    loginWithForumSupergroup();
    ASSERT_GT(openGeneral(), 0);
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    uint8_t imageData[] = {1, 2, 3, 4, 5};
    const int imageId = purple_imgstore_add_with_id(
        arrayDup(imageData, sizeof(imageData)),
        sizeof(imageData), "topic-image");
    const std::string message = fmt::format(
        "<img id=\"{}\">caption", imageId);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, message.c_str(),
               PURPLE_MESSAGE_SEND));
    const uint64_t requestId = tgl.verifyRequest(
        Mock_SendMessage(
            groupChatId,
            make_object<messageTopicForum>(TopicId),
            nullptr, nullptr,
            Mock_InputMessagePhoto(
                make_object<inputFileLocal>(),
                nullptr, std::vector<int32_t>(), 0, 0,
                make_object<formattedText>(
                    "caption",
                    std::vector<object_ptr<textEntity>>()))));

    ASSERT_FALSE(tgl.getInputPhotoPath(0).empty());
    const std::string tempPath = tgl.getInputPhotoPath(0);
    TempFileCleanup cleanup(tempPath);
    ASSERT_TRUE(g_file_test(
        tempPath.c_str(), G_FILE_TEST_EXISTS));

    tgl.reply(requestId, makeMessage(
        PendingMessageId, selfId, groupChatId, true, 1,
        makeMessagePhoto(
            makePhotoUploading(
                123, sizeof(imageData), 0,
                tempPath, 0, 0),
            make_object<formattedText>(
                "caption",
                std::vector<object_ptr<textEntity>>()),
            false),
        make_object<messageTopicForum>(TopicId)));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateMessageSendFailed>(
        makeMessage(
            FailedMessageId, selfId, OtherChatId, true,
            FailureDate,
            makeMessagePhoto(
                makePhotoUploading(
                    123, sizeof(imageData), 0,
                    tempPath, 0, 0),
                make_object<formattedText>(
                    "caption",
                    std::vector<object_ptr<textEntity>>()),
                false)),
        PendingMessageId,
        make_object<error>(100, "moved rejection")));

    EXPECT_FALSE(g_file_test(
        tempPath.c_str(), G_FILE_TEST_EXISTS));
    prpl.verifyEvents(ConversationWriteEvent(
        topicPurpleName(), NotificationWho,
        "Failed to send message: code 100 (moved rejection)",
        PURPLE_MESSAGE_SYSTEM, 0));
}

TEST_F(
    ForumTopicSendingTest,
    ActiveProvisionalChildWithoutMetadataSendsToExactTopic)
{
    constexpr int64_t IncomingMessageId = 10000;

    loginWithForumSupergroup();
    tgl.update(make_object<updateNewMessage>(makeMessage(
        IncomingMessageId, userIds[0], groupChatId, false, 12345,
        makeTextMessage("Before metadata"),
        make_object<messageTopicForum>(TopicId))));

    const uint64_t metadataRequest = tgl.verifyRequest(
        getForumTopic(groupChatId, TopicId));
    tgl.verifyRequest(*Mock_ViewMessages(
        groupChatId, {IncomingMessageId}, true,
        make_object<messageSourceForumTopicHistory>()));
    tgl.verifyNoRequests();
    prpl.discardEvents();

    PurpleConversation *conversation = topicConversation();
    ASSERT_NE(nullptr, conversation);
    PurpleConvChat *chat =
        purple_conversation_get_chat_data(conversation);
    ASSERT_NE(nullptr, chat);
    const int32_t purpleId = purple_conv_chat_get_id(chat);
    ASSERT_GT(purpleId, 0);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "while metadata is pending",
               PURPLE_MESSAGE_SEND));
    tgl.verifyRequest(expectedTextSend(
        groupChatId,
        make_object<messageTopicForum>(TopicId),
        "while metadata is pending"));
    prpl.verifyNoEvents();

    tgl.reply(metadataRequest, makeForumTopic(makeForumTopicInfo(
        groupChatId, TopicId, "Resolved")));
    prpl.discardEvents();
}

TEST_F(ForumTopicSendingTest, OrdinaryGroupSendKeepsNullTopic)
{
    loginWithSupergroup();
    const int32_t purpleId = openTarget(ChatTarget::chat(
        ChatId::fromString(
            std::to_string(groupChatId).c_str())));
    ASSERT_GT(purpleId, 0);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "ordinary",
               PURPLE_MESSAGE_SEND));

    tgl.verifyRequest(expectedTextSend(
        groupChatId, nullptr, "ordinary"));
    prpl.verifyNoEvents();
}

TEST_F(
    ForumTopicSendingTest,
    ChannelMarkedAsForumKeepsLegacyNullTopic)
{
    loginWithSupergroup();

    object_ptr<supergroup> channel = makeSupergroup(
        groupId, make_object<chatMemberStatusMember>(), 2, true);
    channel->is_forum_ = true;
    tgl.update(make_object<updateSupergroup>(std::move(channel)));

    object_ptr<chat> channelChat = makeChat(
        groupChatId,
        make_object<chatTypeSupergroup>(groupId, true),
        groupChatTitle);
    addChatPosition(
        channelChat, make_object<chatListMain>());
    tgl.update(make_object<updateNewChat>(
        std::move(channelChat)));
    tgl.verifyNoRequests();
    prpl.discardEvents();

    const int32_t purpleId = openTarget(ChatTarget::chat(
        ChatId::fromString(
            std::to_string(groupChatId).c_str())));
    ASSERT_GT(purpleId, 0);

    ASSERT_EQ(
        0, pluginInfo().chat_send(
               connection, purpleId, "channel compatibility",
               PURPLE_MESSAGE_SEND));
    tgl.verifyRequest(expectedTextSend(
        groupChatId, nullptr, "channel compatibility"));
    prpl.verifyNoEvents();
}

TEST_F(ForumTopicSendingTest, PrivateSendKeepsNullTopic)
{
    loginWithOneContact();

    ASSERT_EQ(
        0, pluginInfo().send_im(
               connection, purpleUserName(0).c_str(),
               "private", PURPLE_MESSAGE_SEND));

    tgl.verifyRequest(expectedTextSend(
        chatIds[0], nullptr, "private"));
    prpl.verifyNoEvents();
}

TEST_F(ForumTopicSendingTest, UnknownChildConversationIdFailsClosed)
{
    loginWithForumSupergroup();
    constexpr int32_t StalePurpleId = 77;
    serv_got_joined_chat(
        connection, StalePurpleId,
        topicPurpleName().c_str());
    prpl.discardEvents();

    expectChildSendRejected(StalePurpleId);
}

TEST_F(ForumTopicSendingTest, InactiveChildFailsClosed)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    PurpleConversation *conversation = topicConversation();
    ASSERT_NE(nullptr, conversation);

    purple_conversation_destroy(conversation);
    prpl.verifyNoEvents();

    expectChildSendRejected(purpleId);
}

TEST_F(ForumTopicSendingTest, DeletedChildFailsClosed)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    PurpleConversation *conversation = topicConversation();
    ASSERT_NE(nullptr, conversation);

    deleteTopicAuthoritatively();

    ASSERT_NE(nullptr, purple_conversation_get_chat_data(conversation));
    EXPECT_TRUE(purple_conv_chat_has_left(
        purple_conversation_get_chat_data(conversation)));
    expectChildSendRejected(purpleId);
}

TEST_F(
    ForumTopicSendingTest,
    PendingTempFilesWithSameMessageIdAreScopedByChatAndOwned)
{
    const std::string firstPath = createTestTempFile();
    ASSERT_FALSE(firstPath.empty());
    TempFileCleanup firstCleanup(firstPath);
    const std::string secondPath = createTestTempFile();
    ASSERT_FALSE(secondPath.empty());
    TempFileCleanup secondCleanup(secondPath);

    const ChatId firstChat = ChatId::fromString("1000");
    const ChatId secondChat = ChatId::fromString("1001");
    const MessageId pending = MessageId::fromString("10");
    ASSERT_TRUE(firstChat.valid());
    ASSERT_TRUE(secondChat.valid());
    ASSERT_TRUE(pending.valid());

    TestTransceiver backend;
    TdTransceiver transceiver(
        nullptr, nullptr, nullptr, &backend);
    {
        TdAccountData accountData(account, transceiver);
        accountData.addPendingSend(
            ChatTarget::chat(firstChat), pending, firstPath);
        accountData.addPendingSend(
            ChatTarget::chat(secondChat), pending, secondPath);

        TdAccountData::PendingSendInfo extracted;
        EXPECT_EQ(
            TdAccountData::PendingSendLookupResult::Ambiguous,
            accountData.extractPendingSend(
                pending, extracted));
        ASSERT_TRUE(accountData.extractPendingSend(
            firstChat, pending, extracted));
        EXPECT_EQ(ChatTarget::chat(firstChat), extracted.target);
        EXPECT_EQ(firstPath, extracted.tempFile);
        EXPECT_FALSE(accountData.extractPendingSend(
            firstChat, pending, extracted));
        EXPECT_TRUE(g_file_test(
            firstPath.c_str(), G_FILE_TEST_EXISTS));
        EXPECT_TRUE(g_file_test(
            secondPath.c_str(), G_FILE_TEST_EXISTS));
    }

    EXPECT_TRUE(g_file_test(
        firstPath.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test(
        secondPath.c_str(), G_FILE_TEST_EXISTS));
}

} // namespace
