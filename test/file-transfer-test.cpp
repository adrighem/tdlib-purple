#include "fixture.h"
#include "libpurple-mock.h"
#include "buildopt.h"
#include "purple-info.h"
#include <td/telegram/td_api.h>
using namespace td::td_api;

class FileTransferTest: public CommTest {
protected:
    struct InlineReceive {
        uint64_t requestId;
        PurpleXfer *xfer;
        std::string tempFileName;
    };

    struct StandardReceive {
        uint64_t requestId;
        PurpleXfer *xfer;
    };

    object_ptr<messagePhoto> remotePhoto(
        int32_t fileId, const std::string &caption)
    {
        return makeMessagePhoto(
            makePhotoRemote(fileId, 10000, 640, 480),
            make_object<formattedText>(
                caption,
                std::vector<object_ptr<textEntity>>()),
            false);
    }

    object_ptr<messageDocument> remoteDocument(
        int32_t fileId, const std::string &caption)
    {
        return make_object<messageDocument>(
            make_object<document>(
                "shared.file", "mime/type", nullptr, nullptr,
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

    object_ptr<file> activeDownload(
        int32_t fileId, int32_t downloadedSize)
    {
        return make_object<file>(
            fileId, 10000, 10000,
            make_object<localFile>(
                "/partial", true, true, true, false,
                0, 0, downloadedSize),
            make_object<remoteFile>(
                "remote", "unique", false, true, 10000));
    }

    object_ptr<file> completedDownload(
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

    InlineReceive beginTimedOutInlineReceive(
        int64_t messageId, int32_t fileId,
        int32_t date, const std::string &caption)
    {
        tgl.update(make_object<updateNewMessage>(makeMessage(
            messageId, userIds[0], chatIds[0], false, date,
            remotePhoto(fileId, caption))));
        const uint64_t requestId = tgl.verifyRequest(
            downloadFile(fileId, 1, 0, 0, true));
        prpl.verifyNoEvents();

        runTimeouts();
        std::string tempFileName;
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
                    ": Downloading photo",
                PURPLE_MESSAGE_SYSTEM, date));
        tgl.verifyRequest(*Mock_ViewMessages(
            chatIds[0], {messageId}, true));

        PurpleXfer *xfer = prpl.getLastXfer();
        if (!xfer)
            ADD_FAILURE() << "Timed out inline download has no transfer";
        return InlineReceive{
            requestId, xfer, std::move(tempFileName)};
    }

    StandardReceive beginStandardReceive(
        int64_t messageId, int32_t fileId,
        int32_t date, const std::string &caption,
        const char *outputFileName)
    {
        tgl.update(make_object<updateNewMessage>(makeMessage(
            messageId, userIds[0], chatIds[0], false, date,
            remoteDocument(fileId, caption))));
        prpl.verifyEvents(XferRequestEvent(
            PURPLE_XFER_RECEIVE, purpleUserName(0).c_str(),
            "shared.file"));
        PurpleXfer *xfer = prpl.getLastXfer();
        if (!xfer)
            ADD_FAILURE() << "Standard download has no transfer";
        tgl.verifyNoRequests();

        purple_xfer_request_accepted(xfer, outputFileName);
        prpl.verifyEvents(
            XferAcceptedEvent(
                purpleUserName(0), outputFileName),
            XferStartEvent(outputFileName));
        const uint64_t requestId =
            tgl.verifyRequest(
                downloadFile(fileId, 1, 0, 0, true));
        return StandardReceive{requestId, xfer};
    }
};

TEST_F(FileTransferTest, Document_AlreadyDownloaded)
{
    const int64_t messageId = 1;
    const int32_t date      = 10001;
    const int32_t fileId    = 1234;
    loginWithOneContact();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        messageId,
        userIds[0],
        chatIds[0],
        false,
        date,
        make_object<messageDocument>(
            make_object<document>(
                "doc.file.name", "mime/type", nullptr, nullptr,
                make_object<file>(
                    fileId, 10000, 10000,
                    make_object<localFile>("/path", true, true, false, true, 0, 10000, 10000),
                    make_object<remoteFile>("beh", "bleh", false, true, 10000)
                )
            ),
            make_object<formattedText>("caption", std::vector<object_ptr<textEntity>>())
        )
    )));
    tgl.verifyRequestsV(
        Mock_ViewMessages(chatIds[0], std::vector<int64_t>(1, messageId), true)
    );
    prpl.verifyEvents(
        ServGotImEvent(
            connection,
            purpleUserName(0),
            "<a href=\"file:///path\">doc.file.name [mime/type]</a>\ncaption",
            PURPLE_MESSAGE_RECV,
            date
        )
    );
}

TEST_F(FileTransferTest, BigPhoto_RequestDownload)
{
    purple_account_set_string(account, "media-size-threshold", "0.5");

    const int32_t date   = 10001;
    const int32_t fileId = 1234;
    loginWithOneContact();

    std::vector<object_ptr<photoSize>> sizes;
    sizes.push_back(makePhotoSize(
        "whatever",
        make_object<file>(
            fileId, 600000, 600000,
            make_object<localFile>("", true, true, false, false, 0, 0, 0),
            make_object<remoteFile>("beh", "bleh", false, true, 600000)
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
            make_object<formattedText>("caption", std::vector<object_ptr<textEntity>>()),
            false
        )
    )));
    tgl.verifyRequest(
        *Mock_ViewMessages(chatIds[0], std::vector<int64_t>(1, 1), true)
    );
    prpl.verifyEvents(
        ServGotImEvent(connection, purpleUserName(0), "caption", PURPLE_MESSAGE_RECV, date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] + ": Requesting photo download",
            PURPLE_MESSAGE_SYSTEM, date
        ),
        RequestActionEvent(connection, account, NULL, NULL, 2)
    );

    prpl.requestedAction("_Yes");
    tgl.verifyRequest(downloadFile(fileId, 1, 0, 0, true));
    tgl.reply(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));

    prpl.verifyEvents(ServGotImEvent(
        connection,
        purpleUserName(0),
        "<img src=\"file:///path\">",
        (PurpleMessageFlags)(PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_IMAGES),
        date
    ));
}

TEST_F(FileTransferTest, BigPhoto_Ignore)
{
    purple_account_set_string(account, "media-size-threshold", "0.5");
    purple_account_set_string(account, "media-handling-behavior", "discard");

    const int32_t date   = 10001;
    const int32_t fileId = 1234;
    loginWithOneContact();

    std::vector<object_ptr<photoSize>> sizes;
    sizes.push_back(makePhotoSize(
        "whatever",
        make_object<file>(
            fileId, 655360, 655360,
            make_object<localFile>("", true, true, false, false, 0, 0, 0),
            make_object<remoteFile>("beh", "bleh", false, true, 655360)
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
            make_object<formattedText>("caption", std::vector<object_ptr<textEntity>>()),
            false
        )
    )));
    tgl.verifyRequest(
        *Mock_ViewMessages(chatIds[0], std::vector<int64_t>(1, 1), true)
    );
    prpl.verifyEvents(
        ServGotImEvent(connection, purpleUserName(0), "caption", PURPLE_MESSAGE_RECV, date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] + ": Ignoring photo download (purple_str_size_to_units)",
            PURPLE_MESSAGE_SYSTEM, date
        )
    );
}

TEST_F(FileTransferTest, SecretPhoto_AlreadyDownloaded)
{
    const int32_t date   = 10001;
    const int32_t fileId = 1234;
    loginWithOneContact();

    std::vector<object_ptr<photoSize>> sizes;
    sizes.push_back(makePhotoSize(
        "whatever",
        make_object<file>(
            fileId, 10000, 10000,
            make_object<localFile>("/path", true, true, false, true, 0, 10000, 10000),
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
            make_object<formattedText>("caption", std::vector<object_ptr<textEntity>>()),
            true
        )
    )));
    tgl.verifyRequest(
        *Mock_ViewMessages(chatIds[0], std::vector<int64_t>(1, 1), true)
    );

    // Secret photos are always ignored
    prpl.verifyEvents(
        ServGotImEvent(connection, purpleUserName(0), "caption", PURPLE_MESSAGE_RECV, date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] + ": Ignoring secret file (photo)",
            PURPLE_MESSAGE_SYSTEM, date
        )
    );
}

TEST_F(FileTransferTest, PhotoWithoutCaption)
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
            make_object<formattedText>("", std::vector<object_ptr<textEntity>>()),
            false
        )
    )));
    tgl.verifyRequest(downloadFile(fileId, 1, 0, 0, true));
    prpl.verifyNoEvents();

    tgl.reply(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));

    prpl.verifyEvents(ServGotImEvent(
        connection,
        purpleUserName(0),
        "<img src=\"file:///path\">",
        (PurpleMessageFlags)(PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_IMAGES),
        date
    ));
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));
}

TEST_F(FileTransferTest, SendFile_ErrorInUploadResponse)
{
    const char *const PATH = "/path";
    loginWithOneContact();

    setFakeFileSize(PATH, 9000);
    pluginInfo().send_file(connection, purpleUserName(0).c_str(), PATH);
    prpl.verifyEvents(XferAcceptedEvent(purpleUserName(0), PATH));
    tgl.verifyRequest(uploadFile(
        make_object<inputFileLocal>(PATH),
        make_object<fileTypeDocument>(),
        1
    ));

    tgl.reply(make_object<error>(1, "error"));
    prpl.verifyEvents(XferRemoteCancelEvent(PATH));
}

TEST_F(FileTransferTest, SendFile_SendMessageResponseError)
{
    const char *const PATH   = "/path";
    const int32_t     fileId = 1234;
    loginWithOneContact();

    setFakeFileSize(PATH, 9000);
    pluginInfo().send_file(connection, purpleUserName(0).c_str(), PATH);
    prpl.verifyEvents(XferAcceptedEvent(purpleUserName(0), PATH));
    tgl.verifyRequest(uploadFile(
        make_object<inputFileLocal>(PATH),
        make_object<fileTypeDocument>(),
        1
    ));

    tgl.reply(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(PATH, false, false, false, true, 0, 10000, 10000),
        make_object<remoteFile>("", "", true, false, 0)
    ));
    prpl.verifyEvents(
        XferStartEvent(PATH),
        XferProgressEvent(PATH, 0)
    );

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(PATH, false, false, false, true, 0, 10000, 10000),
        make_object<remoteFile>("", "", true, false, 5000)
    )));
    prpl.verifyEvents(XferProgressEvent(PATH, 5000));

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(PATH, false, false, false, true, 0, 10000, 10000),
        make_object<remoteFile>("", "", true, false, 9500)
    )));
    prpl.verifyEvents(XferProgressEvent(PATH, 9000));

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(PATH, false, false, false, true, 0, 10000, 10000),
        make_object<remoteFile>("", "", false, false, 10000)
    )));
    prpl.verifyEvents(
        XferCompletedEvent(PATH, TRUE, 9000),
        XferEndEvent(PATH)
    );
    tgl.verifyRequest(*Mock_SendMessage(
        chatIds[0],
        0,
        nullptr,
        nullptr,
        Mock_InputMessageDocument(
            make_object<inputFileId>(fileId),
            nullptr,
            make_object<formattedText>()
        )
    ));

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(PATH, false, false, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));

    tgl.reply(make_object<error>(100, "error"));
    prpl.verifyEvents(
        NewConversationEvent(PURPLE_CONV_TYPE_IM, account, purpleUserName(0)),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            "Failed to send message: code 100 (error)",
            PURPLE_MESSAGE_SYSTEM, 0
        )
    );
}

TEST_F(FileTransferTest, SendFile_UnknownUser)
{
    const char *const PATH = "/path";
    login();

    setFakeFileSize(PATH, 9000);
    pluginInfo().send_file(connection, "Antonie van Leeuwenhoek", PATH);
    prpl.verifyEvents(
        XferAcceptedEvent("Antonie van Leeuwenhoek", PATH),
        XferLocalCancelEvent(PATH)
    );
}

#ifndef NoWebp
TEST_F(FileTransferTest, WebpStickerDecode)
#else
TEST_F(FileTransferTest, DISABLED_WebpStickerDecode)
#endif
{
    const int32_t date      = 10001;
    const int32_t fileId    = 1234;
    loginWithOneContact();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1,
        userIds[0],
        chatIds[0],
        false,
        date,
        makeMessageSticker(makeSticker(
            0, 320, 200, "", true, false, nullptr,
            nullptr,
            make_object<file>(
                fileId, 10000, 10000,
                make_object<localFile>(TEST_SOURCE_DIR "/test.webp", true, true, false, true, 0, 10000, 10000),
                make_object<remoteFile>("beh", "bleh", false, true, 10000)
            )
        ))
    )));
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));
    prpl.verifyEvents(ServGotImEvent(
        connection,
        purpleUserName(0),
        "\n<img id=\"" + std::to_string(getLastImgstoreId()) + "\">",
        (PurpleMessageFlags)(PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_IMAGES),
        date
    ));
}

#ifndef NoLottie
TEST_F(FileTransferTest, AnimatedStickerDecode)
#else
TEST_F(FileTransferTest, DISABLED_AnimatedStickerDecode)
#endif
{
    const int32_t date    = 10001;
    const int32_t fileId  = 1234;
    loginWithOneContact();

    // No thumbnail, only .tgs
    tgl.update(make_object<updateNewMessage>(makeMessage(
        1,
        userIds[0],
        chatIds[0],
        false,
        date,
        makeMessageSticker(makeSticker(
            0, 320, 200, "", true, false, nullptr,
            nullptr,
            make_object<file>(
                fileId, 10000, 10000,
                make_object<localFile>(TEST_SOURCE_DIR "/test.tgs", true, true, false, true, 0, 10000, 10000),
                make_object<remoteFile>("beh", "bleh", false, true, 10000)
            )
        ))
    )));
    tgl.verifyRequestsV(
        Mock_ViewMessages(chatIds[0], std::vector<int64_t>(1, 1), true)
    );

    tgl.reply(make_object<error>(404, "Not Found")); // reply to viewMessages

    prpl.verifyEvents(
        ServGotImEvent(
            connection,
            purpleUserName(0),
            // Sticker was converted to gif
            "\n<img id=\"" + std::to_string(getLastImgstoreId()) + "\">",
            (PurpleMessageFlags)(PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_IMAGES),
            date
        )
    );
}

TEST_F(FileTransferTest, Sticker_AnimatedDisabled_AlreadyDownloaded)
{
    const int32_t date      = 10001;
    const int32_t fileId[2] = {1234, 1235};
    const int32_t thumbId   = 1236;
    purple_account_set_bool(account, "animated-stickers", FALSE);
    loginWithOneContact();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1,
        userIds[0],
        chatIds[0],
        false,
        date,
        makeMessageSticker(makeSticker(
            0, 320, 200, "", true, false, nullptr,
            nullptr,
            make_object<file>(
                fileId[0], 10000, 10000,
                nullptr,
                make_object<remoteFile>("beh", "bleh", false, true, 10000)
            )
        ))
    )));
    tgl.verifyRequest(downloadFile(fileId[0], 1, 0, 0, true));
    prpl.verifyNoEvents();

    tgl.reply(make_object<file>(
        fileId[0], 10000, 10000,
        make_object<localFile>("/sticker", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));

    prpl.verifyEvents(ServGotImEvent(
        connection,
        purpleUserName(0),
        "<a href=\"file:///sticker\">sticker</a>",
        PURPLE_MESSAGE_RECV,
        date
    ));
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));

    // Now with thumbnail and main file, both already downloaded
    tgl.update(make_object<updateNewMessage>(makeMessage(
        2,
        userIds[0],
        chatIds[0],
        false,
        date,
        makeMessageSticker(makeSticker(
            0, 320, 200, "", true, false, nullptr,
            make_object<thumbnail>(
                make_object<thumbnailFormatJpeg>(),
                320, 200,
                make_object<file>(
                    thumbId, 10000, 10000,
                    make_object<localFile>("/thumb", true, true, false, true, 0, 10000, 10000),
                    make_object<remoteFile>("beh", "bleh", false, true, 10000)
                )
            ),
            make_object<file>(
                fileId[1], 10000, 10000,
                make_object<localFile>("/sticker2.tgs", true, true, false, true, 0, 10000, 10000),
                make_object<remoteFile>("beh", "bleh", false, true, 10000)
            )
        ))
    )));

    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {2}, true));
    prpl.verifyEvents(ServGotImEvent(
        connection,
        purpleUserName(0),
        // Sticker replaced with thumbnail because it's .tgs
        "<a href=\"file:///thumb\">sticker</a>",
        PURPLE_MESSAGE_RECV,
        date
    ));
}

TEST_F(FileTransferTest, Sticker_AnimatedDisabled_ThumbnailAboveLimit)
{
    const int32_t date      = 10001;
    const int32_t fileId    = 1234;
    const int32_t thumbId   = 1236;
    purple_account_set_bool(account, "animated-stickers", FALSE);
    purple_account_set_string(account, "media-size-threshold", "0.1");
    loginWithOneContact();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1,
        userIds[0],
        chatIds[0],
        false,
        date,
        makeMessageSticker(makeSticker(
            0, 320, 200, "", true, false, nullptr,
            make_object<thumbnail>(
                make_object<thumbnailFormatJpeg>(),
                320, 200,
                make_object<file>(
                    thumbId, 100000000, 100000000,
                    make_object<localFile>("", true, true, false, false, 0, 0, 0),
                    make_object<remoteFile>("beh", "bleh", false, true, 100000000)
                )
            ),
            make_object<file>(
                fileId, 10000, 10000,
                make_object<localFile>("", true, true, false, false, 0, 0, 0),
                make_object<remoteFile>("beh", "bleh", false, true, 10000)
            )
        ))
    )));
    tgl.verifyRequest(downloadFile(fileId, 1, 0, 0, true));
    prpl.verifyNoEvents();

    tgl.reply(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/sticker.tgs", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));
    prpl.verifyNoEvents();
    tgl.verifyRequest(downloadFile(thumbId, 1, 0, 0, true));

    tgl.reply(make_object<file>(
        fileId, 100000000, 100000000,
        make_object<localFile>("/thumb", true, true, false, true, 0, 100000000, 100000000),
        make_object<remoteFile>("beh", "bleh", false, true, 100000000)
    ));
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0),
            // Sticker replaced with thumbnail because it's .tgs
            "<a href=\"file:///thumb\">sticker</a>",
            PURPLE_MESSAGE_RECV, date
        )
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));
}

TEST_F(FileTransferTest, Sticker_AnimatedDisabled_LongDownloads_ThumbnailAboveLimit)
{
    const int32_t date      = 10001;
    const int32_t fileId    = 1234;
    const int32_t thumbId   = 1236;
    purple_account_set_bool(account, "animated-stickers", FALSE);
    purple_account_set_string(account, "media-size-threshold", "0.1");
    loginWithOneContact();

    // Now with thumbnail and main file, both already downloaded
    tgl.update(make_object<updateNewMessage>(makeMessage(
        1,
        userIds[0],
        chatIds[0],
        false,
        date,
        makeMessageSticker(makeSticker(
            0, 320, 200, "", true, false, nullptr,
            make_object<thumbnail>(
                make_object<thumbnailFormatJpeg>(),
                320, 200,
                make_object<file>(
                    thumbId, 100000000, 100000000,
                    make_object<localFile>("", true, true, false, false, 0, 0, 0),
                    make_object<remoteFile>("beh", "bleh", false, true, 100000000)
                )
            ),
            make_object<file>(
                fileId, 10000, 10000,
                make_object<localFile>("", true, true, false, false, 0, 0, 0),
                make_object<remoteFile>("beh", "bleh", false, true, 10000)
            )
        ))
    )));
    tgl.verifyRequest(downloadFile(fileId, 1, 0, 0, true));
    prpl.verifyNoEvents();

    runTimeouts();
    std::string tempFileName;
    prpl.verifyEvents(
        XferAcceptedEvent(purpleUserName(0), &tempFileName)
    );

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/sticker.tgs", true, true, true, false, 0, 0, 2000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));
    prpl.verifyEvents(
        XferStartEvent(tempFileName),
        XferProgressEvent(tempFileName, 2000)
    );

    tgl.reply(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/sticker.tgs", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));
    prpl.verifyEvents(
        XferCompletedEvent(tempFileName, TRUE, 10000),
        XferEndEvent(tempFileName)
    );
    ASSERT_FALSE(g_file_test(tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyRequestsV(make_object<downloadFile>(thumbId, 1, 0, 0, true));

    runTimeouts();
    prpl.verifyEvents(
        XferAcceptedEvent(purpleUserName(0), &tempFileName)
    );

    tgl.update(make_object<updateFile>(make_object<file>(
        thumbId, 100000000, 100000000,
        make_object<localFile>("/thumb", true, true, true, false, 0, 0, 2000),
        make_object<remoteFile>("beh", "bleh", false, true, 100000000)
    )));
    prpl.verifyEvents(
        XferStartEvent(tempFileName),
        XferProgressEvent(tempFileName, 2000)
    );

    tgl.reply(make_object<file>(
        fileId, 100000000, 100000000,
        make_object<localFile>("/thumb", true, true, false, true, 0, 100000000, 100000000),
        make_object<remoteFile>("beh", "bleh", false, true, 100000000)
    ));
    prpl.verifyEvents(
        XferCompletedEvent(tempFileName, TRUE, 100000000),
        XferEndEvent(tempFileName),
        ServGotImEvent(
            connection, purpleUserName(0),
            "<a href=\"file:///thumb\">sticker</a>",
            PURPLE_MESSAGE_RECV, date
        )
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));

    ASSERT_FALSE(g_file_test(tempFileName.c_str(), G_FILE_TEST_EXISTS));
}

TEST_F(FileTransferTest, Photo_DownloadProgress_StuckAtStart)
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
    uint64_t downloadReqId = tgl.verifyRequest(
        downloadFile(fileId, 1, 0, 0, true)
    );
    prpl.verifyNoEvents();

    tgl.runTimeouts();
    std::string tempFileName;
    prpl.verifyEvents(
        XferAcceptedEvent(purpleUserName(0), &tempFileName),
        ServGotImEvent(connection, purpleUserName(0), "photo", PURPLE_MESSAGE_RECV, date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] + ": Downloading photo",
            PURPLE_MESSAGE_SYSTEM, date
        )
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, true, false, 0, 0, 2000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));
    prpl.verifyEvents(
        XferStartEvent(tempFileName),
        XferProgressEvent(tempFileName, 2000)
    );

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, true, false, 0, 0, 5000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));
    prpl.verifyEvents(
        XferProgressEvent(tempFileName, 5000)
    );

    tgl.reply(downloadReqId, make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));
    ASSERT_FALSE(g_file_test(tempFileName.c_str(), G_FILE_TEST_EXISTS));
    prpl.verifyEvents(
        XferCompletedEvent(tempFileName, TRUE, 10000),
        XferEndEvent(tempFileName),
        ServGotImEvent(
            connection,
            purpleUserName(0),
            "<img src=\"file:///path\">",
            (PurpleMessageFlags)(PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_IMAGES),
            date
        )
    );

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));
}

TEST_F(FileTransferTest, Photo_DownloadProgress)
{
    const int32_t date   = 10001;
    const int32_t fileId = 1234;
    const int32_t date2  = 10002;
    const int64_t messageId2 = 2;
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
    uint64_t downloadReqId = tgl.verifyRequest(
        downloadFile(fileId, 1, 0, 0, true)
    );
    prpl.verifyNoEvents();

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, true, false, 0, 0, 2000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));

    tgl.runTimeouts();
    std::string tempFileName;
    prpl.verifyEvents(
        XferAcceptedEvent(purpleUserName(0), &tempFileName),
        XferStartEvent(&tempFileName),
        ServGotImEvent(connection, purpleUserName(0), "photo", PURPLE_MESSAGE_RECV, date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] + ": Downloading photo",
            PURPLE_MESSAGE_SYSTEM, date
        )
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));

    tgl.update(make_object<updateNewMessage>(makeMessage(
        messageId2, userIds[0], chatIds[0], false, date2, makeTextMessage("followUp")
    )));
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {messageId2}, true));
    prpl.verifyEvents(ServGotImEvent(connection, purpleUserName(0), "followUp", PURPLE_MESSAGE_RECV, date2));

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, true, false, 0, 0, 5000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));

    prpl.verifyEvents(XferProgressEvent(tempFileName, 5000));

    tgl.reply(downloadReqId, make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));
    ASSERT_FALSE(g_file_test(tempFileName.c_str(), G_FILE_TEST_EXISTS));
    prpl.verifyEvents(
        XferCompletedEvent(tempFileName, TRUE, 10000),
        XferEndEvent(tempFileName),
        ServGotImEvent(
            connection,
            purpleUserName(0),
            "<img src=\"file:///path\">",
            (PurpleMessageFlags)(PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_IMAGES),
            date
        )
    );

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));
}

TEST_F(FileTransferTest, Photo_DownloadProgress_StuckAtStart_Cancel)
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
    uint64_t downloadFileReqId = tgl.verifyRequest(
        downloadFile(fileId, 1, 0, 0, true)
    );
    prpl.verifyNoEvents();

    tgl.runTimeouts();
    std::string tempFileName;
    prpl.verifyEvents(
        XferAcceptedEvent(purpleUserName(0), &tempFileName),
        ServGotImEvent(connection, purpleUserName(0), "photo", PURPLE_MESSAGE_RECV, date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] + ": Downloading photo",
            PURPLE_MESSAGE_SYSTEM, date
        )
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));

    purple_xfer_cancel_local(prpl.getLastXfer());
    prpl.verifyEvents(XferLocalCancelEvent(tempFileName));
    tgl.reply(downloadFileReqId, make_object<error>(400, "Download cancelled"));
    ASSERT_FALSE(g_file_test(tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyRequest(cancelDownloadFile(fileId, false));

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, false, false, 0, 1000, 1000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));
}

TEST_F(FileTransferTest, Photo_DownloadProgress_Cancel)
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
    auto downloadFileReqId = tgl.verifyRequest(
        downloadFile(fileId, 1, 0, 0, true)
    );
    prpl.verifyNoEvents();

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, true, false, 0, 0, 2000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));

    tgl.runTimeouts();
    std::string tempFileName;
    prpl.verifyEvents(
        XferAcceptedEvent(purpleUserName(0), &tempFileName),
        XferStartEvent(&tempFileName),
        ServGotImEvent(connection, purpleUserName(0), "photo", PURPLE_MESSAGE_RECV, date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] + ": Downloading photo",
            PURPLE_MESSAGE_SYSTEM, date
        )
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, true, false, 0, 0, 5000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));

    prpl.verifyEvents(
        XferProgressEvent(tempFileName, 5000)
    );

    purple_xfer_cancel_local(prpl.getLastXfer());
    prpl.verifyEvents(XferLocalCancelEvent(tempFileName));
    tgl.reply(downloadFileReqId, make_object<error>(400, "Download cancelled"));
    ASSERT_FALSE(g_file_test(tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyRequest(cancelDownloadFile(fileId, false));

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, false, false, 0, 6000, 6000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));
}

TEST_F(FileTransferTest, SendFileToNonContact)
{
    const char *const PATH   = "/path";
    const int32_t     fileId = 1234;
    setFakeFileSize(PATH, 10000);

    login();

    // Normaly users without chats have to be group chat members, but for the test it doesn't matter
    tgl.update(standardUpdateUser(0));

    // Send ﬁle successfully
    pluginInfo().send_file(
        connection,
        (userFirstNames[0] + " " + userLastNames[0]).c_str(),
        PATH
    );
    prpl.verifyEvents(
        XferAcceptedEvent(userFirstNames[0] + " " + userLastNames[0], PATH)
    );
    tgl.verifyRequest(createPrivateChat(userIds[0], false));

    tgl.update(standardPrivateChat(0));
    tgl.reply(makeChat(
        chatIds[0],
        make_object<chatTypePrivate>(userIds[0]),
        userFirstNames[0] + " " + userLastNames[0],
        nullptr, 0, 0, 0
    ));
    tgl.verifyRequest(uploadFile(
        make_object<inputFileLocal>(PATH),
        make_object<fileTypeDocument>(),
        1
    ));

    tgl.reply(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(PATH, false, false, false, true, 0, 10000, 10000),
        make_object<remoteFile>("", "", true, false, 0)
    ));
    prpl.verifyEvents(
        AddBuddyEvent(purpleUserName(0), userFirstNames[0] + " " + userLastNames[0], account, nullptr, nullptr, nullptr),
        XferStartEvent(PATH),
        XferProgressEvent(PATH, 0)
    );
    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(PATH, false, false, false, true, 0, 10000, 10000),
        make_object<remoteFile>("", "", false, false, 10000)
    )));
    prpl.verifyEvents(
        XferCompletedEvent(PATH, TRUE, 10000),
        XferEndEvent(PATH)
    );
    tgl.verifyRequest(*Mock_SendMessage(
        chatIds[0],
        0,
        nullptr,
        nullptr,
        Mock_InputMessageDocument(
            make_object<inputFileId>(fileId),
            nullptr,
            make_object<formattedText>()
        )
    ));
    ASSERT_EQ(0, pluginInfo().send_im(
        connection,
        (userFirstNames[0] + " " + userLastNames[0]).c_str(),
        "message2",
        PURPLE_MESSAGE_SEND
    ));
    tgl.verifyRequest(*Mock_SendMessage(
        chatIds[0],
        0,
        nullptr,
        nullptr,
        Mock_InputMessageText(
            make_object<formattedText>("message2", std::vector<object_ptr<textEntity>>()),
            false
        )
    ));
}

TEST_F(FileTransferTest, SendFileToNonContact_CreatePrivateChatFail)
{
    const char *const PATH   = "/path";
    setFakeFileSize(PATH, 10000);

    login();

    // Normaly users without chats have to be group chat members, but for the test it doesn't matter
    tgl.update(standardUpdateUser(1));

    pluginInfo().send_file(
        connection,
        (userFirstNames[1] + " " + userLastNames[1]).c_str(),
        PATH
    );
    prpl.verifyEvents(
        XferAcceptedEvent(userFirstNames[1] + " " + userLastNames[1], PATH)
    );
    tgl.verifyRequest(createPrivateChat(userIds[1], false));

    tgl.reply(make_object<error>(100, "error"));
    prpl.verifyEvents(XferLocalCancelEvent(PATH));
}

TEST_F(
    FileTransferTest,
    SendFileToNonContact_CreatePrivateChatFailDisconnectsSafely)
{
    const char *const PATH = "/path";
    setFakeFileSize(PATH, 10000);

    login();
    tgl.update(standardUpdateUser(1));

    pluginInfo().send_file(
        connection,
        (userFirstNames[1] + " " + userLastNames[1]).c_str(),
        PATH);
    prpl.verifyEvents(
        XferAcceptedEvent(
            userFirstNames[1] + " " + userLastNames[1],
            PATH));
    tgl.verifyRequest(
        createPrivateChat(userIds[1], false));

    prpl.onNextEvent(
        [this](PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferLocalCancel,
                type);
            pluginInfo().close(connection);
        });
    tgl.reply(make_object<error>(100, "error"));

    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.verifyEvents(XferLocalCancelEvent(PATH));
    tgl.verifyNoRequests();
}

TEST_F(FileTransferTest, SendFileToNonContact_TurboCancel)
{
    const char *const PATH   = "/path";
    setFakeFileSize(PATH, 10000);

    login();

    // Normaly users without chats have to be group chat members, but for the test it doesn't matter
    tgl.update(standardUpdateUser(1));

    pluginInfo().send_file(
        connection,
        (userFirstNames[1] + " " + userLastNames[1]).c_str(),
        PATH
    );
    prpl.verifyEvents(
        XferAcceptedEvent(userFirstNames[1] + " " + userLastNames[1], PATH)
    );
    tgl.verifyRequest(createPrivateChat(userIds[1], false));

    purple_xfer_cancel_local(prpl.getLastXfer());
    prpl.discardEvents();

    tgl.update(standardPrivateChat(0));
    tgl.reply(makeChat(
        chatIds[0],
        make_object<chatTypePrivate>(userIds[0]),
        userFirstNames[0] + " " + userLastNames[0],
        nullptr, 0, 0, 0
    ));
}

TEST_F(
    FileTransferTest,
    SendFileToNonContact_CanceledPendingRequestDisconnectsSafely)
{
    const char *const PATH = "/path";
    setFakeFileSize(PATH, 10000);

    login();
    tgl.update(standardUpdateUser(1));

    pluginInfo().send_file(
        connection,
        (userFirstNames[1] + " " + userLastNames[1]).c_str(),
        PATH);
    prpl.verifyEvents(
        XferAcceptedEvent(
            userFirstNames[1] + " " + userLastNames[1],
            PATH));
    tgl.verifyRequest(
        createPrivateChat(userIds[1], false));

    PurpleXfer *upload = prpl.getLastXfer();
    ASSERT_NE(nullptr, upload);
    purple_xfer_cancel_local(upload);
    prpl.verifyEvents(XferLocalCancelEvent(PATH));

    pluginInfo().close(connection);
    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
}

TEST_F(FileTransferTest, ReceiveDocument_StandardTransfer_TinyFile)
{
    const int64_t messageId = 1;
    const int32_t date      = 10001;
    const int32_t fileId    = 1234;
    uint8_t       data[]    = {1, 2, 3, 4, 5};
    const char *outputFileName = ".test_download";

    setUiName("spectrum"); // No longer pidgin - now downloads will use libpurple transfers
    loginWithOneContact();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        messageId,
        userIds[0],
        chatIds[0],
        false,
        date,
        make_object<messageDocument>(
            make_object<document>(
                "doc.file.name", "mime/type", nullptr, nullptr,
                make_object<file>(
                    fileId, 10000, 10000,
                    make_object<localFile>("", true, true, false, false, 0, 0, 0),
                    make_object<remoteFile>("beh", "bleh", false, true, 10000)
                )
            ),
            make_object<formattedText>("document", std::vector<object_ptr<textEntity>>())
        )
    )));

    // TODO: Read receipt is not sent. It's a bug of sorts but it doesn't really matter.
    // tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {messageId}, true));

    prpl.verifyEvents(
        XferRequestEvent(PURPLE_XFER_RECEIVE, purpleUserName(0).c_str(), "doc.file.name")
    );

    purple_xfer_request_accepted(prpl.getLastXfer(), outputFileName);
    prpl.verifyEvents(
        XferAcceptedEvent(purpleUserName(0), outputFileName),
        XferStartEvent(outputFileName)
    );

    tgl.verifyRequest(downloadFile(fileId, 1, 0, 0, true));

    char *tdlibFileName = NULL;
    int fd = g_file_open_tmp("tdlib_test_XXXXXX", &tdlibFileName, NULL);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ((ssize_t)sizeof(data), write(fd, data, sizeof(data)));
    ::close(fd);

    tgl.reply(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(tdlibFileName, true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));
    tgl.update(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(tdlibFileName, true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));

    prpl.verifyEvents(
        XferWriteFileEvent(outputFileName, data, sizeof(data)),
        XferCompletedEvent(outputFileName, TRUE, sizeof(data)),
        XferEndEvent(outputFileName)
    );

    remove(tdlibFileName);
    g_free(tdlibFileName);
}

TEST_F(FileTransferTest, ReceiveDocument_StandardTransfer_Progress)
{
    const int64_t messageId = 1;
    const int32_t date      = 10001;
    const int32_t fileId    = 1234;
    uint8_t       data[]    = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const char *outputFileName = ".test_download";

    setUiName("spectrum"); // No longer pidgin - now downloads will use libpurple transfers
    loginWithOneContact();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        messageId,
        userIds[0],
        chatIds[0],
        false,
        date,
        make_object<messageDocument>(
            make_object<document>(
                "doc.file.name", "mime/type", nullptr, nullptr,
                make_object<file>(
                    fileId, 10000, 10000,
                    make_object<localFile>("", true, true, false, false, 0, 0, 0),
                    make_object<remoteFile>("beh", "bleh", false, true, 10000)
                )
            ),
            make_object<formattedText>("document", std::vector<object_ptr<textEntity>>())
        )
    )));
    // TODO: Read receipt is not sent. It's a bug of sorts but it doesn't really matter.
    // tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {messageId}, true));
    prpl.verifyEvents(
        XferRequestEvent(PURPLE_XFER_RECEIVE, purpleUserName(0).c_str(), "doc.file.name")
    );

    purple_xfer_request_accepted(prpl.getLastXfer(), outputFileName);
    prpl.verifyEvents(
        XferAcceptedEvent(purpleUserName(0), outputFileName),
        XferStartEvent(outputFileName)
    );

    tgl.verifyRequest(downloadFile(fileId, 1, 0, 0, true));

    char *tdlibFileName = NULL;
    int fd = g_file_open_tmp("tdlib_test_XXXXXX", &tdlibFileName, NULL);
    ASSERT_TRUE(fd >= 0);
    ASSERT_EQ((ssize_t)sizeof(data), write(fd, data, sizeof(data)));
    ::close(fd);

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(tdlibFileName, true, true, true, false, 0, 0, 2000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));
    prpl.verifyEvents(XferProgressEvent(outputFileName, 2000));

    tgl.reply(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(tdlibFileName, true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));
    tgl.update(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(tdlibFileName, true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));

    prpl.verifyEvents(
        XferWriteFileEvent(outputFileName, data, 10),
        XferWriteFileEvent(outputFileName, data+10, sizeof(data)-10),
        XferCompletedEvent(outputFileName, TRUE, sizeof(data)),
        XferEndEvent(outputFileName)
    );

    remove(tdlibFileName);
    g_free(tdlibFileName);
}

TEST_F(
    FileTransferTest,
    DuplicateInlineDownloadsCompleteTheirExactTransfers)
{
    constexpr int32_t FileId = 1234;
    constexpr int32_t FirstDate = 10001;
    constexpr int32_t SecondDate = 10002;
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourHyperlink);
    loginWithOneContact();

    const InlineReceive first =
        beginTimedOutInlineReceive(
            1, FileId, FirstDate, "first photo");
    const InlineReceive second =
        beginTimedOutInlineReceive(
            2, FileId, SecondDate, "second photo");

    tgl.update(make_object<updateFile>(
        activeDownload(FileId, 2000)));
    prpl.verifyEvents(
        XferStartEvent(first.tempFileName),
        XferProgressEvent(first.tempFileName, 2000),
        XferStartEvent(second.tempFileName),
        XferProgressEvent(second.tempFileName, 2000));
    ASSERT_TRUE(g_file_test(
        first.tempFileName.c_str(), G_FILE_TEST_EXISTS));
    ASSERT_TRUE(g_file_test(
        second.tempFileName.c_str(), G_FILE_TEST_EXISTS));

    tgl.reply(
        second.requestId,
        completedDownload(FileId, "/second-inline"));
    prpl.verifyEvents(
        XferCompletedEvent(
            second.tempFileName, TRUE, 10000),
        XferEndEvent(second.tempFileName),
        ServGotImEvent(
            connection, purpleUserName(0),
            "<img src=\"file:///second-inline\">",
            (PurpleMessageFlags)(
                PURPLE_MESSAGE_RECV |
                PURPLE_MESSAGE_IMAGES),
            SecondDate));
    EXPECT_TRUE(g_file_test(
        first.tempFileName.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test(
        second.tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();

    tgl.reply(
        first.requestId,
        completedDownload(FileId, "/first-inline"));
    prpl.verifyEvents(
        XferCompletedEvent(
            first.tempFileName, TRUE, 10000),
        XferEndEvent(first.tempFileName),
        ServGotImEvent(
            connection, purpleUserName(0),
            "<img src=\"file:///first-inline\">",
            (PurpleMessageFlags)(
                PURPLE_MESSAGE_RECV |
                PURPLE_MESSAGE_IMAGES),
            FirstDate));
    EXPECT_FALSE(g_file_test(
        first.tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();
}

TEST_F(
    FileTransferTest,
    CancelOneDuplicateInlineDownloadKeepsSiblingActive)
{
    constexpr int32_t FileId = 1234;
    constexpr int32_t SecondDate = 10002;
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourHyperlink);
    loginWithOneContact();

    const InlineReceive first =
        beginTimedOutInlineReceive(
            1, FileId, 10001, "first photo");
    const InlineReceive second =
        beginTimedOutInlineReceive(
            2, FileId, SecondDate, "second photo");
    tgl.update(make_object<updateFile>(
        activeDownload(FileId, 2000)));
    prpl.verifyEvents(
        XferStartEvent(first.tempFileName),
        XferProgressEvent(first.tempFileName, 2000),
        XferStartEvent(second.tempFileName),
        XferProgressEvent(second.tempFileName, 2000));

    purple_xfer_cancel_local(first.xfer);
    prpl.verifyEvents(
        XferLocalCancelEvent(first.tempFileName));
    tgl.verifyNoRequests();
    EXPECT_FALSE(g_file_test(
        first.tempFileName.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_TRUE(g_file_test(
        second.tempFileName.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_EQ(
        PURPLE_XFER_STATUS_STARTED,
        purple_xfer_get_status(second.xfer));

    tgl.reply(
        first.requestId,
        completedDownload(FileId, "/canceled-inline"));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.reply(
        second.requestId,
        completedDownload(FileId, "/surviving-inline"));
    prpl.verifyEvents(
        XferCompletedEvent(
            second.tempFileName, TRUE, 10000),
        XferEndEvent(second.tempFileName),
        ServGotImEvent(
            connection, purpleUserName(0),
            "<img src=\"file:///surviving-inline\">",
            (PurpleMessageFlags)(
                PURPLE_MESSAGE_RECV |
                PURPLE_MESSAGE_IMAGES),
            SecondDate));
    EXPECT_FALSE(g_file_test(
        second.tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();
}

TEST_F(
    FileTransferTest,
    MixedInlineAndStandardDownloadsKeepExactOwnership)
{
    constexpr int32_t FileId = 1234;
    constexpr int32_t InlineDate = 10001;
    constexpr int32_t StandardDate = 10002;
    const char *const OutputFileName =
        ".test_mixed_receive_download";
    const uint8_t contents[] = {1, 2, 3, 4, 5};

    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourHyperlink);
    loginWithOneContact();

    const InlineReceive inlineReceive =
        beginTimedOutInlineReceive(
            1, FileId, InlineDate, "inline photo");

    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourStandard);
    const StandardReceive standardReceive =
        beginStandardReceive(
            2, FileId, StandardDate,
            "standard document", OutputFileName);

    tgl.update(make_object<updateFile>(
        activeDownload(FileId, 2000)));
    prpl.verifyEvents(
        XferStartEvent(inlineReceive.tempFileName),
        XferProgressEvent(
            inlineReceive.tempFileName, 2000),
        XferProgressEvent(OutputFileName, 2000));

    char *tdlibFileName = nullptr;
    const int fd = g_file_open_tmp(
        "tdlib_mixed_receive_XXXXXX",
        &tdlibFileName, nullptr);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(
        static_cast<ssize_t>(sizeof(contents)),
        write(fd, contents, sizeof(contents)));
    ::close(fd);

    tgl.reply(
        standardReceive.requestId,
        completedDownload(FileId, tdlibFileName));
    prpl.verifyEvents(
        XferWriteFileEvent(
            OutputFileName, contents,
            sizeof(contents)),
        XferCompletedEvent(
            OutputFileName, TRUE,
            sizeof(contents)),
        XferEndEvent(OutputFileName));
    EXPECT_TRUE(g_file_test(
        inlineReceive.tempFileName.c_str(),
        G_FILE_TEST_EXISTS));

    tgl.reply(
        inlineReceive.requestId,
        completedDownload(FileId, "/mixed-inline"));
    prpl.verifyEvents(
        XferCompletedEvent(
            inlineReceive.tempFileName, TRUE, 10000),
        XferEndEvent(inlineReceive.tempFileName),
        ServGotImEvent(
            connection, purpleUserName(0),
            "<img src=\"file:///mixed-inline\">",
            (PurpleMessageFlags)(
                PURPLE_MESSAGE_RECV |
                PURPLE_MESSAGE_IMAGES),
            InlineDate));
    EXPECT_FALSE(g_file_test(
        inlineReceive.tempFileName.c_str(),
        G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();

    remove(tdlibFileName);
    g_free(tdlibFileName);
}

TEST_F(
    FileTransferTest,
    CancelStandardDownloadKeepsSharedInlineActive)
{
    constexpr int32_t FileId = 1234;
    constexpr int32_t InlineDate = 10001;
    const char *const OutputFileName =
        ".test_canceled_shared_standard";
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourHyperlink);
    loginWithOneContact();

    const InlineReceive inlineReceive =
        beginTimedOutInlineReceive(
            1, FileId, InlineDate, "inline photo");
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourStandard);
    const StandardReceive standardReceive =
        beginStandardReceive(
            2, FileId, 10002, "standard document",
            OutputFileName);

    tgl.update(make_object<updateFile>(
        activeDownload(FileId, 2000)));
    prpl.verifyEvents(
        XferStartEvent(inlineReceive.tempFileName),
        XferProgressEvent(
            inlineReceive.tempFileName, 2000),
        XferProgressEvent(OutputFileName, 2000));

    purple_xfer_cancel_local(standardReceive.xfer);
    prpl.verifyEvents(
        XferLocalCancelEvent(OutputFileName));
    tgl.verifyNoRequests();
    EXPECT_TRUE(g_file_test(
        inlineReceive.tempFileName.c_str(),
        G_FILE_TEST_EXISTS));
    EXPECT_EQ(
        PURPLE_XFER_STATUS_STARTED,
        purple_xfer_get_status(inlineReceive.xfer));

    tgl.reply(
        standardReceive.requestId,
        completedDownload(FileId, "/canceled-standard"));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.reply(
        inlineReceive.requestId,
        completedDownload(FileId, "/surviving-mixed-inline"));
    prpl.verifyEvents(
        XferCompletedEvent(
            inlineReceive.tempFileName, TRUE, 10000),
        XferEndEvent(inlineReceive.tempFileName),
        ServGotImEvent(
            connection, purpleUserName(0),
            "<img src=\"file:///surviving-mixed-inline\">",
            (PurpleMessageFlags)(
                PURPLE_MESSAGE_RECV |
                PURPLE_MESSAGE_IMAGES),
            InlineDate));
    EXPECT_FALSE(g_file_test(
        inlineReceive.tempFileName.c_str(),
        G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();
}

TEST_F(
    FileTransferTest,
    StandardCompletionCallbackMayEndTransferDirectly)
{
    constexpr int32_t FileId = 1234;
    const char *const OutputFileName =
        ".test_standard_direct_end";
    const uint8_t contents[] = {1, 2, 3, 4, 5};
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourStandard);
    loginWithOneContact();

    const StandardReceive receive =
        beginStandardReceive(
            1, FileId, 10001, "standard document",
            OutputFileName);

    char *tdlibFileName = nullptr;
    const int fd = g_file_open_tmp(
        "tdlib_standard_direct_end_XXXXXX",
        &tdlibFileName, nullptr);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(
        static_cast<ssize_t>(sizeof(contents)),
        write(fd, contents, sizeof(contents)));
    ::close(fd);

    bool endedDirectly = false;
    prpl.onNextEvent(
        [this, &receive, &endedDirectly](
            PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferWriteFile, type);
            prpl.onNextEvent(
                [&receive, &endedDirectly](
                    PurpleEventType nestedType) {
                    EXPECT_EQ(
                        PurpleEventType::XferCompleted,
                        nestedType);
                    endedDirectly = true;
                    purple_xfer_end(receive.xfer);
                });
        });
    tgl.reply(
        receive.requestId,
        completedDownload(FileId, tdlibFileName));

    EXPECT_TRUE(endedDirectly);
    prpl.verifyEvents(
        XferWriteFileEvent(
            OutputFileName, contents,
            sizeof(contents)),
        XferCompletedEvent(
            OutputFileName, TRUE,
            sizeof(contents)),
        XferEndEvent(OutputFileName));
    tgl.verifyNoRequests();

    remove(tdlibFileName);
    g_free(tdlibFileName);
}

TEST_F(
    FileTransferTest,
    DuplicateInlineProgressPinsSecondTransferAcrossDisconnect)
{
    constexpr int32_t FileId = 1234;
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourHyperlink);
    loginWithOneContact();

    const InlineReceive first =
        beginTimedOutInlineReceive(
            1, FileId, 10001, "first photo");
    const InlineReceive second =
        beginTimedOutInlineReceive(
            2, FileId, 10002, "second photo");

    tgl.update(make_object<updateFile>(
        activeDownload(FileId, 2000)));
    prpl.verifyEvents(
        XferStartEvent(first.tempFileName),
        XferProgressEvent(first.tempFileName, 2000),
        XferStartEvent(second.tempFileName),
        XferProgressEvent(second.tempFileName, 2000));

    bool progressed = false;
    bool completed = false;
    prpl.onNextEvent(
        [this, &first, FileId,
         &progressed, &completed](
            PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferProgress, type);
            progressed = true;
            prpl.onNextEvent(
                [this, &completed](
                    PurpleEventType nestedType) {
                    EXPECT_EQ(
                        PurpleEventType::XferCompleted,
                        nestedType);
                    completed = true;
                    pluginInfo().close(connection);
                });
            tgl.reply(
                first.requestId,
                completedDownload(
                    FileId, "/first-completed"));
        });
    tgl.update(make_object<updateFile>(
        activeDownload(FileId, 5000)));

    EXPECT_TRUE(progressed);
    EXPECT_TRUE(completed);
    EXPECT_EQ(
        nullptr,
        purple_connection_get_protocol_data(connection));
    prpl.verifyEvents(
        XferProgressEvent(first.tempFileName, 5000),
        XferCompletedEvent(
            first.tempFileName, TRUE, 10000),
        XferLocalCancelEvent(second.tempFileName),
        XferEndEvent(first.tempFileName));
    EXPECT_FALSE(g_file_test(
        first.tempFileName.c_str(), G_FILE_TEST_EXISTS));
    EXPECT_FALSE(g_file_test(
        second.tempFileName.c_str(), G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();

    tgl.reply(
        second.requestId,
        completedDownload(FileId, "/second-late"));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
}

TEST_F(
    FileTransferTest,
    InlineCompletionCallbackMayEndTransferDirectly)
{
    constexpr int32_t FileId = 1234;
    constexpr int32_t Date = 10001;
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourHyperlink);
    loginWithOneContact();

    const InlineReceive receive =
        beginTimedOutInlineReceive(
            1, FileId, Date, "photo");
    tgl.update(make_object<updateFile>(
        activeDownload(FileId, 2000)));
    prpl.verifyEvents(
        XferStartEvent(receive.tempFileName),
        XferProgressEvent(receive.tempFileName, 2000));

    bool endedDirectly = false;
    prpl.onNextEvent(
        [&receive, &endedDirectly](
            PurpleEventType type) {
            EXPECT_EQ(
                PurpleEventType::XferCompleted, type);
            endedDirectly = true;
            purple_xfer_end(receive.xfer);
        });
    tgl.reply(
        receive.requestId,
        completedDownload(FileId, "/direct-end"));

    EXPECT_TRUE(endedDirectly);
    prpl.verifyEvents(
        XferCompletedEvent(
            receive.tempFileName, TRUE, 10000),
        XferEndEvent(receive.tempFileName),
        ServGotImEvent(
            connection, purpleUserName(0),
            "<img src=\"file:///direct-end\">",
            (PurpleMessageFlags)(
                PURPLE_MESSAGE_RECV |
                PURPLE_MESSAGE_IMAGES),
            Date));
    EXPECT_FALSE(g_file_test(
        receive.tempFileName.c_str(),
        G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();
}

TEST_F(
    FileTransferTest,
    InlineDownloadErrorRemoteCancelsExactProgressTransfer)
{
    constexpr int32_t FileId = 1234;
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourHyperlink);
    loginWithOneContact();

    const InlineReceive receive =
        beginTimedOutInlineReceive(
            1, FileId, 10001, "photo");
    tgl.update(make_object<updateFile>(
        activeDownload(FileId, 2000)));
    prpl.verifyEvents(
        XferStartEvent(receive.tempFileName),
        XferProgressEvent(receive.tempFileName, 2000));

    tgl.reply(
        receive.requestId,
        make_object<error>(400, "download failed"));
    prpl.verifyEvents(
        XferRemoteCancelEvent(receive.tempFileName));
    EXPECT_FALSE(g_file_test(
        receive.tempFileName.c_str(),
        G_FILE_TEST_EXISTS));
    tgl.verifyNoRequests();
}

TEST_F(
    FileTransferTest,
    InlineFailureBeforeTimeoutReleasesReadyFollower)
{
    constexpr int32_t FileId = 1234;
    constexpr int32_t PhotoDate = 10001;
    constexpr int32_t FollowerDate = 10002;
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourHyperlink);
    loginWithOneContact();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        1, userIds[0], chatIds[0], false, PhotoDate,
        remotePhoto(FileId, "failed photo"))));
    const uint64_t requestId =
        tgl.verifyRequest(
            downloadFile(FileId, 1, 0, 0, true));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateNewMessage>(makeMessage(
        2, userIds[0], chatIds[0], false, FollowerDate,
        makeTextMessage("ready follower"))));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.reply(
        requestId,
        make_object<error>(400, "download failed"));
    prpl.verifyEvents(
        ServGotImEvent(
            connection, purpleUserName(0), "failed photo",
            PURPLE_MESSAGE_RECV, PhotoDate),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " +
                userLastNames[0] +
                ": Downloading photo",
            PURPLE_MESSAGE_SYSTEM, PhotoDate),
        ServGotImEvent(
            connection, purpleUserName(0),
            "ready follower",
            PURPLE_MESSAGE_RECV, FollowerDate));
    tgl.verifyRequest(*Mock_ViewMessages(
        chatIds[0], {1, 2}, true));

    runTimeouts();
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
}

TEST_F(
    FileTransferTest,
    CancelInlineFromStartCallbackCleansReopenedDestination)
{
    constexpr int32_t FileId = 1234;
    purple_account_set_string(
        account, AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourHyperlink);
    loginWithOneContact();

    const InlineReceive receive =
        beginTimedOutInlineReceive(
            1, FileId, 10001, "photo");
    ASSERT_TRUE(g_file_test(
        receive.tempFileName.c_str(),
        G_FILE_TEST_EXISTS));

    bool canceled = false;
    prpl.onNextEvent(
        [this, &receive, &canceled](
            PurpleEventType type) {
            EXPECT_EQ(PurpleEventType::XferStart, type);
            prpl.onNextEvent(
                [&canceled](
                    PurpleEventType nestedType) {
                    EXPECT_EQ(
                        PurpleEventType::XferLocalCancel,
                        nestedType);
                    canceled = true;
                });
            purple_xfer_cancel_local(receive.xfer);
        });
    tgl.update(make_object<updateFile>(
        activeDownload(FileId, 2000)));

    EXPECT_TRUE(canceled);
    prpl.verifyEvents(
        XferStartEvent(receive.tempFileName),
        XferLocalCancelEvent(receive.tempFileName));
    EXPECT_FALSE(g_file_test(
        receive.tempFileName.c_str(),
        G_FILE_TEST_EXISTS));
    tgl.verifyRequest(
        cancelDownloadFile(FileId, false));

    tgl.update(make_object<updateFile>(
        activeDownload(FileId, 5000)));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.reply(
        receive.requestId,
        completedDownload(FileId, "/late-start-cancel"));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();
}

TEST_F(FileTransferTest, Photo_LongDownload_StartandDownloadsConfigured)
{
    purple_account_set_string(account, "download-behaviour", "file-transfer");
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
    uint64_t downloadReqId = tgl.verifyRequest(
        downloadFile(fileId, 1, 0, 0, true)
    );
    prpl.verifyNoEvents();

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, true, false, 0, 0, 2000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));

    tgl.runTimeouts();
    prpl.verifyEvents(
        ServGotImEvent(connection, purpleUserName(0), "photo", PURPLE_MESSAGE_RECV, date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] + ": Downloading photo",
            PURPLE_MESSAGE_SYSTEM, date
        )
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, true, false, 0, 0, 5000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));
    tgl.reply(downloadReqId, make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, false, true, 0, 10000, 10000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    ));
    prpl.verifyEvents(ServGotImEvent(
        connection,
        purpleUserName(0),
        "<img src=\"file:///path\">",
        (PurpleMessageFlags)(PURPLE_MESSAGE_RECV | PURPLE_MESSAGE_IMAGES),
        date
    ));
}

TEST_F(FileTransferTest, ActiveUploadAtLogout_BeforeUploadResponse)
{
    const char *const PATH = "/path";
    loginWithOneContact();

    setFakeFileSize(PATH, 9000);
    pluginInfo().send_file(connection, purpleUserName(0).c_str(), PATH);
    prpl.verifyEvents(XferAcceptedEvent(purpleUserName(0), PATH));
    tgl.verifyRequest(uploadFile(
        make_object<inputFileLocal>(PATH),
        make_object<fileTypeDocument>(),
        1
    ));

    pluginInfo().close(connection);
    prpl.verifyEvents(XferLocalCancelEvent(PATH));
}

TEST_F(FileTransferTest, ActiveUploadAtLogout_AfterUploadResponse)
{
    const char *const PATH = "/path";
    const int32_t     fileId = 1234;
    loginWithOneContact();

    setFakeFileSize(PATH, 9000);
    pluginInfo().send_file(connection, purpleUserName(0).c_str(), PATH);
    prpl.verifyEvents(XferAcceptedEvent(purpleUserName(0), PATH));
    tgl.verifyRequest(uploadFile(
        make_object<inputFileLocal>(PATH),
        make_object<fileTypeDocument>(),
        1
    ));

    tgl.reply(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>(PATH, false, false, false, true, 0, 10000, 10000),
        make_object<remoteFile>("", "", true, false, 0)
    ));
    prpl.verifyEvents(
        XferStartEvent(PATH),
        XferProgressEvent(PATH, 0)
    );
    pluginInfo().close(connection);
    prpl.verifyEvents(XferLocalCancelEvent(PATH));
}

TEST_F(FileTransferTest, ActiveUploadAtLogout_SendFileToNonContact_LogoutBeforeChatResponse)
{
    const char *const PATH   = "/path";
    setFakeFileSize(PATH, 10000);

    login();

    // Normaly users without chats have to be group chat members, but for the test it doesn't matter
    tgl.update(standardUpdateUser(0));

    // Send ﬁle successfully
    pluginInfo().send_file(
        connection,
        (userFirstNames[0] + " " + userLastNames[0]).c_str(),
        PATH
    );
    prpl.verifyEvents(
        XferAcceptedEvent(userFirstNames[0] + " " + userLastNames[0], PATH)
    );
    tgl.verifyRequest(createPrivateChat(userIds[0], false));

    pluginInfo().close(connection);
    prpl.verifyEvents(XferLocalCancelEvent(PATH));
}

TEST_F(FileTransferTest, ActiveDownloadAtLogout_StuckAtStart)
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

    tgl.update(make_object<updateFile>(make_object<file>(
        fileId, 10000, 10000,
        make_object<localFile>("/path", true, true, true, false, 0, 0, 2000),
        make_object<remoteFile>("beh", "bleh", false, true, 10000)
    )));

    tgl.runTimeouts();
    std::string tempFileName;
    prpl.verifyEvents(
        XferAcceptedEvent(purpleUserName(0), &tempFileName),
        XferStartEvent(&tempFileName),
        ServGotImEvent(connection, purpleUserName(0), "photo", PURPLE_MESSAGE_RECV, date),
        ConversationWriteEvent(
            purpleUserName(0), purpleUserName(0),
            userFirstNames[0] + " " + userLastNames[0] + ": Downloading photo",
            PURPLE_MESSAGE_SYSTEM, date
        )
    );
    tgl.verifyRequest(*Mock_ViewMessages(chatIds[0], {1}, true));

    pluginInfo().close(connection);
    prpl.verifyEvents(XferLocalCancelEvent(tempFileName));
}
