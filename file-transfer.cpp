#include "file-transfer.h"
#include "config.h"
#include "client-utils.h"
#include "format.h"
#include "receiving.h"
#include "sticker.h"
#include "purple-info.h"
#include <unistd.h>

enum {
    FILE_UPLOAD_PRIORITY = 1,
};

DownloadRequest::~DownloadRequest()
{
    if (tempFd >= 0)
        close(tempFd);
    if (!tempFileName.empty())
        remove(tempFileName.c_str());
}

bool saveImage(int id, char **fileName)
{
    *fileName = NULL;
    char *tempFileName = NULL;

    PurpleStoredImage *psi = purple_imgstore_find_by_id (id);
    if (!psi) {
        purple_debug_misc(config::pluginId, "Failed to send image: id %d not found\n", id);
        return false;
    }
    int fd = g_file_open_tmp("tdlib_upload_XXXXXX", &tempFileName, NULL);
    if (fd < 0) {
        purple_debug_misc(config::pluginId, "Failed to send image: could not create temporary file\n");
        return false;
    }
    ssize_t len = write(fd, purple_imgstore_get_data (psi), purple_imgstore_get_size (psi));
    close(fd);
    if (len != (ssize_t)purple_imgstore_get_size(psi)) {
        purple_debug_misc(config::pluginId, "Failed to send image: could not write temporary file\n");
        remove(tempFileName);
        g_free(tempFileName);
        return false;
    }

    *fileName = tempFileName;
    return true;
}

void startDocumentUpload(ChatTarget target, const std::string &filename, PurpleXfer *xfer,
                         TdTransceiver &transceiver, TdAccountData &account,
                         TdTransceiver::ResponseCb response)
{
    auto uploadRequest = td::td_api::make_object<td::td_api::preliminaryUploadFile>();
    uploadRequest->file_ = td::td_api::make_object<td::td_api::inputFileLocal>(filename);
    uploadRequest->file_type_ = td::td_api::make_object<td::td_api::fileTypeDocument>();
    uploadRequest->priority_ = FILE_UPLOAD_PRIORITY;
    purple_xfer_ref(xfer);
    uint64_t requestId = transceiver.sendQuery(std::move(uploadRequest), response);
    account.addPendingRequest<UploadRequest>(requestId, xfer, target);
}

static void reportDocumentUploadProgress(
    const td::td_api::file &file, PurpleXfer *xfer);

void startDocumentUploadProgress(ChatTarget target, PurpleXfer *xfer, const td::td_api::file &file,
                                 TdTransceiver &transceiver, TdAccountData &account,
                                 TdTransceiver::ResponseCb sendMessageResponse)
{
    purple_debug_misc(config::pluginId, "Got file id %d for uploading %s\n", (int)file.id_,
                      purple_xfer_get_local_filename(xfer));
    account.addFileTransfer(
        file.id_, xfer, target,
        ReceiveTransferKind::None, 0);

    if (file.remote_ && file.remote_->is_uploading_active_) {
        // Transfer events can synchronously cancel the transfer or disconnect
        // the account. The pending-upload reference keeps this transfer alive.
        purple_xfer_ref(xfer);
        reportDocumentUploadProgress(file, xfer);
        purple_xfer_unref(xfer);
    } else {
        updateFileTransferProgress(
            file, transceiver, account,
            sendMessageResponse);
    }
}

void uploadResponseError(PurpleXfer *xfer, const std::string &message, TdAccountData &account)
{
    const PurpleXferType type = purple_xfer_get_type(xfer);
    PurpleAccount *purpleAccount = account.purpleAccount;
    const char *remoteUser = purple_xfer_get_remote_user(xfer);
    const std::string who = remoteUser ? remoteUser : "";

    // Error notification handlers can synchronously disconnect the account.
    // Capture everything above and only touch the independently referenced
    // transfer afterwards.
    purple_xfer_error(
        type, purpleAccount, who.c_str(), message.c_str());
    if (xfer->data && !purple_xfer_is_canceled(xfer))
        purple_xfer_cancel_remote(xfer);
    purple_xfer_unref(xfer);
}

static ChatTarget resolveDocumentUploadTarget(
    ChatTarget target, const TdAccountData &account)
{
    if (!target.valid())
        return ChatTarget();

    const td::td_api::chat *chat =
        account.getChat(target.chatId());
    if (!chat || !chat->type_)
        return ChatTarget();

    if (target.isForumTopic()) {
        if (target.forumTopicId() == ForumTopicId::general()) {
            if (isEligibleForumParent(account, *chat))
                return target;
            if (account.isGroupChatWithMembership(*chat))
                return ChatTarget::chat(target.chatId());
            return ChatTarget();
        }

        if (!isEligibleForumParent(account, *chat))
            return ChatTarget();

        const TdAccountData::ForumTopicState *topic =
            account.findForumTopic(target);
        // A locally closed Purple room marks the topic inactive, but the
        // immutable upload target remains valid. Only authoritative deletion
        // or loss of the forum parent cancels the final send.
        return (topic && !topic->deleted)
                   ? target
                   : ChatTarget();
    }

    if (isEligibleForumParent(account, *chat)) {
        return ChatTarget::forumTopic(
            target.chatId(), ForumTopicId::general());
    }

    const int32_t chatType = chat->type_->get_id();
    if (chatType == td::td_api::chatTypePrivate::ID ||
        chatType == td::td_api::chatTypeSecret::ID) {
        return target;
    }
    return account.isGroupChatWithMembership(*chat)
               ? target
               : ChatTarget();
}

static void reportDocumentUploadProgress(
    const td::td_api::file &file, PurpleXfer *upload)
{
    if (!file.remote_ || !file.remote_->is_uploading_active_)
        return;

    if (purple_xfer_get_status(upload) != PURPLE_XFER_STATUS_STARTED) {
        purple_debug_misc(
            config::pluginId, "Started uploading %s\n",
            purple_xfer_get_local_filename(upload));
        purple_xfer_start(upload, -1, NULL, 0);
    }
    if (upload->data && !purple_xfer_is_canceled(upload)) {
        const size_t fileSize = purple_xfer_get_size(upload);
        const size_t bytesSent = std::max(
            static_cast<td::td_api::int53>(0),
            file.remote_->uploaded_size_);
        purple_xfer_set_bytes_sent(
            upload, std::min(fileSize, bytesSent));
        if (upload->data && !purple_xfer_is_canceled(upload))
            purple_xfer_update_progress(upload);
    }
}

static void finishPurpleUpload(
    PurpleXfer *upload, bool sent)
{
    if (!upload)
        return;

    // The upload registry owns one operation reference. Purple's terminal
    // functions consume the original transfer reference, while callbacks may
    // synchronously consume it first. Release the operation reference last.
    if (!sent) {
        if (upload->data &&
            !purple_xfer_is_canceled(upload)) {
            purple_xfer_cancel_remote(upload);
        }
        purple_xfer_unref(upload);
        return;
    }

    if (!purple_xfer_is_canceled(upload) &&
        upload->data) {
        const size_t fileSize =
            purple_xfer_get_size(upload);
        purple_xfer_set_bytes_sent(
            upload, fileSize);
        purple_xfer_set_completed(upload, TRUE);

        if (!purple_xfer_is_canceled(upload) &&
            upload->data) {
            // A handler for XferEnd may itself cancel the transfer. Keep one
            // guard reference and account for that extra terminal unref.
            purple_xfer_ref(upload);
            purple_xfer_end(upload);
            if (!purple_xfer_is_canceled(upload))
                purple_xfer_unref(upload);
        }
    }
    purple_xfer_unref(upload);
}

static void finishDocumentUploads(
    const td::td_api::file &file,
    std::vector<TdAccountData::FileTransferInfo> uploads,
    TdTransceiver &transceiver, TdAccountData &account,
    TdTransceiver::ResponseCb sendMessageResponse)
{
    std::vector<bool> sent(uploads.size(), false);

    // Queue every Telegram send and register every response route before
    // invoking terminal Purple callbacks. The first callback may disconnect
    // the account, while all later transfers still need deterministic targets.
    for (size_t i = 0; i < uploads.size(); ++i) {
        const ChatTarget target =
            resolveDocumentUploadTarget(
                uploads[i].target, account);
        if (!target.valid()) {
            purple_debug_warning(
                config::pluginId,
                "Refusing document send because its chat target is no longer available\n");
            continue;
        }

        auto sendMessageRequest =
            td::td_api::make_object<td::td_api::sendMessage>();
        auto content =
            td::td_api::make_object<td::td_api::inputMessageDocument>();
        content->caption_ =
            td::td_api::make_object<td::td_api::formattedText>();
        content->document_ =
            td::td_api::make_object<td::td_api::inputDocument>(
                td::td_api::make_object<td::td_api::inputFileId>(
                    file.id_),
                nullptr, false);
        sendMessageRequest->input_message_content_ =
            std::move(content);
        sendMessageRequest->chat_id_ =
            target.chatId().value();
        sendMessageRequest->topic_id_ =
            makeMessageTopic(target);

        const uint64_t requestId = transceiver.sendQuery(
            std::move(sendMessageRequest),
            sendMessageResponse);
        account.addPendingRequest<SendMessageRequest>(
            requestId, target, nullptr);
        sent[i] = true;
    }

    // No account or transceiver access is allowed below this point.
    for (size_t i = 0; i < uploads.size(); ++i)
        finishPurpleUpload(uploads[i].xfer, sent[i]);
}

static void cancelDocumentUploads(
    std::vector<TdAccountData::FileTransferInfo> uploads)
{
    for (const TdAccountData::FileTransferInfo &transfer: uploads)
        finishPurpleUpload(transfer.xfer, false);
}

struct DownloadData {
    TdAccountData *account;
    TdTransceiver *transceiver;

    DownloadData(TdAccountData &account, TdTransceiver &transceiver)
    : account(&account), transceiver(&transceiver) {}
};

static bool isCurrentInlineDownloadRequest(
    const DownloadRequest &request)
{
    if (request.transferKind !=
        ReceiveTransferKind::InlineProgress) {
        return true;
    }
    const PendingContentHandle &pendingContent =
        request.pendingContent;
    return pendingContent.currentAndNotCancelled();
}

static bool isSameClient(
    PurpleAccount *purpleAccount,
    PurpleTdClient *client)
{
    return !client ||
           getTdClient(purpleAccount) == client;
}

static void nop(PurpleXfer *xfer)
{
}

static void closeReceiveDestination(PurpleXfer *xfer)
{
    if (xfer->dest_fp) {
        fclose(xfer->dest_fp);
        xfer->dest_fp = NULL;
    }
}

static void cleanupCanceledInlineDownloadStart(
    PurpleXfer *xfer)
{
    if (!purple_xfer_is_canceled(xfer))
        return;

    // libpurple emits file-recv-start before begin_transfer opens the
    // destination. A synchronous disconnect can cancel the transfer during
    // that signal, after which begin_transfer still opens the file and calls
    // this start hook. Canceled transfer destruction does not close it.
    closeReceiveDestination(xfer);
    const char *path =
        purple_xfer_get_local_filename(xfer);
    if (path && *path)
        remove(path);
}

static void cancelDownload(PurpleXfer *xfer)
{
    std::unique_ptr<DownloadData> data(static_cast<DownloadData *>(xfer->data));
    xfer->data = NULL;
    if (!data) return;

    // DownloadRequest owns the inline transfer's temporary path. Close the
    // destination before extracting that request so the path can also be
    // removed on platforms that do not unlink open files.
    closeReceiveDestination(xfer);

    TdAccountData::FileTransferInfo transfer;
    if (data->account->getFileTransferInfo(
            xfer, transfer)) {
        purple_debug_misc(config::pluginId, "Cancelling download of %s (file id %d)\n",
                            purple_xfer_get_local_filename(xfer), transfer.fileId);

        if (transfer.requestId == 0 ||
            transfer.receiveKind ==
                ReceiveTransferKind::None) {
            data->account->removeFileTransfer(
                transfer.fileId, xfer);
            return;
        }

        TdAccountData::FileTransferInfo detached;
        data->account->extractFileTransferForRequest(
            transfer.requestId, transfer.receiveKind,
            detached);
        std::unique_ptr<DownloadRequest> request =
            data->account->
                getPendingRequest<DownloadRequest>(
                    transfer.requestId);
        if (!request)
            return;

        // TDLib cancellation is file-scoped, while a Purple transfer and
        // downloadFile response are request-scoped. Suppress this exact
        // response immediately, but leave the underlying file download
        // running while any sibling request still needs it.
        if (!data->account->findDownloadRequestIds(
                transfer.fileId).empty()) {
            return;
        }

        auto cancelRequest =
            td::td_api::make_object<
                td::td_api::cancelDownloadFile>();
        cancelRequest->file_id_ = transfer.fileId;
        cancelRequest->only_if_pending_ = false;
        data->transceiver->sendQuery(
            std::move(cancelRequest), nullptr);
    }
}

static bool finishInlineDownloadProgress(
    DownloadRequest &downloadReq, bool succeeded,
    TdAccountData &account)
{
    PurpleAccount *purpleAccount = account.purpleAccount;
    PurpleTdClient *client =
        getTdClient(purpleAccount);
    const size_t fileSize = downloadReq.fileSize;

    // A Telegram file id may be shared by unrelated inline and standard
    // downloads. Terminal ownership is request-scoped: detach and pin only
    // the fake progress transfer created for this request.
    TdAccountData::FileTransferInfo transfer;
    PurpleXfer *download = nullptr;
    if (account.extractFileTransferForRequest(
            downloadReq.requestId,
            ReceiveTransferKind::InlineProgress,
            transfer)) {
        download = transfer.xfer;
        purple_xfer_ref(download);
        closeReceiveDestination(download);
    }

    if (downloadReq.tempFd >= 0) {
        close(downloadReq.tempFd);
        downloadReq.tempFd = -1;
    }
    if (!downloadReq.tempFileName.empty()) {
        remove(downloadReq.tempFileName.c_str());
        downloadReq.tempFileName.clear();
    }

    // Account and request cleanup is complete. Only the independently pinned
    // PurpleXfer is touched below this point.
    if (download) {
        std::unique_ptr<DownloadData> data(
            static_cast<DownloadData *>(download->data));
        download->data = NULL;
        data.reset();

        if (!purple_xfer_is_canceled(download) &&
            succeeded) {
            purple_xfer_set_bytes_sent(download, fileSize);
            if (!purple_xfer_is_canceled(download))
                purple_xfer_set_completed(download, TRUE);
            // A synchronous completion callback is allowed to end the
            // transfer itself. In that case the core reference is already
            // consumed and only our operation guard remains.
            if (!purple_xfer_is_canceled(download) &&
                purple_xfer_get_end_time(download) == 0) {
                // XferEnd handlers may cancel the transfer and consume its
                // original reference before purple_xfer_end does the same.
                purple_xfer_ref(download);
                purple_xfer_end(download);
                if (!purple_xfer_is_canceled(download))
                    purple_xfer_unref(download);
            }
        } else if (!purple_xfer_is_canceled(download)) {
            // A failed TDLib response must not be reported as a successful
            // fake transfer, nor allowed to display an empty-path file.
            purple_xfer_cancel_remote(download);
        }
        purple_xfer_unref(download);
    }

    return isSameClient(purpleAccount, client);
}

static void inlineDownloadResponse(uint64_t requestId,
                                   td::td_api::object_ptr<td::td_api::Object> object,
                                   TdTransceiver &transceiver, TdAccountData &account)
{
    std::unique_ptr<DownloadRequest> request = account.getPendingRequest<DownloadRequest>(requestId);

    if (request &&
        request->transferKind ==
            ReceiveTransferKind::InlineProgress) {
        std::string path = getDownloadPath(object);
        if (!finishInlineDownloadProgress(
                *request, !path.empty(), account)) {
            return;
        }
        const PendingContentHandle pendingContent =
            request->pendingContent;
        if (path.empty()) {
            if (pendingContent.current() &&
                pendingContent.message.disposition() ==
                    PendingMessageState::Disposition::Queued) {
                IncomingMessage *pendingMessage =
                    account.pendingMessages.
                        findPendingMessage(pendingContent);
                if (pendingMessage) {
                    // The failed request is terminal. Reuse the existing
                    // timeout display path so it cannot block later messages
                    // or start another download.
                    pendingMessage->inlineDownloadTimeout =
                        true;
                    checkMessageReady(
                        pendingContent.message,
                        transceiver, account);
                }
            }
            return;
        }
        if (pendingContent.valid() &&
            (!pendingContent.current() ||
             pendingContent.message.disposition() ==
                 PendingMessageState::Disposition::Cancelled)) {
            return;
        }

        IncomingMessage *pendingMessage =
            pendingContent.valid()
            ? account.pendingMessages.findPendingMessage(
                  pendingContent)
            : nullptr;

        if (pendingMessage) {
            // Quick download response while message still in PendingMessageQueue
            const td::td_api::file *replacementFile = nullptr;

            if (pendingMessage->message && pendingMessage->message->content_ &&
                (pendingMessage->message->content_->get_id() == td::td_api::messageSticker::ID) &&
                isStickerAnimated(path))
            {
                if (shouldConvertAnimatedSticker(pendingMessage->messageInfo, account.purpleAccount)) {
                    pendingMessage->inlineDownloadComplete = true;
                    pendingMessage->inlineDownloadedFilePath = path;
                    StickerConversionThread *thread;
                    thread = new StickerConversionThread(account.purpleAccount, path, getChatId(*pendingMessage->message),
                                                         &pendingMessage->messageInfo,
                                                         pendingContent);
                    thread->startThread();
                    return;
                } else
                    replacementFile = pendingMessage->thumbnail.get();
            }

            if (replacementFile) {
                // TODO: if thumbnail already downloaded, mark ready and don't download
                downloadFileInline(replacementFile->id_, request->chatId, request->message,
                                   request->fileDescription, nullptr, transceiver, account,
                                   pendingContent);
                return;
            } else {
                pendingMessage->inlineDownloadComplete = true;
                pendingMessage->inlineDownloadedFilePath = path;
                checkMessageReady(
                    pendingContent.message,
                    transceiver, account);
                pendingMessage = nullptr;
            }
        } else if (
            !pendingContent.valid() ||
            pendingContent.message.disposition() ==
                PendingMessageState::Disposition::Released) {
            // Message no longer in PendingMessageQueue
            if (!path.empty())
                showDownloadedFileInline(request->chatId, request->message, path, NULL,
                                         request->fileDescription, std::move(request->thumbnail),
                                         transceiver, account,
                                         pendingContent);
        }
    }
}

static void discardUnregisteredInlineDownloadProgress(
    PurpleXfer *download, int tempFd,
    const std::string &tempFileName)
{
    if (tempFd >= 0)
        close(tempFd);
    if (!tempFileName.empty())
        remove(tempFileName.c_str());

    // The caller holds a guard reference in addition to Purple's original
    // transfer reference.
    if (!purple_xfer_is_canceled(download))
        purple_xfer_cancel_local(download);
    purple_xfer_unref(download);
}

static bool startInlineDownloadProgress(
    uint64_t requestId, TdTransceiver &transceiver,
    TdAccountData &account)
{
    DownloadRequest *request =
        account.findPendingRequest<DownloadRequest>(
            requestId);
    if (!request ||
        !isCurrentInlineDownloadRequest(*request)) {
        return false;
    }

    const int32_t fileId = request->fileId;
    const int32_t fileSize = request->fileSize;
    const int32_t downloadedSize =
        request->downloadedSize;
    const std::string fileDescription =
        request->fileDescription;
    const std::string who =
        getDownloadXferPeerName(
            request->chatId, request->message, account);
    PurpleAccount *purpleAccount = account.purpleAccount;
    PurpleTdClient *client =
        getTdClient(purpleAccount);

    purple_debug_misc(config::pluginId, "Tracking download progress of file id %d: downloaded %d/%d\n",
        (int)fileId, (int)downloadedSize, (int)fileSize);

    char *tempFileName = NULL;
    int fd = g_file_open_tmp("tdlib_download_XXXXXX", &tempFileName, NULL);
    if (fd < 0)
        return true;

    const std::string tempPath = tempFileName;
    PurpleXfer *xfer = purple_xfer_new (account.purpleAccount, PURPLE_XFER_RECEIVE, who.c_str());
    purple_xfer_set_init_fnc(xfer, nop);
    purple_xfer_set_start_fnc(
        xfer, cleanupCanceledInlineDownloadStart);
    purple_xfer_set_cancel_recv_fnc(xfer, nop);
    purple_xfer_set_filename(xfer, fileDescription.c_str());
    // Pin the transfer before the first Purple callback. It is not yet
    // present in the account registry.
    purple_xfer_ref(xfer);
    purple_xfer_request_accepted(xfer, tempFileName);
    if (purple_xfer_is_canceled(xfer)) {
        const bool canContinue =
            isSameClient(purpleAccount, client);
        discardUnregisteredInlineDownloadProgress(
            xfer, fd, tempPath);
        g_free(tempFileName);
        return canContinue;
    }
    if (!isSameClient(purpleAccount, client)) {
        discardUnregisteredInlineDownloadProgress(
            xfer, fd, tempPath);
        g_free(tempFileName);
        return false;
    }
    request =
        account.findPendingRequest<DownloadRequest>(
            requestId);
    if (!request ||
        !isCurrentInlineDownloadRequest(*request)) {
        discardUnregisteredInlineDownloadProgress(
            xfer, fd, tempPath);
        g_free(tempFileName);
        return false;
    }

    purple_xfer_set_size(xfer, fileSize);
    purple_xfer_set_bytes_sent(xfer, downloadedSize);
    if (purple_xfer_is_canceled(xfer)) {
        const bool canContinue =
            isSameClient(purpleAccount, client);
        discardUnregisteredInlineDownloadProgress(
            xfer, fd, tempPath);
        g_free(tempFileName);
        return canContinue;
    }
    if (!isSameClient(purpleAccount, client)) {
        discardUnregisteredInlineDownloadProgress(
            xfer, fd, tempPath);
        g_free(tempFileName);
        return false;
    }
    request =
        account.findPendingRequest<DownloadRequest>(
            requestId);
    if (!request ||
        !isCurrentInlineDownloadRequest(*request)) {
        discardUnregisteredInlineDownloadProgress(
            xfer, fd, tempPath);
        g_free(tempFileName);
        return false;
    }

    if (downloadedSize) {
        close(fd);
        fd = -1;
        // purple_xfer_start will create file passed to purple_xfer_request_accepted and fail the
        // transfer if could not be created. Thus we do our best to give it path to a temporary file
        // that it will be able to create. If it somehow does fail then cancel handler isn't set yet
        // so the actual download won't be cancelled.
        purple_xfer_start(xfer, -1, NULL, 0);
        if (!isSameClient(purpleAccount, client)) {
            discardUnregisteredInlineDownloadProgress(
                xfer, fd, tempPath);
            g_free(tempFileName);
            return false;
        }
        request =
            account.findPendingRequest<DownloadRequest>(
                requestId);
        if (!request ||
            !isCurrentInlineDownloadRequest(*request)) {
            discardUnregisteredInlineDownloadProgress(
                xfer, fd, tempPath);
            g_free(tempFileName);
            return false;
        }
    }

    if (!purple_xfer_is_canceled(xfer)) {
        // Commit request and account ownership only after every reentrant
        // setup callback has completed and the request was reacquired.
        request->tempFileName = tempPath;
        request->tempFd = fd;
        fd = -1;
        xfer->data = new DownloadData(account, transceiver);
        purple_xfer_set_cancel_recv_fnc(xfer, cancelDownload);
        account.addFileTransfer(
            fileId, xfer, ChatTarget(),
            ReceiveTransferKind::InlineProgress,
            requestId);
        purple_xfer_unref(xfer);
    } else {
        // A synchronous callback cancelled Purple's original reference. Only
        // the guard reference and local artifacts remain.
        discardUnregisteredInlineDownloadProgress(
            xfer, fd, tempPath);
    }

    g_free(tempFileName);
    return true;
}

static void handleLongInlineDownload(uint64_t requestId, TdTransceiver &transceiver,
                                     TdAccountData &account)
{
    DownloadRequest *request =
        account.findPendingRequest<DownloadRequest>(
            requestId);
    if (!request ||
        !isCurrentInlineDownloadRequest(*request)) {
        return;
    }

    PendingContentHandle pendingContent =
        request->pendingContent;
    const char *option = purple_account_get_string(
        account.purpleAccount,
        AccountOptions::DownloadBehaviour,
        AccountOptions::DownloadBehaviourDefault());
    if (!strcmp(
            option,
            AccountOptions::DownloadBehaviourHyperlink)) {
        // We didn't want inline downloads, but got one anyway because it's
        // image or sticker. At least keep the fake transfer progress used by
        // Pidgin.
        if (!startInlineDownloadProgress(
                requestId, transceiver, account)) {
            return;
        }
    }

    // Progress callbacks can synchronously complete and extract this request,
    // or invalidate its released content incarnation.
    request =
        account.findPendingRequest<DownloadRequest>(
            requestId);
    if (!request ||
        !isCurrentInlineDownloadRequest(*request)) {
        return;
    }
    pendingContent = request->pendingContent;

    IncomingMessage *pendingMessage = nullptr;
    if (pendingContent.valid() &&
        pendingContent.message.disposition() ==
            PendingMessageState::Disposition::Queued) {
        pendingMessage =
            account.pendingMessages.findPendingMessage(
                pendingContent);
        if (!pendingMessage)
            return;
    }

    if (!pendingMessage)
        return;

    pendingMessage->inlineDownloadTimeout = true;
    std::vector<IncomingMessage> readyMessages;
    PurpleAccount *purpleAccount =
        account.purpleAccount;
    PurpleTdClient *client =
        getTdClient(purpleAccount);
    checkMessageReady(
        pendingContent.message,
        transceiver, account, &readyMessages);
    pendingMessage = nullptr;
    if (!isSameClient(purpleAccount, client))
        return;

    // Display callbacks can complete and extract the same request while the
    // client remains alive. Reacquire and revalidate it before moving any
    // delayed reply or thumbnail state.
    request =
        account.findPendingRequest<DownloadRequest>(
            requestId);
    if (!request ||
        !isCurrentInlineDownloadRequest(*request)) {
        return;
    }

    // Now after "Downloading..." notification has been displayed (which may
    // have included a caption), move reply and thumbnail state onto the
    // still-current request for its eventual hyperlink.
    for (IncomingMessage &readyMessage : readyMessages) {
        if (readyMessage.pendingMessage.state ==
            request->pendingContent.message.state) {
            request->message.repliedMessage =
                std::move(readyMessage.repliedMessage);
            request->thumbnail =
                std::move(readyMessage.thumbnail);
        }
    }
}

void downloadFileInline(int32_t fileId, ChatId chatId, TgMessageInfo &message,
                        const std::string &fileDescription,
                        td::td_api::object_ptr<td::td_api::file> thumbnail,
                        TdTransceiver &transceiver, TdAccountData &account,
                        PendingContentHandle pendingContent)
{
    td::td_api::object_ptr<td::td_api::downloadFile> downloadReq =
        td::td_api::make_object<td::td_api::downloadFile>();
    downloadReq->file_id_     = fileId;
    downloadReq->priority_    = FILE_DOWNLOAD_PRIORITY;
    downloadReq->offset_      = 0;
    downloadReq->limit_       = 0;
    downloadReq->synchronous_ = true;

    uint64_t requestId = transceiver.sendQuery(
        std::move(downloadReq),
        [&transceiver, &account](uint64_t reqId, td::td_api::object_ptr<td::td_api::Object> object) {
            inlineDownloadResponse(reqId, std::move(object), transceiver, account);
        });
    std::unique_ptr<DownloadRequest> request = std::make_unique<DownloadRequest>(
                                               requestId,
                                               ReceiveTransferKind::InlineProgress,
                                               chatId,
                                               message, fileId, 0, fileDescription, thumbnail.release(),
                                               std::move(pendingContent));

    account.addPendingRequest<DownloadRequest>(requestId, std::move(request));
    transceiver.setQueryTimer(requestId,
                              [&transceiver, &account](uint64_t reqId, td::td_api::object_ptr<td::td_api::Object>) {
                                  handleLongInlineDownload(reqId, transceiver, account);
                              }, 1, false);
}

struct DownloadProgressWaiter {
    uint64_t requestId;
    ReceiveTransferKind transferKind;
    PurpleXfer *xfer;
};

static DownloadRequest *findCurrentDownloadWaiter(
    const DownloadProgressWaiter &waiter,
    TdAccountData &account)
{
    DownloadRequest *downloadReq =
        account.findPendingRequest<DownloadRequest>(
            waiter.requestId);
    if (!downloadReq ||
        downloadReq->transferKind !=
            waiter.transferKind ||
        !isCurrentInlineDownloadRequest(*downloadReq)) {
        return nullptr;
    }

    if (waiter.xfer) {
        TdAccountData::FileTransferInfo transfer;
        if (!account.getFileTransferForRequest(
                waiter.requestId,
                waiter.transferKind,
                transfer) ||
            transfer.xfer != waiter.xfer) {
            return nullptr;
        }
    }
    return downloadReq;
}

static bool updateDownloadProgress(
    const td::td_api::file &file,
    const DownloadProgressWaiter &waiter,
    PurpleAccount *purpleAccount,
    PurpleTdClient *client,
    TdAccountData &account)
{
    DownloadRequest *downloadReq =
        findCurrentDownloadWaiter(waiter, account);
    if (!downloadReq)
        return true;

    const int32_t previousDownloadedSize =
        downloadReq->downloadedSize;
    unsigned fileSize       = getFileSize(file);
    int32_t  downloadedSize = std::max((td::td_api::int53)0, file.local_ ? file.local_->downloaded_size_ : (td::td_api::int53)0);

    if (waiter.xfer) {
        if (purple_xfer_is_canceled(waiter.xfer))
            return true;

        purple_xfer_set_size(waiter.xfer, fileSize);
        if (!isSameClient(purpleAccount, client)) {
            return false;
        }
        downloadReq =
            findCurrentDownloadWaiter(waiter, account);
        if (!downloadReq)
            return true;

        if ((downloadedSize != 0) &&
            (previousDownloadedSize == 0)) {
            // For "inline" file downloads with fake-file-name PurpleXfer tracking progress,
            // both if below should evaluate to true - close the fake file and start transfer
            // (which reopens the fake file).
            // For downloads using PurpleXfer in standard way, both if should evaluate to false:
            // purple_xfer_start is called when downloadFile request is sent.
            if (downloadReq->tempFd >= 0)
                close(downloadReq->tempFd);
            downloadReq->tempFd = -1;
            downloadReq->fileSize = fileSize;
            downloadReq->downloadedSize =
                downloadedSize;
            if (purple_xfer_get_status(waiter.xfer) !=
                PURPLE_XFER_STATUS_STARTED) {
                purple_xfer_start(
                    waiter.xfer, -1, NULL, 0);
                if (!isSameClient(
                        purpleAccount, client)) {
                    return false;
                }
                downloadReq =
                    findCurrentDownloadWaiter(
                        waiter, account);
                if (!downloadReq)
                    return true;
            }
        } else {
            downloadReq->fileSize = fileSize;
            downloadReq->downloadedSize =
                downloadedSize;
        }

        if (purple_xfer_is_canceled(waiter.xfer))
            return true;

        purple_xfer_set_bytes_sent(
            waiter.xfer, downloadedSize);
        if (!isSameClient(purpleAccount, client)) {
            return false;
        }
        downloadReq =
            findCurrentDownloadWaiter(waiter, account);
        if (!downloadReq ||
            purple_xfer_is_canceled(waiter.xfer)) {
            return true;
        }
        purple_xfer_update_progress(waiter.xfer);
        return isSameClient(purpleAccount, client);
    }

    downloadReq->fileSize = fileSize;
    downloadReq->downloadedSize = downloadedSize;
    return true;
}

void updateFileTransferProgress(const td::td_api::file &file, TdTransceiver &transceiver,
                                TdAccountData &account, TdTransceiver::ResponseCb sendMessageResponse)
{
    std::vector<TdAccountData::FileTransferInfo> uploads =
        account.getFileTransfers(
            file.id_, PURPLE_XFER_SEND);
    if (!uploads.empty()) {
        if (!file.remote_) {
            cancelDocumentUploads(
                account.extractFileTransfers(
                    file.id_, PURPLE_XFER_SEND));
        } else if (file.remote_->is_uploading_active_) {
            // Pin every waiter before the first progress callback. A
            // synchronous disconnect removes the registry references for all
            // waiters, after which their cleared data markers suppress the
            // remaining callbacks.
            for (const auto &upload: uploads)
                purple_xfer_ref(upload.xfer);
            for (const auto &upload: uploads) {
                if (upload.xfer->data)
                    reportDocumentUploadProgress(
                        file, upload.xfer);
            }
            for (const auto &upload: uploads)
                purple_xfer_unref(upload.xfer);
        } else if (file.local_ &&
                   file.remote_->uploaded_size_ ==
                       file.local_->downloaded_size_) {
            purple_debug_misc(
                config::pluginId,
                "Finishing %zu document upload(s) for file id %d\n",
                uploads.size(), static_cast<int>(file.id_));
            finishDocumentUploads(
                file,
                account.extractFileTransfers(
                    file.id_, PURPLE_XFER_SEND),
                transceiver, account,
                sendMessageResponse);
        }
        return;
    }

    std::vector<DownloadProgressWaiter> waiters;
    const std::vector<uint64_t> requestIds =
        account.findDownloadRequestIds(file.id_);
    waiters.reserve(requestIds.size());
    for (uint64_t requestId: requestIds) {
        DownloadRequest *request =
            account.findPendingRequest<DownloadRequest>(
                requestId);
        if (!request ||
            !isCurrentInlineDownloadRequest(*request)) {
            continue;
        }

        PurpleXfer *xfer = nullptr;
        TdAccountData::FileTransferInfo transfer;
        if (account.getFileTransferForRequest(
                requestId, request->transferKind,
                transfer)) {
            xfer = transfer.xfer;
            // Pin every exact waiter before the first Purple callback. A
            // callback for one request may cancel or disconnect all others.
            purple_xfer_ref(xfer);
        }
        waiters.push_back(
            DownloadProgressWaiter{
                requestId, request->transferKind, xfer});
    }

    PurpleAccount *purpleAccount =
        account.purpleAccount;
    PurpleTdClient *client =
        getTdClient(purpleAccount);
    for (const DownloadProgressWaiter &waiter: waiters) {
        if (!isSameClient(purpleAccount, client) ||
            !updateDownloadProgress(
                file, waiter, purpleAccount, client,
                account)) {
            break;
        }
    }

    // No account or registry access is allowed below this point. Transfers
    // may have been removed and their core references consumed by callbacks.
    for (const DownloadProgressWaiter &waiter: waiters) {
        if (waiter.xfer)
            purple_xfer_unref(waiter.xfer);
    }
}

std::string getDownloadPath(const td::td_api::object_ptr<td::td_api::Object> &downloadResponse)
{
    if (downloadResponse && (downloadResponse->get_id() == td::td_api::file::ID)) {
        const td::td_api::file &file = static_cast<const td::td_api::file &>(*downloadResponse);
        if (!file.local_)
            purple_debug_warning(config::pluginId, "No local file info after downloading\n");
        else if (!file.local_->is_downloading_completed_)
            purple_debug_warning(config::pluginId, "File not completely downloaded\n");
        else
            return file.local_->path_;
    } else {
        std::string message = getDisplayedError(downloadResponse);
        purple_debug_warning(config::pluginId, "Error downloading file: %s\n", message.c_str());
    }

    return "";
}

struct DownloadWrapup {
    PurpleXfer *download;
    FILE       *tdlibFile;
    std::string tdlibPath;
};

static bool canContinueStandardDownload(
    PurpleXfer *download)
{
    return !purple_xfer_is_canceled(download) &&
           purple_xfer_get_end_time(download) == 0;
}

static gboolean wrapupDownload(void *data)
{
    DownloadWrapup *wrapupData = static_cast<DownloadWrapup *>(data);
    unsigned chunkSize = AccountThread::isSingleThread() ? 10 : 1048576;
    PurpleXfer *download = wrapupData->download;

    bool last = false;
    if (canContinueStandardDownload(download)) {
        if (purple_xfer_get_bytes_sent(download) + chunkSize >=
            purple_xfer_get_size(download)) {
            last = true;
            chunkSize =
                purple_xfer_get_size(download) -
                purple_xfer_get_bytes_sent(download);
        }

        uint8_t *buf = new uint8_t[chunkSize];
        unsigned bytesRead = fread(
            buf, 1, chunkSize, wrapupData->tdlibFile);
        if (bytesRead < chunkSize) {
            // Unlikely error message not worth translating
            std::string message = formatMessage("Failed to download {}: error reading {} after {} bytes",
                                                {purple_xfer_get_local_filename(download),
                                                wrapupData->tdlibPath,
                                                std::to_string(purple_xfer_get_bytes_sent(download) + bytesRead)});
            purple_debug_warning(config::pluginId, "%s\n", message.c_str());
            purple_xfer_error(
                PURPLE_XFER_RECEIVE,
                purple_xfer_get_account(download),
                download->who, message.c_str());
            if (canContinueStandardDownload(download))
                purple_xfer_cancel_remote(download);
            last = true;
        } else {
            const gboolean written =
                purple_xfer_write_file(
                    download, buf, bytesRead);
            if (!written ||
                !canContinueStandardDownload(download)) {
                if (canContinueStandardDownload(download))
                    purple_xfer_cancel_remote(download);
                last = true;
            } else if (last) {
                purple_xfer_set_completed(download, TRUE);
                if (canContinueStandardDownload(download)) {
                    // Keep a second terminal guard. An XferEnd callback may
                    // cancel and consume Purple's original reference before
                    // purple_xfer_end returns.
                    purple_xfer_ref(download);
                    purple_xfer_end(download);
                    if (!purple_xfer_is_canceled(download))
                        purple_xfer_unref(download);
                }
            }
        }
        delete[] buf;
    } else
        last = true;

    if (last) {
        purple_xfer_unref(download);
        fclose(wrapupData->tdlibFile);
        delete wrapupData;
        return G_SOURCE_REMOVE;
    } else
        return G_SOURCE_CONTINUE;
}

static void standardDownloadResponse(TdAccountData *account, uint64_t requestId,
                                     td::td_api::object_ptr<td::td_api::Object> object)
{
    std::unique_ptr<DownloadRequest> request = account->getPendingRequest<DownloadRequest>(requestId);
    std::string                      path    = getDownloadPath(object);
    if (!request ||
        request->transferKind !=
            ReceiveTransferKind::Standard) {
        return;
    }

    TdAccountData::FileTransferInfo transfer;
    if (account->extractFileTransferForRequest(
            requestId, ReceiveTransferKind::Standard,
            transfer)) {
        PurpleXfer *download = transfer.xfer;
        std::unique_ptr<DownloadData> data(static_cast<DownloadData *>(download->data));
        download->data = NULL;

        FILE *f = NULL;
        if (!path.empty())
            f = fopen(path.c_str(), "r");

        if (f) {
            purple_xfer_set_bytes_sent(download, 0);
            long fileSize;
            if (fseek(f, 0, SEEK_END) == 0) {
                fileSize = ftell(f);
                if (fileSize >= 0)
                    purple_xfer_set_size(download, fileSize);
                fseek(f, 0, SEEK_SET);
            }

            DownloadWrapup *idleData = new DownloadWrapup;
            idleData->download = download;
            idleData->tdlibFile = f;
            idleData->tdlibPath = path;
            purple_xfer_ref(download);
            if (AccountThread::isSingleThread()) {
                while (wrapupDownload(idleData) == G_SOURCE_CONTINUE) ;
            } else
                g_idle_add(wrapupDownload, idleData);
        } else {
            if (!path.empty()) {
                // Unlikely error message not worth translating
                std::string message = formatMessage("Failed to open {}: {}", {path, std::string(strerror(errno))});
                purple_debug_misc(config::pluginId, "%s\n", message.c_str());
                purple_xfer_error(PURPLE_XFER_RECEIVE, account->purpleAccount, download->who, message.c_str());
            }
            if (path.empty())
                purple_debug_warning(config::pluginId, "Incomplete file in download response for %s\n",
                                     purple_xfer_get_local_filename(download));
            purple_xfer_cancel_remote(download);
        }
    }
}

static void startStandardDownload(PurpleXfer *xfer)
{
    DownloadData *data = static_cast<DownloadData *>(xfer->data);
    if (!data) return;

    TdAccountData::FileTransferInfo transfer;
    if (data->account->getFileTransferInfo(
            xfer, transfer)) {
        td::td_api::object_ptr<td::td_api::downloadFile> downloadReq =
            td::td_api::make_object<td::td_api::downloadFile>();
        downloadReq->file_id_     = transfer.fileId;
        downloadReq->priority_    = FILE_DOWNLOAD_PRIORITY;
        downloadReq->offset_      = 0;
        downloadReq->limit_       = 0;
        downloadReq->synchronous_ = true;

        uint64_t requestId = data->transceiver->sendQuery(std::move(downloadReq),
                                                          [account=data->account](uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object) {
                                                              standardDownloadResponse(account, requestId, std::move(object));
                                                          });
        TgMessageInfo messageInfo;
        std::unique_ptr<DownloadRequest> request = std::make_unique<DownloadRequest>(
                                                        requestId,
                                                        ReceiveTransferKind::Standard,
                                                        ChatId::invalid,
                                                        messageInfo, transfer.fileId, 0, "", nullptr,
                                                        // Standard transfers are intentionally
                                                        // not bound to a displayed message.
                                                        PendingContentHandle());
        data->account->addPendingRequest<DownloadRequest>(requestId, std::move(request));
        if (!data->account->associateFileTransferRequest(
                xfer, ReceiveTransferKind::Standard,
                requestId)) {
            return;
        }
        // Start immediately, because standardDownloadResponse will call purple_xfer_write_file, which
        // will fail if purple_xfer_start hasn't been called
        purple_xfer_start(xfer, -1, NULL, 0);
    }
}

void requestStandardDownload(ChatId chatId, const TgMessageInfo &message, const std::string &fileName,
                             const td::td_api::file &file, TdTransceiver &transceiver, TdAccountData &account)
{
    std::string who = getDownloadXferPeerName(chatId, message, account);
    PurpleXfer *xfer = purple_xfer_new (account.purpleAccount, PURPLE_XFER_RECEIVE, who.c_str());
    purple_xfer_set_init_fnc(xfer, startStandardDownload);
    purple_xfer_set_cancel_recv_fnc(xfer, cancelDownload);
    purple_xfer_set_filename(xfer, fileName.c_str());
    purple_xfer_set_size(xfer, getFileSize(file));
    xfer->data = new DownloadData(account, transceiver);
    account.addFileTransfer(
        file.id_, xfer, ChatTarget(),
        ReceiveTransferKind::Standard, 0);
    purple_xfer_request(xfer);
}

unsigned getFileSize(const td::td_api::file &file)
{
    int32_t size = file.size_;
    if (size == 0)
        size = file.expected_size_;

    if (size <= 0)
        return 0;
    else
        return size;
}

unsigned getFileSizeKb(const td::td_api::file &file)
{
    return getFileSize(file)/1024;
}

std::string makeDocumentDescription(const td::td_api::voiceNote *document)
{
    if (!document)
        // Unlikely error message not worth translating
        return "faulty voice note";
    // TRANSLATOR: In-line document type. Argument will be a mime type.
    return formatMessage(_("voice note [{}]"), document->mime_type_);
}

std::string makeDocumentDescription(const td::td_api::videoNote *document)
{
    if (!document)
        // Unlikely error message not worth translating
        return "faulty voice note";
    // TRANSLATOR: In-line document type. Argument will be a duration.
    return formatMessage(_("video note [{}]"), formatDuration(document->duration_));
}

std::string getFileName(const td::td_api::voiceNote* document)
{
    td::Client::Response resp = td::Client::execute({0, td::td_api::make_object<td::td_api::getFileExtension>(document->mime_type_)});
    if (resp.object && (resp.object->get_id() == td::td_api::text::ID)) {
        // TRANSLATOR: Filename. Keep it short, and as few special characters as possible.
        return std::string(_("voiceNote")) + '.' + static_cast<const td::td_api::text &>(*resp.object).text_;
    }
    return _("voiceNote");
}

std::string getFileName(const td::td_api::videoNote *document)
{
    // TRANSLATOR: Filename. Keep it short, and as few special characters as possible.
    return std::string(_("videoNote")) + ".avi";
}
