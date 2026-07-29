#include "account-data.h"
#include "fixture.h"
#include "libpurple-mock.h"
#include "purple-info.h"
#include <fmt/format.h>
#include <td/telegram/td_api.h>
using namespace td::td_api;

class MessageOrderTest: public CommTest {
protected:
    void loginWithoutReadReceipts()
    {
        loginWithOneContact();
        setUiName("BitlBee");
        purple_account_set_string(
            account, AccountOptions::DownloadBehaviour,
            AccountOptions::DownloadBehaviourHyperlink);
        purple_account_set_bool(
            account, AccountOptions::ReadReceipts, FALSE);
        ASSERT_FALSE(isReadReceiptsEnabled(account));
    }

    object_ptr<messageDocument> remoteDocument(
        int32_t fileId, const std::string &name,
        const std::string &caption)
    {
        return make_object<messageDocument>(
            make_object<document>(
                name, "mime/type", nullptr, nullptr,
                make_object<file>(
                    fileId, 10000, 10000,
                    make_object<localFile>(
                        "", true, true, false, false,
                        0, 0, 0),
                    make_object<remoteFile>(
                        "remote", "unique", false, true,
                        10000))),
            make_object<formattedText>(
                caption,
                std::vector<object_ptr<textEntity>>()));
    }

    object_ptr<file> completedFile(
        int32_t fileId, const std::string &path)
    {
        return make_object<file>(
            fileId, 10000, 10000,
            make_object<localFile>(
                path, true, true, false, true,
                0, 10000, 10000),
            make_object<remoteFile>(
                "remote", "unique", false, true, 10000));
    }

    object_ptr<messagePhoto> remotePhoto(
        int32_t fileId, int32_t size,
        const std::string &caption)
    {
        std::vector<object_ptr<photoSize>> sizes;
        sizes.push_back(makePhotoSize(
            "main",
            make_object<file>(
                fileId, size, size,
                make_object<localFile>(
                    "", true, true, false, false,
                    0, 0, 0),
                make_object<remoteFile>(
                    "remote", "unique", false, true,
                    size)),
            640, 480));
        return makeMessagePhoto(
            make_object<photo>(
                false, nullptr, std::move(sizes)),
            make_object<formattedText>(
                caption,
                std::vector<object_ptr<textEntity>>()),
            false);
    }

    ConversationWriteEvent deletedNotice(
        const std::string &messageIds)
    {
        return ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            "Deleted message(s): " + messageIds,
            PURPLE_MESSAGE_SYSTEM, 0);
    }

    uint64_t startTimedOutDocument(
        int64_t messageId, int32_t fileId,
        int32_t date, const std::string &caption,
        std::string &tempFileName)
    {
        tgl.update(make_object<updateNewMessage>(makeMessage(
            messageId, userIds[0], chatIds[0], false, date,
            remoteDocument(
                fileId, "delayed.file", caption))));
        const uint64_t downloadRequest =
            tgl.verifyRequest(downloadFile(
                fileId, 1, 0, 0, true));
        prpl.verifyNoEvents();

        runTimeouts();
        tgl.verifyNoRequests();
        prpl.verifyEvents(
            XferAcceptedEvent(
                purpleUserName(0), &tempFileName),
            ServGotImEvent(
                connection, purpleUserName(0), caption,
                PURPLE_MESSAGE_RECV, date),
            ConversationWriteEvent(
                purpleUserName(0), purpleUserName(0),
                userFirstNames[0] + " " +
                    userLastNames[0] +
                    ": Downloading delayed.file [mime/type]",
                PURPLE_MESSAGE_SYSTEM, date));
        return downloadRequest;
    }

    void advanceTimedOutDocument(
        int32_t fileId,
        const std::string &tempFileName)
    {
        updateTimedOutDocumentProgress(fileId, 2000);
        tgl.verifyNoRequests();
        prpl.verifyEvents(
            XferStartEvent(tempFileName),
            XferProgressEvent(tempFileName, 2000));
    }

    void updateTimedOutDocumentProgress(
        int32_t fileId, int32_t downloadedSize)
    {
        tgl.update(make_object<updateFile>(
            make_object<file>(
                fileId, 10000, 10000,
                make_object<localFile>(
                    "/partial", true, true, true,
                    false, 0, 0, downloadedSize),
                make_object<remoteFile>(
                    "remote", "unique", false, true,
                    10000))));
    }
};

TEST_F(MessageOrderTest, ReplyOrdering)
{
    const int32_t dates[2]  = {10002, 10003};
    const int64_t msgIds[2] = {2, 3};
    const int32_t srcDate  = 10001;
    const int64_t srcMsgId = 1;
    loginWithOneContact();

    object_ptr<message> message = makeMessage(
        msgIds[0], userIds[0], chatIds[0], false, dates[0], makeTextMessage("reply")
    );
    message->reply_to_ = makeMessageReplyTo(chatIds[0], srcMsgId);

    tgl.update(make_object<updateNewMessage>(std::move(message)));
    uint64_t getMessageReqId = tgl.verifyRequest(
        getMessage(chatIds[0], srcMsgId)
    );
    prpl.verifyNoEvents();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        msgIds[1], userIds[0], chatIds[0], false, dates[1], makeTextMessage("followUp")
    )));
    prpl.verifyNoEvents();

    tgl.reply(getMessageReqId, makeMessage(srcMsgId, userIds[0], chatIds[0], false, srcDate, makeTextMessage("original")));
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0),
            fmt::format(replyPattern, userFirstNames[0] + " " + userLastNames[0], "original", "reply"),
            PURPLE_MESSAGE_RECV, dates[0]
        ),
        ServGotImEvent(connection, purpleUserName(0), "followUp", PURPLE_MESSAGE_RECV, dates[1])
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {msgIds[0], msgIds[1]}, true));
}

TEST_F(MessageOrderTest, Reply_FlushAtLogout)
{
    const int32_t dates[2]  = {10002, 10003};
    const int64_t msgIds[2] = {2, 3};
    const int64_t srcMsgId  = 1;
    loginWithOneContact();

    object_ptr<message> message = makeMessage(
        msgIds[0], userIds[0], chatIds[0], false, dates[0], makeTextMessage("reply")
    );
    message->reply_to_ = makeMessageReplyTo(chatIds[0], srcMsgId);

    tgl.update(make_object<updateNewMessage>(std::move(message)));
    tgl.verifyRequest(getMessage(chatIds[0], srcMsgId));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        msgIds[1], userIds[0], chatIds[0], false, dates[1], makeTextMessage("followUp")
    )));
    prpl.verifyNoEvents();

    pluginInfo().close(connection);
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0),
            fmt::format(replyPattern, "Unknown user", "[message unavailable]", "reply"),
            PURPLE_MESSAGE_RECV, dates[0]
        ),
        ServGotImEvent(connection, purpleUserName(0), "followUp", PURPLE_MESSAGE_RECV, dates[1])
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {msgIds[0], msgIds[1]}, true));
}

TEST_F(MessageOrderTest, Photo_Download_FlushAtLogout)
{
    const int32_t date   = 10001;
    const int32_t fileId = 1234;
    loginWithOneContact();

    std::vector<object_ptr<photoSize>> sizes;
    sizes.push_back(makePhotoSize(
        "whatever",
        make_object<file>(
            fileId, 10000, 10000,
            make_object<localFile>("", true, true, false, false, 0, 0, 0),
            make_object<remoteFile>("beh", "bleh", false, true, 10000)
        ),
        640, 480
    ));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        1,
        userIds[0],
        chatIds[0],
        false,
        date,
        makeMessagePhoto(
            make_object<photo>(false, nullptr, std::move(sizes)),
            make_object<formattedText>("photo", std::vector<object_ptr<textEntity>>()),
            false
        )
    )));
    tgl.verifyRequest(downloadFile(fileId, 1, 0, 0, true));
    prpl.verifyNoEvents();

    pluginInfo().close(connection);
    prpl.verifyEvents(
        ServGotImEvent(connection, purpleUserName(0), "photo", PURPLE_MESSAGE_RECV, date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] + ": Downloading photo",
            PURPLE_MESSAGE_SYSTEM, date
        )
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));
}

TEST_F(
    MessageOrderTest,
    ForcedSyncAnimatedStickerDoesNotDownloadThumbnail)
{
    constexpr int32_t BlockedFileId = 1234;
    constexpr int32_t StickerFileId = 1235;
    constexpr int32_t ThumbnailFileId = 1236;
    loginWithoutReadReceipts();
    purple_account_set_bool(
        account, AccountOptions::AnimatedStickers, FALSE);

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(
            BlockedFileId, "blocked.file", "blocked"))));
    tgl.verifyRequest(downloadFile(
        BlockedFileId, 1, 0, 0, true));

    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeMessageSticker(makeSticker(
            0, 320, 200, "", true, false, nullptr,
            make_object<thumbnail>(
                make_object<thumbnailFormatJpeg>(),
                320, 200,
                make_object<file>(
                    ThumbnailFileId, 10000, 10000,
                    make_object<localFile>(
                        "", true, true, false, false,
                        0, 0, 0),
                    make_object<remoteFile>(
                        "remote", "unique", false, true,
                        10000))),
            completedFile(
                StickerFileId, "/available.tgs"))))));
    prpl.verifyNoEvents();

    pluginInfo().close(connection);

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0), "blocked",
            PURPLE_MESSAGE_RECV, 10001),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] +
                ": Downloading blocked.file [mime/type]",
            PURPLE_MESSAGE_SYSTEM, 10001),
        ServGotImEvent(
            connection, purpleUserName(0),
            "<a href=\"file:///available.tgs\">sticker</a>",
            PURPLE_MESSAGE_RECV, 10002));
}

TEST_F(
    MessageOrderTest,
    ForcedSyncStandardDocumentDoesNotCreateTransfer)
{
    loginWithoutReadReceipts();
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourStandard);

    object_ptr<message> document = makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        remoteDocument(
            1234, "blocked.file", "document"));
    document->reply_to_ =
        makeMessageReplyTo(chatIds[0], 1);
    tgl.update(make_object<updateNewMessage>(
        std::move(document)));
    tgl.verifyRequest(getMessage(chatIds[0], 1));
    prpl.verifyNoEvents();

    pluginInfo().close(connection);

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0),
            fmt::format(
                replyPattern, "Unknown user",
                "[message unavailable]", "document"),
            PURPLE_MESSAGE_RECV, 10002),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] +
                ": Downloading blocked.file [mime/type]",
            PURPLE_MESSAGE_SYSTEM, 10002));
}

class MessageOrderTestLongDownloadInReply: public MessageOrderTest,
    public testing::WithParamInterface<std::string> {};

TEST_P(MessageOrderTestLongDownloadInReply, LongDownloadInReply)
{
    const int32_t date     = 10002;
    const int64_t msgId    = 2;
    const int32_t fileId   = 1234;
    const int32_t srcDate  = 10001;
    const int64_t srcMsgId = 1;
    std::string   caption  = GetParam();
    loginWithOneContact();

    object_ptr<message> message = makeMessage(
        msgId, userIds[0], chatIds[0], false, date,
        make_object<messageDocument>(
            make_object<document>(
                "doc.file.name", "mime/type", nullptr, nullptr,
                make_object<file>(
                    fileId, 10000, 10000,
                    make_object<localFile>("", true, true, false, false, 0, 0, 0),
                    make_object<remoteFile>("beh", "bleh", false, true, 10000)
                )
            ),
            make_object<formattedText>(caption, std::vector<object_ptr<textEntity>>())
        )
    );
    message->reply_to_ = makeMessageReplyTo(chatIds[0], srcMsgId);

    tgl.update(make_object<updateNewMessage>(std::move(message)));
    auto requestIds = tgl.verifyRequestsV(
        make_object<getMessage>(chatIds[0], srcMsgId),
        make_object<downloadFile>(fileId, 1, 0, 0, true)
    );
    uint64_t getMessageReqId = requestIds.at(0);
    uint64_t downloadReqId = requestIds.at(1);
    prpl.verifyNoEvents();

    tgl.reply(getMessageReqId, makeMessage(srcMsgId, userIds[0], chatIds[0], false, srcDate, makeTextMessage("1<2")));

    runTimeouts();
    std::string tempFileName;
    if (!caption.empty())
        prpl.verifyEvents(
            XferAcceptedEvent(purpleUserName(0), &tempFileName),
            ServGotImEvent(
                connection, purpleUserName(0),
                fmt::format(replyPattern, userFirstNames[0] + " " + userLastNames[0], "1&lt;2", caption),
                PURPLE_MESSAGE_RECV, date
            ),
            ConversationWriteEvent(
                purpleUserName(0), purpleUserName(0),
                userFirstNames[0] + " " + userLastNames[0] + ": Downloading doc.file.name [mime/type]",
                PURPLE_MESSAGE_SYSTEM, date
            )
        );
    else
        prpl.verifyEvents(
            XferAcceptedEvent(purpleUserName(0), &tempFileName),
            NewConversationEvent(PURPLE_CONV_TYPE_IM, account, purpleUserName(0)),
            ConversationWriteEvent(
                purpleUserName(0), purpleUserName(0),
                userFirstNames[0] + " " + userLastNames[0] + ": Downloading doc.file.name [mime/type]",
                PURPLE_MESSAGE_SYSTEM, date
            )
        );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {msgId}, true));

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, true, false, 0, 0, 2000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));
    prpl.verifyEvents(
        XferStartEvent(tempFileName),
        XferProgressEvent(tempFileName, 2000)
    );

    tgl.reply(downloadReqId, make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));
    prpl.verifyEvents(
        XferCompletedEvent(tempFileName, TRUE, 10000),
        XferEndEvent(tempFileName),
        ServGotImEvent(
            connection, purpleUserName(0),
            fmt::format(replyPattern, userFirstNames[0] + " " + userLastNames[0], "1&lt;2", 
                        "<a href=\"file:///path\">doc.file.name [mime/type]</a>"),
            PURPLE_MESSAGE_RECV, date
        )
    );
    ASSERT_FALSE(g_file_test(tempFileName.c_str(), G_FILE_TEST_EXISTS));
}

INSTANTIATE_TEST_CASE_P(bleh, MessageOrderTestLongDownloadInReply, ::testing::Values("", "caption"));

TEST_F(MessageOrderTest, DownloadOrdering)
{
    const int64_t messageId[3] = {1, 2, 3};
    const int32_t date[3]      = {10001, 10002, 10003};
    const int32_t fileId[3]    = {1234, 0, 1235};
    loginWithOneContact();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        messageId[0], userIds[0], chatIds[0], false, date[0],
        make_object<messageDocument>(
            make_object<document>(
                "doc1.file.name", "mime/type", nullptr, nullptr,
                make_object<file>(
                    fileId[0], 10000, 10000,
                    make_object<localFile>("", true, true, false, false, 0, 0, 0),
                    make_object<remoteFile>("beh", "bleh", false, true, 10000)
                )
            ),
            make_object<formattedText>("document1", std::vector<object_ptr<textEntity>>())
        )
    )));
    uint64_t download1ReqId = tgl.verifyRequest(
        downloadFile(fileId[0], 1, 0, 0, true)
    );
    prpl.verifyNoEvents();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        messageId[1], userIds[0], chatIds[0], false, date[1], makeTextMessage("followUp")
    )));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        messageId[2], userIds[0], chatIds[0], false, date[2],
        make_object<messageDocument>(
            make_object<document>(
                "doc2.file.name", "mime/type", nullptr, nullptr,
                make_object<file>(
                    fileId[2], 10000, 10000,
                    make_object<localFile>("", true, true, false, false, 0, 0, 0),
                    make_object<remoteFile>("beh", "bleh", false, true, 10000)
                )
            ),
            make_object<formattedText>("document1", std::vector<object_ptr<textEntity>>())
        )
    )));
    uint64_t download2ReqId = tgl.verifyRequest(
        downloadFile(fileId[2], 1, 0, 0, true)
    );
    prpl.verifyNoEvents();

    tgl.reply(download1ReqId, make_object<file>(
        fileId[0], 10000, 10000,
        make_object<localFile>("/path1", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0),
            "<a href=\"file:///path1\">doc1.file.name [mime/type]</a>\ndocument1",
            PURPLE_MESSAGE_RECV, date[0]
        ),
        ServGotImEvent(connection, purpleUserName(0), "followUp", PURPLE_MESSAGE_RECV, date[1])
    );
    tgl.verifyRequest(*Mock_ViewMessages(
        chatIds[0], {messageId[0], messageId[1]}, true));

    tgl.reply(download2ReqId, make_object<file>(
        fileId[0], 10000, 10000,
        make_object<localFile>("/path2", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0),
            "<a href=\"file:///path2\">doc2.file.name [mime/type]</a>\ndocument1",
            PURPLE_MESSAGE_RECV, date[2]
        )
    );
    tgl.verifyRequest(*Mock_ViewMessages(
        chatIds[0], {messageId[2]}, true));
}

TEST_F(
    MessageOrderTest,
    DeleteBlockedDownloadReleasesReadyFollowerAndIgnoresLateCompletion)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(FileId, "blocked.file", "blocked"))));
    const uint64_t downloadRequest = tgl.verifyRequest(
        downloadFile(FileId, 1, 0, 0, true));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeTextMessage("follower"))));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateDeleteMessages>(
        chatIds[0], std::vector<int64_t>{1},
        true, false));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0), "follower",
            PURPLE_MESSAGE_RECV, 10002),
        deletedNotice("1"));

    tgl.reply(
        downloadRequest,
        completedFile(FileId, "/late"));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
}

TEST_F(
    MessageOrderTest,
    BulkDeleteBlockedHeadAndReadyFollowerReleasesOnlySurvivor)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(FileId, "blocked.file", "blocked"))));
    const uint64_t downloadRequest = tgl.verifyRequest(
        downloadFile(FileId, 1, 0, 0, true));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeTextMessage("deleted follower"))));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        3, userIds[0], chatIds[0], false, 10003,
        makeTextMessage("survivor"))));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateDeleteMessages>(
        chatIds[0], std::vector<int64_t>{1, 2},
        true, false));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0), "survivor",
            PURPLE_MESSAGE_RECV, 10003),
        deletedNotice("1, 2"));

    tgl.reply(
        downloadRequest,
        completedFile(FileId, "/late"));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
}

TEST_F(
    MessageOrderTest,
    EditBlockedFileToTextReleasesEditedHeadAndFollower)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(FileId, "old.file", "old"))));
    const uint64_t oldDownload = tgl.verifyRequest(
        downloadFile(FileId, 1, 0, 0, true));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeTextMessage("follower"))));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateMessageContent>(
        chatIds[0], 1, makeTextMessage("edited")));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0), "edited",
            PURPLE_MESSAGE_RECV, 10001),
        ServGotImEvent(
            connection, purpleUserName(0), "follower",
            PURPLE_MESSAGE_RECV, 10002));

    tgl.reply(
        oldDownload,
        completedFile(FileId, "/late"));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
}

TEST_F(
    MessageOrderTest,
    EditBlockedFileToReplacementFileIgnoresOldGeneration)
{
    constexpr int32_t OldFileId = 1234;
    constexpr int32_t NewFileId = 1235;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(
            OldFileId, "old.file", "old caption"))));
    const uint64_t oldDownload = tgl.verifyRequest(
        downloadFile(OldFileId, 1, 0, 0, true));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeTextMessage("follower"))));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateMessageContent>(
        chatIds[0], 1,
        remoteDocument(
            NewFileId, "new.file", "new caption")));
    const uint64_t newDownload = tgl.verifyRequest(
        downloadFile(NewFileId, 1, 0, 0, true));
    tgl.verifyNoRequests();

    tgl.reply(
        oldDownload,
        completedFile(OldFileId, "/old"));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.reply(
        newDownload,
        completedFile(NewFileId, "/new"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0),
            "<a href=\"file:///new\">new.file [mime/type]</a>\n"
            "new caption",
            PURPLE_MESSAGE_RECV, 10001),
        ServGotImEvent(
            connection, purpleUserName(0), "follower",
            PURPLE_MESSAGE_RECV, 10002));
}

TEST_F(
    MessageOrderTest,
    DeleteReplyBlockedHeadReleasesFollowerAndIgnoresLateReply)
{
    loginWithoutReadReceipts();

    object_ptr<message> reply = makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeTextMessage("reply"));
    reply->reply_to_ =
        makeMessageReplyTo(chatIds[0], 1);
    tgl.update(make_object<updateNewMessage>(
        std::move(reply)));
    const uint64_t replyRequest = tgl.verifyRequest(
        getMessage(chatIds[0], 1));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        3, userIds[0], chatIds[0], false, 10003,
        makeTextMessage("follower"))));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateDeleteMessages>(
        chatIds[0], std::vector<int64_t>{2},
        true, false));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0), "follower",
            PURPLE_MESSAGE_RECV, 10003),
        deletedNotice("2"));

    tgl.reply(
        replyRequest,
        makeMessage(
            1, userIds[0], chatIds[0], false, 10001,
            makeTextMessage("original")));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
}

TEST_F(
    MessageOrderTest,
    EditReadyFollowerToDelayedKeepsItBehindCompletingHead)
{
    constexpr int32_t HeadFileId = 1234;
    constexpr int32_t FollowerFileId = 1235;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(
            HeadFileId, "head.file", "head caption"))));
    const uint64_t headDownload = tgl.verifyRequest(
        downloadFile(HeadFileId, 1, 0, 0, true));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeTextMessage("initial follower"))));

    tgl.update(make_object<updateMessageContent>(
        chatIds[0], 2,
        remoteDocument(
            FollowerFileId, "follower.file",
            "follower caption")));
    const uint64_t followerDownload = tgl.verifyRequest(
        downloadFile(FollowerFileId, 1, 0, 0, true));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.reply(
        headDownload,
        completedFile(HeadFileId, "/head"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotImEvent(
        connection, purpleUserName(0),
        "<a href=\"file:///head\">head.file [mime/type]</a>\n"
        "head caption",
        PURPLE_MESSAGE_RECV, 10001));

    tgl.reply(
        followerDownload,
        completedFile(FollowerFileId, "/follower"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotImEvent(
        connection, purpleUserName(0),
        "<a href=\"file:///follower\">"
        "follower.file [mime/type]</a>\n"
        "follower caption",
        PURPLE_MESSAGE_RECV, 10002));
}

TEST_F(
    MessageOrderTest,
    CachedDeleteDoesNotMutatePendingOrder)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(FileId, "cached.file", "cached"))));
    const uint64_t downloadRequest = tgl.verifyRequest(
        downloadFile(FileId, 1, 0, 0, true));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeTextMessage("follower"))));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateDeleteMessages>(
        chatIds[0], std::vector<int64_t>{1},
        true, true));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.reply(
        downloadRequest,
        completedFile(FileId, "/cached"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0),
            "<a href=\"file:///cached\">"
            "cached.file [mime/type]</a>\ncached",
            PURPLE_MESSAGE_RECV, 10001),
        ServGotImEvent(
            connection, purpleUserName(0), "follower",
            PURPLE_MESSAGE_RECV, 10002));
}

TEST_F(
    MessageOrderTest,
    EditAfterTimeoutSuppressesLateReleasedDownload)
{
    constexpr int64_t MessageId = 1;
    constexpr int32_t FileId = 1234;
    constexpr int32_t Date = 10001;
    loginWithoutReadReceipts();

    std::string tempFileName;
    const uint64_t downloadRequest =
        startTimedOutDocument(
            MessageId, FileId, Date,
            "old caption", tempFileName);
    advanceTimedOutDocument(
        FileId, tempFileName);

    tgl.update(make_object<updateMessageContent>(
        chatIds[0], MessageId,
        makeTextMessage("edited")));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConversationWriteEvent(
        purpleUserName(0),
        "Updated " + userFirstNames[0] + " " +
            userLastNames[0],
        "edited", PURPLE_MESSAGE_RECV, Date));

    tgl.reply(
        downloadRequest,
        completedFile(FileId, "/obsolete"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        XferCompletedEvent(
            tempFileName, TRUE, 10000),
        XferEndEvent(tempFileName));
    ASSERT_FALSE(g_file_test(
        tempFileName.c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(
    MessageOrderTest,
    DeleteAfterTimeoutSuppressesLateReleasedDownload)
{
    constexpr int64_t MessageId = 1;
    constexpr int32_t FileId = 1234;
    constexpr int32_t Date = 10001;
    loginWithoutReadReceipts();

    std::string tempFileName;
    const uint64_t downloadRequest =
        startTimedOutDocument(
            MessageId, FileId, Date,
            "deleted caption", tempFileName);
    advanceTimedOutDocument(
        FileId, tempFileName);

    tgl.update(make_object<updateDeleteMessages>(
        chatIds[0],
        std::vector<int64_t>{MessageId},
        false, false));
    tgl.verifyNoRequests();
    prpl.verifyEvents(deletedNotice("1"));

    tgl.reply(
        downloadRequest,
        completedFile(FileId, "/deleted"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        XferCompletedEvent(
            tempFileName, TRUE, 10000),
        XferEndEvent(tempFileName));
    ASSERT_FALSE(g_file_test(
        tempFileName.c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(
    MessageOrderTest,
    ReentrantCompletionDuringTimeoutDisplayDoesNotReuseRequest)
{
    constexpr int64_t MessageId = 1;
    constexpr int32_t FileId = 1234;
    constexpr int32_t Date = 10001;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        MessageId, userIds[0], chatIds[0], false, Date,
        remoteDocument(
            FileId, "reentrant.file", "caption"))));
    const uint64_t downloadRequest =
        tgl.verifyRequest(downloadFile(
            FileId, 1, 0, 0, true));
    prpl.verifyNoEvents();

    bool completed = false;
    bool ended = false;
    bool hyperlinkShown = false;
    prpl.onNextEvent(
        [this, downloadRequest, FileId,
         &completed, &ended, &hyperlinkShown](
            PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferAccepted, type);
            prpl.onNextEvent(
                [this, downloadRequest, FileId,
                 &completed, &ended, &hyperlinkShown](
                    PurpleEventType displayType) {
                    EXPECT_EQ(
                        PurpleEventType::ServGotIm,
                        displayType);
                    prpl.onNextEvent(
                        [this, &completed, &ended,
                         &hyperlinkShown](
                            PurpleEventType completeType) {
                            EXPECT_EQ(
                                PurpleEventType::
                                    XferCompleted,
                                completeType);
                            completed = true;
                            prpl.onNextEvent(
                                [this, &ended,
                                 &hyperlinkShown](
                                    PurpleEventType endType) {
                                    EXPECT_EQ(
                                        PurpleEventType::
                                            XferEnd,
                                        endType);
                                    ended = true;
                                    prpl.onNextEvent(
                                        [&hyperlinkShown](
                                            PurpleEventType
                                                messageType) {
                                            EXPECT_EQ(
                                                PurpleEventType::
                                                    ServGotIm,
                                                messageType);
                                            hyperlinkShown =
                                                true;
                                        });
                                });
                        });
                    tgl.reply(
                        downloadRequest,
                        completedFile(
                            FileId, "/reentrant"));
                });
        });

    runTimeouts();
    EXPECT_TRUE(completed);
    EXPECT_TRUE(ended);
    EXPECT_TRUE(hyperlinkShown);
    EXPECT_NE(
        nullptr,
        purple_connection_get_protocol_data(
            connection));
    tgl.verifyNoRequests();
    prpl.discardEvents();
}

TEST_F(
    MessageOrderTest,
    DisconnectDuringInlineXferAcceptedIsSafe)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(
            FileId, "accepted.file", "caption"))));
    tgl.verifyRequest(downloadFile(
        FileId, 1, 0, 0, true));
    prpl.verifyNoEvents();

    bool accepted = false;
    prpl.onNextEvent(
        [this, &accepted](PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferAccepted, type);
            accepted = true;
            pluginInfo().close(connection);
        });
    runTimeouts();

    EXPECT_TRUE(accepted);
    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    tgl.verifyNoRequests();
    prpl.discardEvents();
}

TEST_F(
    MessageOrderTest,
    DisconnectDuringInlineXferStartIsSafe)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    std::string tempFileName;
    startTimedOutDocument(
        1, FileId, 10001, "caption", tempFileName);

    bool started = false;
    bool cancelled = false;
    prpl.onNextEvent(
        [this, &started, &cancelled](
            PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::XferStart, type);
            started = true;
            prpl.onNextEvent(
                [&cancelled](PurpleEventType nestedType) {
                    EXPECT_EQ(
                        PurpleEventType::XferLocalCancel,
                        nestedType);
                    cancelled = true;
                });
            pluginInfo().close(connection);
        });
    updateTimedOutDocumentProgress(FileId, 2000);

    EXPECT_TRUE(started);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    EXPECT_FALSE(g_file_test(
        tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();
    prpl.discardEvents();
}

TEST_F(
    MessageOrderTest,
    DisconnectDuringInlineXferProgressIsSafe)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    std::string tempFileName;
    startTimedOutDocument(
        1, FileId, 10001, "caption", tempFileName);
    advanceTimedOutDocument(
        FileId, tempFileName);

    bool progressed = false;
    bool cancelled = false;
    prpl.onNextEvent(
        [this, &progressed, &cancelled](
            PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferProgress, type);
            progressed = true;
            prpl.onNextEvent(
                [&cancelled](PurpleEventType nestedType) {
                    EXPECT_EQ(
                        PurpleEventType::XferLocalCancel,
                        nestedType);
                    cancelled = true;
                });
            pluginInfo().close(connection);
        });
    updateTimedOutDocumentProgress(FileId, 5000);

    EXPECT_TRUE(progressed);
    EXPECT_TRUE(cancelled);
    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    EXPECT_FALSE(g_file_test(
        tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();
    prpl.discardEvents();
}

TEST_F(
    MessageOrderTest,
    DisconnectDuringInlineXferCompletionIsSafe)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    std::string tempFileName;
    const uint64_t downloadRequest =
        startTimedOutDocument(
            1, FileId, 10001, "caption", tempFileName);
    advanceTimedOutDocument(
        FileId, tempFileName);

    bool completed = false;
    bool ended = false;
    prpl.onNextEvent(
        [this, &completed, &ended](
            PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferCompleted, type);
            completed = true;
            prpl.onNextEvent(
                [&ended](PurpleEventType nestedType) {
                    EXPECT_EQ(
                        PurpleEventType::XferEnd,
                        nestedType);
                    ended = true;
                });
            pluginInfo().close(connection);
        });
    tgl.reply(
        downloadRequest,
        completedFile(FileId, "/completed"));

    EXPECT_TRUE(completed);
    EXPECT_TRUE(ended);
    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    EXPECT_FALSE(g_file_test(
        tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();
    prpl.discardEvents();
}

TEST_F(
    MessageOrderTest,
    DisconnectDuringInlineXferEndIsSafe)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    std::string tempFileName;
    const uint64_t downloadRequest =
        startTimedOutDocument(
            1, FileId, 10001, "caption", tempFileName);
    advanceTimedOutDocument(
        FileId, tempFileName);

    bool ended = false;
    prpl.onNextEvent(
        [this, &ended](PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferCompleted, type);
            prpl.onNextEvent(
                [this, &ended](
                    PurpleEventType nestedType) {
                    EXPECT_EQ(
                        PurpleEventType::XferEnd,
                        nestedType);
                    ended = true;
                    pluginInfo().close(connection);
                });
        });
    tgl.reply(
        downloadRequest,
        completedFile(FileId, "/ended"));

    EXPECT_TRUE(ended);
    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    EXPECT_FALSE(g_file_test(
        tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();
    prpl.discardEvents();
}

TEST_F(
    MessageOrderTest,
    EditDuringInlineXferStartStopsStaleProgress)
{
    constexpr int64_t MessageId = 1;
    constexpr int32_t FileId = 1234;
    constexpr int32_t Date = 10001;
    loginWithoutReadReceipts();

    std::string tempFileName;
    const uint64_t downloadRequest =
        startTimedOutDocument(
            MessageId, FileId, Date,
            "old caption", tempFileName);

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::XferStart, type);
            tgl.update(make_object<updateMessageContent>(
                chatIds[0], 1,
                makeTextMessage("edited")));
        });
    updateTimedOutDocumentProgress(FileId, 2000);
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        XferStartEvent(tempFileName),
        ConversationWriteEvent(
            purpleUserName(0),
            "Updated " + userFirstNames[0] + " " +
                userLastNames[0],
            "edited", PURPLE_MESSAGE_RECV, Date));

    tgl.reply(
        downloadRequest,
        completedFile(FileId, "/obsolete"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        XferCompletedEvent(
            tempFileName, TRUE, 10000),
        XferEndEvent(tempFileName));
}

TEST_F(
    MessageOrderTest,
    EditDuringRequestingNoticeDoesNotOpenPrompt)
{
    constexpr int64_t MessageId = 1;
    constexpr int32_t FileId = 1234;
    constexpr int32_t Date = 10001;
    purple_account_set_string(
        account, AccountOptions::AutoDownloadLimit, "0.5");
    loginWithoutReadReceipts();

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::ServGotIm, type);
            prpl.onNextEvent(
                [this](PurpleEventType noticeType) {
                    EXPECT_EQ(
                        PurpleEventType::ConversationWrite,
                        noticeType);
                    tgl.update(
                        make_object<updateMessageContent>(
                            chatIds[0], 1,
                            makeTextMessage("edited")));
                });
        });
    tgl.update(make_object<updateNewMessage>(makeMessage(
        MessageId, userIds[0], chatIds[0], false, Date,
        remotePhoto(FileId, 600000, "caption"))));

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0), "caption",
            PURPLE_MESSAGE_RECV, Date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] +
                ": Requesting photo download",
            PURPLE_MESSAGE_SYSTEM, Date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            "Message 1 updated: edited",
            PURPLE_MESSAGE_SYSTEM, 0));
}

TEST_F(
    MessageOrderTest,
    DeleteDuringRequestingNoticeDoesNotOpenPrompt)
{
    constexpr int64_t MessageId = 1;
    constexpr int32_t FileId = 1234;
    constexpr int32_t Date = 10001;
    purple_account_set_string(
        account, AccountOptions::AutoDownloadLimit, "0.5");
    loginWithoutReadReceipts();

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::ServGotIm, type);
            prpl.onNextEvent(
                [this](PurpleEventType noticeType) {
                    EXPECT_EQ(
                        PurpleEventType::ConversationWrite,
                        noticeType);
                    tgl.update(
                        make_object<updateDeleteMessages>(
                            chatIds[0],
                            std::vector<int64_t>{1},
                            true, false));
                });
        });
    tgl.update(make_object<updateNewMessage>(makeMessage(
        MessageId, userIds[0], chatIds[0], false, Date,
        remotePhoto(FileId, 600000, "caption"))));

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0), "caption",
            PURPLE_MESSAGE_RECV, Date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] +
                ": Requesting photo download",
            PURPLE_MESSAGE_SYSTEM, Date),
        deletedNotice("1"));
}

TEST_F(
    MessageOrderTest,
    EditWhileManualPromptOpenMakesYesNoOp)
{
    constexpr int64_t MessageId = 1;
    constexpr int32_t FileId = 1234;
    constexpr int32_t Date = 10001;
    purple_account_set_string(
        account, AccountOptions::AutoDownloadLimit, "0.5");
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        MessageId, userIds[0], chatIds[0], false, Date,
        remotePhoto(FileId, 600000, "caption"))));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0), "caption",
            PURPLE_MESSAGE_RECV, Date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] +
                ": Requesting photo download",
            PURPLE_MESSAGE_SYSTEM, Date),
        RequestActionEvent(
            connection, account, NULL, NULL, 2));

    tgl.update(make_object<updateMessageContent>(
        chatIds[0], MessageId,
        makeTextMessage("edited")));
    prpl.verifyEvents(ConversationWriteEvent(
        purpleUserName(0),
        "Updated " + userFirstNames[0] + " " +
            userLastNames[0],
        "edited", PURPLE_MESSAGE_RECV, Date));

    prpl.requestedAction("_Yes");
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
}

TEST_F(
    MessageOrderTest,
    DeleteWhileManualPromptOpenMakesYesNoOp)
{
    constexpr int64_t MessageId = 1;
    constexpr int32_t FileId = 1234;
    constexpr int32_t Date = 10001;
    purple_account_set_string(
        account, AccountOptions::AutoDownloadLimit, "0.5");
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        MessageId, userIds[0], chatIds[0], false, Date,
        remotePhoto(FileId, 600000, "caption"))));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0), "caption",
            PURPLE_MESSAGE_RECV, Date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] +
                ": Requesting photo download",
            PURPLE_MESSAGE_SYSTEM, Date),
        RequestActionEvent(
            connection, account, NULL, NULL, 2));

    tgl.update(make_object<updateDeleteMessages>(
        chatIds[0],
        std::vector<int64_t>{MessageId},
        true, false));
    prpl.verifyEvents(deletedNotice("1"));

    prpl.requestedAction("_Yes");
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
}

TEST_F(
    MessageOrderTest,
    SameClientChatReplacementDuringDownloadingNoticeContinues)
{
    constexpr int32_t FileId = 1234;
    constexpr int32_t Date = 10001;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, Date,
        remoteDocument(
            FileId, "replacement.file", "caption"))));
    const uint64_t downloadRequest =
        tgl.verifyRequest(downloadFile(
            FileId, 1, 0, 0, true));
    prpl.verifyNoEvents();

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferAccepted, type);
            prpl.onNextEvent(
                [this](PurpleEventType messageType) {
                    EXPECT_EQ(
                        PurpleEventType::ServGotIm,
                        messageType);
                    prpl.onNextEvent(
                        [this](PurpleEventType noticeType) {
                            EXPECT_EQ(
                                PurpleEventType::
                                    ConversationWrite,
                                noticeType);
                            tgl.update(
                                standardPrivateChat(0));
                        });
                });
        });
    runTimeouts();

    std::string tempFileName;
    prpl.verifyEvents(
        XferAcceptedEvent(
            purpleUserName(0), &tempFileName),
        ServGotImEvent(
            connection, purpleUserName(0), "caption",
            PURPLE_MESSAGE_RECV, Date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] +
                ": Downloading replacement.file [mime/type]",
            PURPLE_MESSAGE_SYSTEM, Date));
    tgl.verifyNoRequests();

    advanceTimedOutDocument(
        FileId, tempFileName);
    tgl.reply(
        downloadRequest,
        completedFile(FileId, "/replacement"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        XferCompletedEvent(
            tempFileName, TRUE, 10000),
        XferEndEvent(tempFileName),
        ServGotImEvent(
            connection, purpleUserName(0),
            "<a href=\"file:///replacement\">"
            "replacement.file [mime/type]</a>",
            PURPLE_MESSAGE_RECV, Date));
}

TEST_F(
    MessageOrderTest,
    ReentrantEditSkipsStaleReleasedBatchFollower)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(
            FileId, "blocked.file", "blocked"))));
    tgl.verifyRequest(downloadFile(
        FileId, 1, 0, 0, true));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeTextMessage("first follower"))));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        3, userIds[0], chatIds[0], false, 10003,
        makeTextMessage("stale follower"))));
    prpl.verifyNoEvents();

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::ServGotIm, type);
            tgl.update(make_object<updateMessageContent>(
                chatIds[0], 3,
                makeTextMessage("edited follower")));
        });
    tgl.update(make_object<updateDeleteMessages>(
        chatIds[0], std::vector<int64_t>{1},
        true, false));

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0),
            "first follower",
            PURPLE_MESSAGE_RECV, 10002),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            "Message 3 updated: edited follower",
            PURPLE_MESSAGE_SYSTEM, 0),
        deletedNotice("1"));
}

TEST_F(
    MessageOrderTest,
    ReentrantDeleteSkipsStaleReleasedBatchFollower)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(
            FileId, "blocked.file", "blocked"))));
    tgl.verifyRequest(downloadFile(
        FileId, 1, 0, 0, true));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeTextMessage("first follower"))));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        3, userIds[0], chatIds[0], false, 10003,
        makeTextMessage("deleted follower"))));
    prpl.verifyNoEvents();

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::ServGotIm, type);
            tgl.update(make_object<updateDeleteMessages>(
                chatIds[0], std::vector<int64_t>{3},
                true, false));
        });
    tgl.update(make_object<updateDeleteMessages>(
        chatIds[0], std::vector<int64_t>{1},
        true, false));

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0),
            "first follower",
            PURPLE_MESSAGE_RECV, 10002),
        deletedNotice("3"),
        deletedNotice("1"));
}

TEST_F(
    MessageOrderTest,
    EditDuringSelfDestructWarningSkipsOldContent)
{
    constexpr int64_t MessageId = 2;
    constexpr int32_t Date = 10002;
    loginWithoutReadReceipts();
    purple_account_set_bool(
        account, AccountOptions::ShowSelfDestruct, TRUE);

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        makeTextMessage("establish conversation"))));
    prpl.verifyEvents(ServGotImEvent(
        connection, purpleUserName(0),
        "establish conversation",
        PURPLE_MESSAGE_RECV, 10001));

    object_ptr<message> selfDestruct = makeMessage(
        MessageId, userIds[0], chatIds[0], false, Date,
        makeTextMessage("stale secret"));
    selfDestruct->self_destruct_type_ =
        make_object<messageSelfDestructTypeTimer>(30);
    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::ConversationWrite,
                type);
            tgl.update(make_object<updateMessageContent>(
                chatIds[0], 2,
                makeTextMessage("edited secret")));
        });
    tgl.update(make_object<updateNewMessage>(
        std::move(selfDestruct)));

    tgl.verifyNoRequests();
    prpl.verifyEvents(
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] +
                ": Received self-destructing message, "
                "displaying anyway",
            PURPLE_MESSAGE_SYSTEM, Date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            "Message 2 updated: edited secret",
            PURPLE_MESSAGE_SYSTEM, 0));
}

TEST(PendingMessageQueueTest, DeleteLastMessagePreservesHistoryGate)
{
    constexpr int64_t Chat = 1000;
    PendingMessageQueue queue;
    const ChatId chatId = ChatId::fromString("1000");

    queue.setChatNotReady(chatId);
    IncomingMessage blocked;
    blocked.message = makeMessage(
        1, 100, Chat, false, 10001,
        makeTextMessage("blocked"));
    const PendingMessageHandle blockedHandle =
        queue.addPendingMessage(
            std::move(blocked),
            PendingMessageQueue::Append);

    PendingMessageQueue::RemoveResult removed =
        queue.removeMessages(
            chatId, {MessageId::fromString("1")});
    EXPECT_EQ(1u, removed.removedCount);
    EXPECT_TRUE(removed.readyMessages.empty());
    EXPECT_EQ(
        PendingMessageState::Disposition::Cancelled,
        blockedHandle.disposition());
    EXPECT_FALSE(queue.isChatReady(chatId));

    IncomingMessage ready;
    ready.message = makeMessage(
        2, 100, Chat, false, 10002,
        makeTextMessage("ready"));
    IncomingMessage immediate =
        queue.addReadyMessage(
            std::move(ready),
            PendingMessageQueue::Append);
    EXPECT_FALSE(immediate.message);

    std::vector<IncomingMessage> readyMessages;
    queue.setChatReady(chatId, readyMessages);
    ASSERT_EQ(1u, readyMessages.size());
    ASSERT_TRUE(readyMessages.front().message);
    EXPECT_EQ(
        MessageId::fromString("2"),
        getId(*readyMessages.front().message));
}

TEST(
    PendingMessageQueueTest,
    FreshSameIdIncarnationCancelsReleasedState)
{
    constexpr int64_t Chat = 1000;
    PendingMessageQueue queue;

    IncomingMessage first;
    first.message = makeMessage(
        1, 100, Chat, false, 10001,
        makeTextMessage("first"));
    IncomingMessage released =
        queue.addReadyMessage(
            std::move(first),
            PendingMessageQueue::Append);
    const PendingMessageHandle firstHandle =
        released.pendingMessage;
    ASSERT_TRUE(firstHandle.valid());
    EXPECT_EQ(
        PendingMessageState::Disposition::Released,
        firstHandle.disposition());

    IncomingMessage replacement;
    replacement.message = makeMessage(
        1, 100, Chat, false, 10002,
        makeTextMessage("replacement"));
    const PendingMessageHandle replacementHandle =
        queue.addPendingMessage(
            std::move(replacement),
            PendingMessageQueue::Append);

    ASSERT_TRUE(replacementHandle.valid());
    EXPECT_NE(
        firstHandle.state, replacementHandle.state);
    EXPECT_EQ(
        PendingMessageState::Disposition::Cancelled,
        firstHandle.disposition());
    EXPECT_EQ(
        PendingMessageState::Disposition::Queued,
        replacementHandle.disposition());
    EXPECT_TRUE(
        queue.currentContentHandle(
            replacementHandle).current());
}

TEST_F(
    MessageOrderTest,
    ReleasedFollowerDisconnectStopsBatchSafely)
{
    constexpr int32_t FileId = 1234;
    loginWithoutReadReceipts();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, 10001,
        remoteDocument(FileId, "blocked.file", "blocked"))));
    tgl.verifyRequest(
        downloadFile(FileId, 1, 0, 0, true));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, 10002,
        makeTextMessage("first follower"))));
    tgl.update(make_object<updateNewMessage>(makeMessage(
        3, userIds[0], chatIds[0], false, 10003,
        makeTextMessage("second follower"))));
    prpl.verifyNoEvents();

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::ServGotIm, type);
            pluginInfo().close(connection);
        });
    tgl.update(make_object<updateDeleteMessages>(
        chatIds[0], std::vector<int64_t>{1},
        true, false));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    tgl.verifyNoRequests();
    prpl.verifyEvents(ServGotImEvent(
        connection, purpleUserName(0), "first follower",
        PURPLE_MESSAGE_RECV, 10002));
}
