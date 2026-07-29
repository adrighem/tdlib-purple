#include "libpurple-mock.h"
#include "purple-info.h"
#include "supergroup-test.h"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <utility>
#include <vector>

using namespace td::td_api;

namespace {

#if PURPLE_VERSION_CHECK(2, 14, 0)

constexpr int32_t FirstTopicId = 42;
constexpr int32_t SecondTopicId = 84;
constexpr int32_t FileSize = 10000;

struct UploadHandle {
    uint64_t preliminaryRequestId = 0;
    int32_t fileId = 0;
    PurpleXfer *xfer = nullptr;
    std::string path;
};

object_ptr<file> makeUploadFile(
    int32_t fileId, const std::string &path,
    bool active, int64_t uploadedSize)
{
    return make_object<file>(
        fileId, FileSize, FileSize,
        make_object<localFile>(
            path, false, false, false, true,
            0, FileSize, FileSize),
        make_object<remoteFile>(
            "", "", active, false, uploadedSize));
}

object_ptr<sendMessage> expectedDocumentSend(
    int64_t chatId, object_ptr<MessageTopic> topic,
    int32_t fileId)
{
    // Purple 2.x has no outgoing reply target. In particular, topic uploads
    // must not emulate routing by replying to a message in the topic.
    return Mock_SendMessage(
        chatId, std::move(topic), nullptr, nullptr,
        Mock_InputMessageDocument(
            make_object<inputFileId>(fileId),
            nullptr,
            make_object<formattedText>()));
}

class ForumTopicFileTransferTest : public SupergroupTest {
protected:
    void loginWithForumSupergroup()
    {
        loginWithSupergroup();
        tgl.update(make_object<updateSupergroup>(
            makeForumSupergroup(
                groupId, make_object<chatMemberStatusMember>(), 2)));
        tgl.verifyNoRequests();
        prpl.verifyNoEvents();
    }

    ChatTarget generalTarget() const
    {
        return ChatTarget::forumTopic(
            ChatId::fromString(
                std::to_string(groupChatId).c_str()),
            ForumTopicId::general());
    }

    ChatTarget topicTarget(int32_t topicId = FirstTopicId) const
    {
        return ChatTarget::forumTopic(
            ChatId::fromString(
                std::to_string(groupChatId).c_str()),
            ForumTopicId::fromValue(topicId));
    }

    ChatTarget ordinaryTarget() const
    {
        return ChatTarget::chat(
            ChatId::fromString(
                std::to_string(groupChatId).c_str()));
    }

    std::string topicPurpleName(
        int32_t topicId = FirstTopicId) const
    {
        return getPurpleChatName(topicTarget(topicId));
    }

    void cacheTopic(
        int32_t topicId = FirstTopicId,
        const std::string &name = "Support")
    {
        tgl.update(make_object<updateForumTopicInfo>(
            makeForumTopicInfo(
                groupChatId, topicId, name)));
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

    int32_t openTopic(
        int32_t topicId = FirstTopicId,
        const std::string &name = "Support")
    {
        cacheTopic(topicId, name);
        return openTarget(topicTarget(topicId));
    }

    UploadHandle beginUpload(
        int32_t purpleId, int32_t fileId,
        const std::string &path)
    {
        PurpleConversation *conversation =
            purple_find_chat(connection, purpleId);
        EXPECT_NE(nullptr, conversation);
        if (!conversation)
            return UploadHandle{};

        const char *title =
            purple_conversation_get_title(conversation);
        EXPECT_NE(nullptr, title);

        setFakeFileSize(path.c_str(), FileSize);
        pluginInfo().chat_send_file(
            connection, purpleId, path.c_str());
        prpl.verifyEvents(XferAcceptedEvent(
            title ? title : "", path));

        UploadHandle upload;
        upload.preliminaryRequestId =
            tgl.verifyRequest(uploadFile(
                make_object<inputFileLocal>(path),
                make_object<fileTypeDocument>(),
                1));
        upload.fileId = fileId;
        upload.xfer = prpl.getLastXfer();
        upload.path = path;
        EXPECT_NE(nullptr, upload.xfer);
        return upload;
    }

    void startUpload(const UploadHandle &upload)
    {
        ASSERT_NE(nullptr, upload.xfer);
        ASSERT_NE(0U, upload.preliminaryRequestId);

        tgl.reply(
            upload.preliminaryRequestId,
            makeUploadFile(
                upload.fileId, upload.path, true, 0));
        prpl.verifyEvents(
            XferStartEvent(upload.path),
            XferProgressEvent(upload.path, 0));
    }

    void completeUpload(const UploadHandle &upload)
    {
        tgl.update(make_object<updateFile>(
            makeUploadFile(
                upload.fileId, upload.path,
                false, FileSize)));
    }

    void completeAndExpectSend(
        const UploadHandle &upload,
        object_ptr<MessageTopic> topic)
    {
        completeUpload(upload);
        prpl.verifyEvents(
            XferCompletedEvent(
                upload.path, TRUE, FileSize),
            XferEndEvent(upload.path));
        tgl.verifyRequest(expectedDocumentSend(
            groupChatId, std::move(topic),
            upload.fileId));
    }

    void completeAndExpectNoSend(
        const UploadHandle &upload)
    {
        completeUpload(upload);
        prpl.verifyEvents(
            XferRemoteCancelEvent(upload.path));
        tgl.verifyNoRequests();
    }

    void deleteTopicAuthoritatively()
    {
        PurpleRoomlist *roomlist =
            pluginInfo().roomlist_get_list(connection);
        ASSERT_NE(nullptr, roomlist);
        prpl.discardEvents();

        const uint64_t requestId =
            tgl.verifyRequest(getForumTopics(
                groupChatId, "", 0, 0, 0, 100));
        tgl.reply(
            requestId,
            makeForumTopicsPage(
                0, std::vector<object_ptr<forumTopic>>()));
        prpl.discardEvents();
        tgl.verifyNoRequests();
        purple_roomlist_unref(roomlist);
    }
};

TEST_F(
    ForumTopicFileTransferTest,
    ChildUploadFinalSendUsesExactTopicAndNullReply)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);
    ASSERT_NE(nullptr, pluginInfo().chat_can_receive_file);
    EXPECT_TRUE(
        pluginInfo().chat_can_receive_file(
            connection, purpleId));

    const UploadHandle upload =
        beginUpload(purpleId, 1201, "/topic-child");
    startUpload(upload);
    completeAndExpectSend(
        upload,
        make_object<messageTopicForum>(FirstTopicId));
}

TEST_F(
    ForumTopicFileTransferTest,
    GeneralUploadFinalSendUsesExplicitTopicOneAndNullReply)
{
    loginWithForumSupergroup();
    const int32_t purpleId =
        openTarget(generalTarget());
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1202, "/topic-general");
    startUpload(upload);
    completeAndExpectSend(
        upload,
        make_object<messageTopicForum>(
            ForumTopicId::general().value()));
}

TEST_F(
    ForumTopicFileTransferTest,
    OrdinaryGroupUploadFinalSendKeepsNullTopicAndReply)
{
    loginWithSupergroup();
    const int32_t purpleId =
        openTarget(ordinaryTarget());
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1203, "/ordinary-group");
    startUpload(upload);
    completeAndExpectSend(upload, nullptr);
}

TEST_F(
    ForumTopicFileTransferTest,
    ConcurrentChildUploadsCompletedInReverseKeepTheirTargets)
{
    loginWithForumSupergroup();
    const int32_t firstPurpleId =
        openTopic(FirstTopicId, "First");
    const int32_t secondPurpleId =
        openTopic(SecondTopicId, "Second");
    ASSERT_GT(firstPurpleId, 0);
    ASSERT_GT(secondPurpleId, 0);
    ASSERT_NE(firstPurpleId, secondPurpleId);

    const UploadHandle first =
        beginUpload(firstPurpleId, 1204, "/topic-first");
    const UploadHandle second =
        beginUpload(secondPurpleId, 1205, "/topic-second");
    startUpload(first);
    startUpload(second);

    completeAndExpectSend(
        second,
        make_object<messageTopicForum>(SecondTopicId));
    completeAndExpectSend(
        first,
        make_object<messageTopicForum>(FirstTopicId));
}

TEST_F(
    ForumTopicFileTransferTest,
    SamePathAndTdFileIdAcrossChildrenFanOutWithoutTargetLoss)
{
    constexpr int32_t SharedFileId = 1300;
    const std::string sharedPath = "/topic-shared";

    loginWithForumSupergroup();
    const int32_t firstPurpleId =
        openTopic(FirstTopicId, "First");
    const int32_t secondPurpleId =
        openTopic(SecondTopicId, "Second");
    ASSERT_GT(firstPurpleId, 0);
    ASSERT_GT(secondPurpleId, 0);
    ASSERT_NE(firstPurpleId, secondPurpleId);

    const UploadHandle first =
        beginUpload(
            firstPurpleId, SharedFileId, sharedPath);
    const UploadHandle second =
        beginUpload(
            secondPurpleId, SharedFileId, sharedPath);
    startUpload(first);
    startUpload(second);

    // A TDLib file ID identifies the underlying file, not one Purple
    // transfer. One completion update must release every exact-room send
    // waiting on that file without collapsing their ChatTargets.
    completeUpload(first);
    prpl.verifyEvents(
        XferCompletedEvent(
            sharedPath, TRUE, FileSize),
        XferEndEvent(sharedPath),
        XferCompletedEvent(
            sharedPath, TRUE, FileSize),
        XferEndEvent(sharedPath));
    tgl.verifyRequestsV(
        expectedDocumentSend(
            groupChatId,
            make_object<messageTopicForum>(FirstTopicId),
            SharedFileId),
        expectedDocumentSend(
            groupChatId,
            make_object<messageTopicForum>(SecondTopicId),
            SharedFileId));
}

TEST_F(
    ForumTopicFileTransferTest,
    CancelBeforeDuplicateFileReplyKeepsTheLaterTopicUpload)
{
    constexpr int32_t SharedFileId = 1301;
    const std::string sharedPath = "/topic-shared-cancel";

    loginWithForumSupergroup();
    const int32_t firstPurpleId =
        openTopic(FirstTopicId, "First");
    const int32_t secondPurpleId =
        openTopic(SecondTopicId, "Second");

    const UploadHandle first =
        beginUpload(
            firstPurpleId, SharedFileId, sharedPath);
    const UploadHandle second =
        beginUpload(
            secondPurpleId, SharedFileId, sharedPath);
    startUpload(first);

    purple_xfer_cancel_local(first.xfer);
    prpl.verifyEvents(
        XferLocalCancelEvent(sharedPath));
    // The second preliminary request may resolve to the same TDLib file ID,
    // so canceling the underlying upload here would cancel the wrong waiter.
    tgl.verifyNoRequests();

    startUpload(second);
    tgl.verifyNoRequests();
    completeAndExpectSend(
        second,
        make_object<messageTopicForum>(SecondTopicId));
}

TEST_F(
    ForumTopicFileTransferTest,
    OrdinaryGroupEnabledAsForumDuringUploadUsesExplicitGeneral)
{
    loginWithSupergroup();
    const int32_t purpleId =
        openTarget(ordinaryTarget());
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1302, "/ordinary-to-forum");
    startUpload(upload);

    tgl.update(make_object<updateSupergroup>(
        makeForumSupergroup(
            groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    completeAndExpectSend(
        upload,
        make_object<messageTopicForum>(
            ForumTopicId::general().value()));
}

TEST_F(
    ForumTopicFileTransferTest,
    GeneralDisabledDuringUploadUsesStableOrdinaryBaseTarget)
{
    loginWithForumSupergroup();
    const int32_t purpleId =
        openTarget(generalTarget());
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1303, "/general-to-ordinary");
    startUpload(upload);

    tgl.update(make_object<updateSupergroup>(
        makeSupergroup(
            groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    completeAndExpectSend(upload, nullptr);
}

TEST_F(
    ForumTopicFileTransferTest,
    TopicRenameDuringUploadPreservesOriginalTarget)
{
    loginWithForumSupergroup();
    const int32_t purpleId =
        openTopic(FirstTopicId, "Before");
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1206, "/topic-rename");
    startUpload(upload);

    tgl.update(make_object<updateForumTopicInfo>(
        makeForumTopicInfo(
            groupChatId, FirstTopicId, "After")));
    tgl.verifyNoRequests();
    prpl.discardEvents();

    completeAndExpectSend(
        upload,
        make_object<messageTopicForum>(FirstTopicId));
}

TEST_F(
    ForumTopicFileTransferTest,
    LocalRoomCloseDuringUploadPreservesOriginalTarget)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1207, "/topic-local-close");
    startUpload(upload);

    PurpleConversation *conversation =
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account);
    ASSERT_NE(nullptr, conversation);
    purple_conversation_destroy(conversation);
    prpl.verifyNoEvents();

    completeAndExpectSend(
        upload,
        make_object<messageTopicForum>(FirstTopicId));
    EXPECT_EQ(
        nullptr,
        purple_find_conversation_with_account(
            PURPLE_CONV_TYPE_CHAT,
            topicPurpleName().c_str(), account));
}

TEST_F(
    ForumTopicFileTransferTest,
    AuthoritativeDeletionDuringUploadPreventsFinalSend)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1208, "/topic-deleted");
    startUpload(upload);
    deleteTopicAuthoritatively();

    completeAndExpectNoSend(upload);
}

TEST_F(
    ForumTopicFileTransferTest,
    ForumDisableDuringUploadPreventsFinalSend)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1209, "/topic-forum-disabled");
    startUpload(upload);

    tgl.update(make_object<updateSupergroup>(
        makeSupergroup(
            groupId, make_object<chatMemberStatusMember>(), 2)));
    tgl.verifyNoRequests();
    prpl.discardEvents();

    completeAndExpectNoSend(upload);
}

TEST_F(
    ForumTopicFileTransferTest,
    MembershipLossDuringUploadPreventsFinalSend)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1210, "/topic-membership-lost");
    startUpload(upload);

    tgl.update(make_object<updateSupergroup>(
        makeForumSupergroup(
            groupId, make_object<chatMemberStatusLeft>(), 2)));
    tgl.verifyNoRequests();
    prpl.discardEvents();

    completeAndExpectNoSend(upload);
}

TEST_F(
    ForumTopicFileTransferTest,
    CancellationBeforePreliminaryResponseNeverSendsDocument)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1211, "/topic-cancel-early");
    ASSERT_NE(nullptr, upload.xfer);

    purple_xfer_cancel_local(upload.xfer);
    prpl.verifyEvents(
        XferLocalCancelEvent(upload.path));

    tgl.reply(
        upload.preliminaryRequestId,
        makeUploadFile(
            upload.fileId, upload.path, true, 0));
    tgl.verifyRequest(
        cancelPreliminaryUploadFile(upload.fileId));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
}

TEST_F(
    ForumTopicFileTransferTest,
    CancellationAfterPreliminaryResponseNeverSendsDocument)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1212, "/topic-cancel-active");
    startUpload(upload);

    purple_xfer_cancel_local(upload.xfer);
    prpl.verifyEvents(
        XferLocalCancelEvent(upload.path));
    tgl.verifyRequest(
        cancelPreliminaryUploadFile(upload.fileId));

    completeUpload(upload);
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
}

TEST_F(
    ForumTopicFileTransferTest,
    CancellationBeforeErrorResponseIsSafe)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1310, "/topic-cancel-error");
    purple_xfer_cancel_local(upload.xfer);
    prpl.verifyEvents(
        XferLocalCancelEvent(upload.path));

    tgl.reply(
        upload.preliminaryRequestId,
        make_object<error>(400, "upload failed"));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
}

TEST_F(
    ForumTopicFileTransferTest,
    CanceledPendingUploadAtDisconnectIsSafe)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(
            purpleId, 1311,
            "/topic-cancel-pending-disconnect");
    purple_xfer_cancel_local(upload.xfer);
    prpl.verifyEvents(
        XferLocalCancelEvent(upload.path));

    pluginInfo().close(connection);
    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
}

TEST_F(
    ForumTopicFileTransferTest,
    SynchronousDisconnectFromUploadErrorIsSafe)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(
            purpleId, 1315,
            "/topic-error-disconnect");
    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferRemoteCancel,
                type);
            pluginInfo().close(connection);
        });

    tgl.reply(
        upload.preliminaryRequestId,
        make_object<error>(400, "upload failed"));
    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.verifyEvents(
        XferRemoteCancelEvent(upload.path));
    tgl.verifyNoRequests();
}

TEST_F(
    ForumTopicFileTransferTest,
    SynchronousDisconnectFromStartCallbackIsSafe)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1213, "/topic-disconnect-start");

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::XferStart, type);
            pluginInfo().close(connection);
        });
    tgl.reply(
        upload.preliminaryRequestId,
        makeUploadFile(
            upload.fileId, upload.path, true, 0));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.verifyEvents(
        XferStartEvent(upload.path),
        XferLocalCancelEvent(upload.path));
    tgl.verifyNoRequests();
}

TEST_F(
    ForumTopicFileTransferTest,
    SynchronousCancelFromCompletionCallbackIsSafe)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(
            purpleId, 1312,
            "/topic-cancel-completion");
    startUpload(upload);

    prpl.onNextEvent(
        [&upload](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::XferCompleted, type);
            purple_xfer_cancel_local(upload.xfer);
        });
    completeUpload(upload);

    prpl.verifyEvents(
        XferCompletedEvent(
            upload.path, TRUE, FileSize),
        XferLocalCancelEvent(upload.path));
    tgl.verifyRequest(expectedDocumentSend(
        groupChatId,
        make_object<messageTopicForum>(FirstTopicId),
        upload.fileId));
}

TEST_F(
    ForumTopicFileTransferTest,
    SynchronousEndFromCompletionCallbackIsSafe)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(
            purpleId, 1313,
            "/topic-end-completion");
    startUpload(upload);

    prpl.onNextEvent(
        [&upload](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::XferCompleted, type);
            purple_xfer_end(upload.xfer);
        });
    completeUpload(upload);

    prpl.verifyEvents(
        XferCompletedEvent(
            upload.path, TRUE, FileSize),
        XferEndEvent(upload.path));
    tgl.verifyRequest(expectedDocumentSend(
        groupChatId,
        make_object<messageTopicForum>(FirstTopicId),
        upload.fileId));
}

TEST_F(
    ForumTopicFileTransferTest,
    SynchronousCancelFromEndCallbackIsSafe)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(
            purpleId, 1314,
            "/topic-cancel-end");
    startUpload(upload);

    prpl.onNextEvent(
        [this, &upload](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::XferCompleted, type);
            prpl.onNextEvent(
                [&upload](PurpleEventType nestedType) {
                    EXPECT_EQ(
                        PurpleEventType::XferEnd,
                        nestedType);
                    purple_xfer_cancel_local(upload.xfer);
                });
        });
    completeUpload(upload);

    prpl.verifyEvents(
        XferCompletedEvent(
            upload.path, TRUE, FileSize),
        XferEndEvent(upload.path),
        XferLocalCancelEvent(upload.path));
    tgl.verifyRequest(expectedDocumentSend(
        groupChatId,
        make_object<messageTopicForum>(FirstTopicId),
        upload.fileId));
}

TEST_F(
    ForumTopicFileTransferTest,
    SynchronousDisconnectFromCompletionCallbackIsSafe)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1214, "/topic-disconnect-complete");
    startUpload(upload);

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::XferCompleted, type);
            pluginInfo().close(connection);
        });
    completeUpload(upload);

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.verifyEvents(
        XferCompletedEvent(
            upload.path, TRUE, FileSize),
        XferEndEvent(upload.path));
    tgl.verifyRequest(expectedDocumentSend(
        groupChatId,
        make_object<messageTopicForum>(FirstTopicId),
        upload.fileId));
}

TEST_F(
    ForumTopicFileTransferTest,
    SynchronousDisconnectFromProgressCallbackIsSafe)
{
    loginWithForumSupergroup();
    const int32_t purpleId = openTopic();
    ASSERT_GT(purpleId, 0);

    const UploadHandle upload =
        beginUpload(purpleId, 1215, "/topic-disconnect-progress");
    startUpload(upload);

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::XferProgress, type);
            pluginInfo().close(connection);
        });
    tgl.update(make_object<updateFile>(
        makeUploadFile(
            upload.fileId, upload.path,
            true, FileSize / 2)));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.verifyEvents(
        XferProgressEvent(upload.path, FileSize / 2),
        XferLocalCancelEvent(upload.path));
    tgl.verifyNoRequests();
}

#endif

} // namespace
