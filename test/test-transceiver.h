#ifndef _TEST_TRANSCEIVER_H
#define _TEST_TRANSCEIVER_H

#include "transceiver.h"
#include <td/telegram/td_api.h>
#include <purple.h>
#include <queue>
#include <vector>
#include <memory>
#include <cstdint>

namespace td {
namespace td_api {

// Backward-compatible helpers for TDLib 1.8.64
inline object_ptr<viewMessages> Mock_ViewMessages(int64_t chat_id, std::vector<int64_t> message_ids, bool force_read) {
    return make_object<td::td_api::viewMessages>(chat_id, std::move(message_ids), nullptr, force_read);
}

inline object_ptr<inputMessageText> Mock_InputMessageText(object_ptr<formattedText> &&text, bool clear_draft = false) {
    return make_object<inputMessageText>(std::move(text), object_ptr<linkPreviewOptions>(), clear_draft);
}

inline object_ptr<inputMessagePhoto> Mock_InputMessagePhoto(object_ptr<InputFile> &&photo,
                                                            object_ptr<inputThumbnail> &&thumbnail,
                                                            std::vector<std::int32_t> &&added_sticker_file_ids,
                                                            std::int32_t width,
                                                            std::int32_t height,
                                                            object_ptr<formattedText> &&caption) {
    return make_object<inputMessagePhoto>(
        std::move(photo), std::move(thumbnail), nullptr, std::move(added_sticker_file_ids),
        width, height, std::move(caption), false, nullptr, false
    );
}

inline object_ptr<inputMessageDocument> Mock_InputMessageDocument(object_ptr<InputFile> &&document,
                                                                  object_ptr<inputThumbnail> &&thumbnail,
                                                                  object_ptr<formattedText> &&caption) {
    return make_object<inputMessageDocument>(std::move(document), std::move(thumbnail), false, std::move(caption));
}

inline object_ptr<sendMessage> Mock_SendMessage(int64_t chat_id, int64_t message_thread_id,
                                          object_ptr<InputMessageReplyTo> &&reply_to,
                                          object_ptr<messageSendOptions> &&options,
                                          object_ptr<InputMessageContent> &&content) {
    return make_object<td::td_api::sendMessage>(chat_id, nullptr, std::move(reply_to), std::move(options), nullptr, std::move(content));
}

inline object_ptr<sendChatAction> Mock_SendChatAction(int64_t chat_id, object_ptr<ChatAction> &&action) {
    return make_object<td::td_api::sendChatAction>(chat_id, nullptr, "", std::move(action));
}

inline object_ptr<changeImportedContacts> Mock_ImportContacts(std::vector<object_ptr<contact>> contacts) {
    std::vector<object_ptr<importedContact>> imported;
    for (auto &c : contacts) {
        imported.push_back(make_object<importedContact>(c->phone_number_, c->first_name_, c->last_name_, nullptr));
    }
    return make_object<td::td_api::changeImportedContacts>(std::move(imported));
}

template <typename T, typename... Args>
std::vector<object_ptr<T>> make_vector(Args&&... args) {
    std::vector<object_ptr<T>> vec;
    (vec.push_back(td::move_tl_object_as<T>(std::move(args))), ...);
    return vec;
}

class TestTransceiver: public ITransceiverBackend {
public:
    void  send(td::Client::Request &&request) override;

    // Check that given request, and no other, has been received, and clear the queue
    uint64_t verifyRequest(const td::td_api::Function &request);
    uint64_t verifyRequest(td::td_api::object_ptr<td::td_api::Function> &&request) { return verifyRequest(*request); }

    // Check that given requests, and no others, have been received, and clear the queue
    std::vector<uint64_t> verifyRequests(std::vector<td::td_api::object_ptr<td::td_api::Function>> &&requests);
    void verifyRequests(const std::vector<const td::td_api::Function *> requests);

    // Variadic template to handle move-only types without initializer_list
    template<typename... Args>
    std::vector<uint64_t> verifyRequestsV(Args&&... args) {
        std::vector<object_ptr<Function>> requests;
        (requests.push_back(td::move_tl_object_as<Function>(std::move(args))), ...);
        return verifyRequests(std::move(requests));
    }

    // Check that no requests have been received
    void verifyNoRequests();
    void forgetVerifiedRequests();

    // Send an update
    template<typename T>
    void update(td::td_api::object_ptr<T> object)
    {
        this->receive(td::Client::Response{0, td::move_tl_object_as<td::td_api::Object>(std::move(object))});
    }

    // Run all pending timeouts
    void runTimeouts();

    // Reply to the last received request
    void reply(td::td_api::object_ptr<td::td_api::Object> object);

    // Reply to a specific request
    void reply(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);

    guint addTimeout(guint interval, GSourceFunc function, gpointer data) override;
    void  cancelTimer(guint id) override;

    const std::string &getInputPhotoPath(unsigned index) const { return m_inputPhotoPaths.at(index); }
    std::string addInputPhoto(const void *data, size_t size);

private:
    struct TimerInfo {
        guint       id;
        GSourceFunc function;
        gpointer    data;
    };

    std::queue<td::Client::Request> m_requests;
    std::queue<uint64_t>            m_verifiedRequestIds;
    uint64_t                        m_lastReceivedRequestId = 0;
    const td::td_api::Function     *expectedRequest = nullptr;
    uint64_t                        expectedRequestId = 1;
    std::vector<std::string>        m_inputPhotoPaths;
    std::vector<TimerInfo>          m_timers;
    guint                           m_nextTimerId = 1;
};

// Functions in td::td_api namespace
std::string requestToString(const td::td_api::Function &request);

void compare_func(const td::td_api::Function &actual, const td::td_api::Function &expected);

object_ptr<user> makeUser(std::int32_t id_, std::string const &first_name_,
                          std::string const &last_name_,
                          std::string const &phone_number_,
                          object_ptr<UserStatus> status_);
object_ptr<users> makeUsers(std::vector<int64_t> user_ids);

object_ptr<chat> makeChat(std::int64_t id_,
                          object_ptr<ChatType> &&type_,
                          std::string const &title_,
                          object_ptr<message> &&last_message_ = nullptr,
                          std::int32_t unread_count_ = 0,
                          std::int64_t last_read_inbox_message_id_ = 0,
                          std::int64_t last_read_outbox_message_id_ = 0);
void addChatPosition(object_ptr<chat> &chat, object_ptr<ChatList> &&chatList, std::int64_t order = 1);

object_ptr<updateChatPosition> makeUpdateChatListMain(int64_t chatId);
object_ptr<updateChatPosition> makeUpdateChatList(int64_t chatId, object_ptr<ChatList> &&chatList);
object_ptr<updateChatPosition> makeUpdateRemoveFromChatList(int64_t chatId, object_ptr<ChatList> &&removeFrom);

object_ptr<loadChats> getChatsRequest();
object_ptr<Object> getChatsNoChatsResponse();
object_ptr<preliminaryUploadFile> uploadFile(object_ptr<InputFile> &&file,
                                             object_ptr<FileType> &&fileType,
                                             std::int32_t priority);

object_ptr<message> makeMessage(std::int64_t id_, std::int32_t sender_user_id_, std::int64_t chat_id_,
                                bool is_outgoing_, std::int32_t date_, object_ptr<MessageContent> &&content_);
object_ptr<messageReplyToMessage> makeMessageReplyTo(std::int64_t chat_id, std::int64_t message_id);
object_ptr<messageText> makeTextMessage(const std::string &text);
object_ptr<photoSize> makePhotoSize(std::string const &type,
                                    object_ptr<file> &&photo,
                                    std::int32_t width,
                                    std::int32_t height,
                                    std::vector<std::int32_t> progressiveSizes = {});
object_ptr<messagePhoto> makeMessagePhoto(object_ptr<photo> &&photo,
                                          object_ptr<formattedText> &&caption,
                                          bool is_secret = false);
object_ptr<sticker> makeSticker(std::int64_t id,
                                std::int32_t width,
                                std::int32_t height,
                                std::string const &emoji,
                                bool isAnimated,
                                bool isMask,
                                const void *,
                                object_ptr<thumbnail> &&thumbnail,
                                object_ptr<file> &&stickerFile);
object_ptr<messageSticker> makeMessageSticker(object_ptr<sticker> &&sticker);
object_ptr<secretChat> makeSecretChat(std::int32_t id,
                                      std::int64_t userId,
                                      object_ptr<SecretChatState> &&state,
                                      bool isOutbound,
                                      std::int32_t,
                                      std::string const &keyHash,
                                      std::int32_t layer);

object_ptr<photo> makePhotoRemote(int32_t fileId, unsigned size, unsigned width, unsigned height);
object_ptr<photo> makePhotoLocal(int32_t fileId, unsigned size, const std::string &path,
                                 unsigned width, unsigned height);
object_ptr<photo> makePhotoUploading(int32_t fileId, unsigned size, unsigned uploaded, const std::string &path,
                                     unsigned width, unsigned height);

object_ptr<chatMember> makeChatMember(int32_t userId, int32_t inviteUserId, time_t joinTime,
                                      object_ptr<ChatMemberStatus> &&memberStatus, const void *);
object_ptr<basicGroupFullInfo> makeBasicGroupFullInfo(const std::string &description,
                                                      int64_t creatorUserId,
                                                      std::vector<object_ptr<chatMember>> &&members,
                                                      const std::string &inviteLink);
object_ptr<supergroup> makeSupergroup(int64_t id,
                                      object_ptr<ChatMemberStatus> &&status,
                                      int32_t memberCount,
                                      bool isChannel = false);
object_ptr<createChatInviteLink> makeInviteLinkRequest(int64_t chatId);
object_ptr<chatInviteLink> makeChatInviteLink(const std::string &link);

}
}

#endif
