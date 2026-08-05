#include "test-transceiver.h"
#include "format.h"
#include "printout.h"
#include <td/telegram/td_api.h>
#include <gtest/gtest.h>
#include <gtest/gtest-spi.h>
#include <iostream>
using namespace td::td_api;

#define COMPARE(param) ASSERT_EQ(expected.param, actual.param)
#define ASSERT_REDACTED_EQ(expected_value, actual_value, field_name) \
    do { \
        if ((expected_value) != (actual_value)) { \
            FAIL() << field_name << " differs (values redacted)"; \
        } \
    } while (false)
#define COMPARE_REDACTED(param) \
    ASSERT_REDACTED_EQ(expected.param, actual.param, #param)

namespace td {
namespace td_api {

void TestTransceiver::close(TdPollingBackend::CloseCallback callback)
{
    if (callback)
        m_closeCallbacks.push_back(std::move(callback));
}

void TestTransceiver::completeClose(TdPollingBackend::CloseResult result)
{
    std::vector<TdPollingBackend::CloseCallback> callbacks;
    callbacks.swap(m_closeCallbacks);
    for (TdPollingBackend::CloseCallback &callback : callbacks) {
        if (callback)
            callback(result);
    }
}

static void compare(const TextEntityType &actual, const TextEntityType &expected)
{
    ASSERT_EQ(expected.get_id(), actual.get_id());
    switch (expected.get_id()) {
        case textEntityTypePreCode::ID:
            ASSERT_REDACTED_EQ(
                static_cast<const textEntityTypePreCode &>(expected).language_,
                static_cast<const textEntityTypePreCode &>(actual).language_,
                "text entity language");
            break;
        case textEntityTypeTextUrl::ID:
            ASSERT_REDACTED_EQ(
                static_cast<const textEntityTypeTextUrl &>(expected).url_,
                static_cast<const textEntityTypeTextUrl &>(actual).url_,
                "text entity URL");
            break;
        case textEntityTypeMentionName::ID:
            ASSERT_EQ(static_cast<const textEntityTypeMentionName &>(expected).user_id_,
                      static_cast<const textEntityTypeMentionName &>(actual).user_id_);
            break;
        case textEntityTypeCustomEmoji::ID:
            ASSERT_EQ(static_cast<const textEntityTypeCustomEmoji &>(expected).custom_emoji_id_,
                      static_cast<const textEntityTypeCustomEmoji &>(actual).custom_emoji_id_);
            break;
        case textEntityTypeMediaTimestamp::ID:
            ASSERT_EQ(static_cast<const textEntityTypeMediaTimestamp &>(expected).media_timestamp_,
                      static_cast<const textEntityTypeMediaTimestamp &>(actual).media_timestamp_);
            break;
    }
}

static void compare(const textEntity &actual, const textEntity &expected)
{
    ASSERT_EQ(expected.offset_, actual.offset_);
    ASSERT_EQ(expected.length_, actual.length_);
    ASSERT_EQ(expected.type_ != nullptr, actual.type_ != nullptr);
    if (expected.type_)
        compare(*actual.type_, *expected.type_);
}

void compare(const formattedText &actual, const formattedText &expected)
{
    ASSERT_REDACTED_EQ(expected.text_, actual.text_, "formatted text");
    ASSERT_EQ(expected.entities_.size(), actual.entities_.size());
    for (size_t i = 0; i < expected.entities_.size(); i++)
        compare(*actual.entities_[i], *expected.entities_[i]);
}

void compare(const setTdlibParameters &actual, const setTdlibParameters &expected)
{
    COMPARE_REDACTED(database_directory_);
    COMPARE_REDACTED(api_id_);
    COMPARE_REDACTED(api_hash_);
    COMPARE(use_secret_chats_);
}

void compare(const setAuthenticationPhoneNumber &actual, const setAuthenticationPhoneNumber &expected)
{
    COMPARE_REDACTED(phone_number_);
    COMPARE(settings_ != nullptr);
}

void compare(const checkAuthenticationCode &actual, const checkAuthenticationCode &expected)
{
    COMPARE_REDACTED(code_);
}

void compare(const checkAuthenticationPassword &actual, const checkAuthenticationPassword &expected)
{
    COMPARE_REDACTED(password_);
}

void compare(const setAuthenticationEmailAddress &actual,
             const setAuthenticationEmailAddress &expected)
{
    COMPARE_REDACTED(email_address_);
}

void compare(const checkAuthenticationEmailCode &actual,
             const checkAuthenticationEmailCode &expected)
{
    COMPARE(code_ != nullptr);
    if (actual.code_ != nullptr) {
        COMPARE(code_->get_id());
        if (actual.code_->get_id() ==
            emailAddressAuthenticationCode::ID)
        {
            const auto &actual_code =
                static_cast<const emailAddressAuthenticationCode &>(
                    *actual.code_);
            const auto &expected_code =
                static_cast<const emailAddressAuthenticationCode &>(
                    *expected.code_);
            ASSERT_REDACTED_EQ(
                expected_code.code_,
                actual_code.code_,
                "email authentication code");
        }
    }
}

void compare(const proxyTypeSocks5 &actual, const proxyTypeSocks5 &expected)
{
    COMPARE_REDACTED(username_);
    COMPARE_REDACTED(password_);
}

void compare(const proxyTypeHttp &actual, const proxyTypeHttp &expected)
{
    COMPARE_REDACTED(username_);
    COMPARE_REDACTED(password_);
    COMPARE(http_only_);
}

void compare(const proxyTypeMtproto &actual, const proxyTypeMtproto &expected)
{
    COMPARE_REDACTED(secret_);
}

void compare(const proxy &actual, const proxy &expected)
{
    COMPARE_REDACTED(server_);
    COMPARE(port_);
    COMPARE(type_ != nullptr);
    if (actual.type_ != nullptr) {
        COMPARE(type_->get_id());
        if (actual.type_->get_id() == proxyTypeSocks5::ID) {
            compare(
                static_cast<const proxyTypeSocks5 &>(*actual.type_),
                static_cast<const proxyTypeSocks5 &>(*expected.type_));
        } else if (actual.type_->get_id() == proxyTypeHttp::ID) {
            compare(
                static_cast<const proxyTypeHttp &>(*actual.type_),
                static_cast<const proxyTypeHttp &>(*expected.type_));
        } else if (actual.type_->get_id() == proxyTypeMtproto::ID) {
            compare(
                static_cast<const proxyTypeMtproto &>(*actual.type_),
                static_cast<const proxyTypeMtproto &>(*expected.type_));
        }
    }
}

void compare(const addProxy &actual, const addProxy &expected)
{
    COMPARE(proxy_ != nullptr);
    if (actual.proxy_ != nullptr) {
        compare(*actual.proxy_, *expected.proxy_);
    }
    COMPARE(enable_);
}

void compare(const removeProxy &actual, const removeProxy &expected)
{
    COMPARE(proxy_id_);
}

void compare(const inputMessageText &actual, const inputMessageText &expected)
{
    compare(*actual.text_, *expected.text_);
}

void compare(const inputMessagePhoto &actual, const inputMessagePhoto &expected)
{
    COMPARE(photo_ != nullptr);
    COMPARE(caption_ != nullptr);
}

static void compare(const InputFile &actual, const InputFile &expected)
{
    ASSERT_EQ(expected.get_id(), actual.get_id());
    switch (expected.get_id()) {
    case inputFileId::ID:
        ASSERT_EQ(
            static_cast<const inputFileId &>(expected).id_,
            static_cast<const inputFileId &>(actual).id_);
        break;
    case inputFileRemote::ID:
        ASSERT_REDACTED_EQ(
            static_cast<const inputFileRemote &>(expected).id_,
            static_cast<const inputFileRemote &>(actual).id_,
            "remote file identifier");
        break;
    case inputFileLocal::ID:
        ASSERT_REDACTED_EQ(
            static_cast<const inputFileLocal &>(expected).path_,
            static_cast<const inputFileLocal &>(actual).path_,
            "local file path");
        break;
    case inputFileGenerated::ID: {
        const auto &actualGenerated =
            static_cast<const inputFileGenerated &>(actual);
        const auto &expectedGenerated =
            static_cast<const inputFileGenerated &>(expected);
        ASSERT_REDACTED_EQ(
            expectedGenerated.original_path_,
            actualGenerated.original_path_,
            "generated file source path");
        ASSERT_REDACTED_EQ(
            expectedGenerated.conversion_,
            actualGenerated.conversion_,
            "generated file conversion");
        ASSERT_EQ(
            expectedGenerated.expected_size_,
            actualGenerated.expected_size_);
        break;
    }
    default:
        FAIL() << "Unsupported InputFile type "
               << expected.get_id();
    }
}

void compare(const inputMessageDocument &actual, const inputMessageDocument &expected)
{
    COMPARE(document_ != nullptr);
    if (actual.document_ != nullptr) {
        COMPARE(document_->document_ != nullptr);
        if (actual.document_->document_ != nullptr) {
            compare(
                *actual.document_->document_,
                *expected.document_->document_);
        }
    }
    COMPARE(caption_ != nullptr);
}

static void compare(const MessageTopic &actual, const MessageTopic &expected)
{
    ASSERT_EQ(expected.get_id(), actual.get_id());
    switch (expected.get_id()) {
    case messageTopicThread::ID:
        ASSERT_EQ(
            static_cast<const messageTopicThread &>(expected).message_thread_id_,
            static_cast<const messageTopicThread &>(actual).message_thread_id_
        );
        break;
    case messageTopicForum::ID:
        ASSERT_EQ(
            static_cast<const messageTopicForum &>(expected).forum_topic_id_,
            static_cast<const messageTopicForum &>(actual).forum_topic_id_
        );
        break;
    case messageTopicDirectMessages::ID:
        ASSERT_EQ(
            static_cast<const messageTopicDirectMessages &>(expected).direct_messages_chat_topic_id_,
            static_cast<const messageTopicDirectMessages &>(actual).direct_messages_chat_topic_id_
        );
        break;
    case messageTopicSavedMessages::ID:
        ASSERT_EQ(
            static_cast<const messageTopicSavedMessages &>(expected).saved_messages_topic_id_,
            static_cast<const messageTopicSavedMessages &>(actual).saved_messages_topic_id_
        );
        break;
    }
}

void compare(const viewMessages &actual, const viewMessages &expected)
{
    COMPARE(chat_id_);
    COMPARE(message_ids_);
    COMPARE(source_ != nullptr);
    if (actual.source_ != nullptr) {
        ASSERT_EQ(expected.source_->get_id(), actual.source_->get_id());
    }
    COMPARE(force_read_);
}

void compare(const sendMessage &actual, const sendMessage &expected)
{
    COMPARE(chat_id_);
    COMPARE(topic_id_ != nullptr);
    if (actual.topic_id_ != nullptr) {
        compare(*actual.topic_id_, *expected.topic_id_);
    }
    COMPARE(reply_to_ != nullptr);
    COMPARE(input_message_content_ != nullptr);
    if (actual.input_message_content_ != nullptr) {
        COMPARE(input_message_content_->get_id());
        switch (actual.input_message_content_->get_id()) {
        case inputMessageText::ID:
            compare(static_cast<const inputMessageText &>(*actual.input_message_content_),
                    static_cast<const inputMessageText &>(*expected.input_message_content_));
            break;
        case inputMessagePhoto::ID:
            compare(static_cast<const inputMessagePhoto &>(*actual.input_message_content_),
                    static_cast<const inputMessagePhoto &>(*expected.input_message_content_));
            break;
        case inputMessageDocument::ID:
            compare(static_cast<const inputMessageDocument &>(*actual.input_message_content_),
                    static_cast<const inputMessageDocument &>(*expected.input_message_content_));
            break;
        }
    }
}

void compare(const getBasicGroupFullInfo &actual, const getBasicGroupFullInfo &expected)
{
    COMPARE(basic_group_id_);
}

void compare(const joinChatByInviteLink &actual, const joinChatByInviteLink &expected)
{
    COMPARE_REDACTED(invite_link_);
}

void compare(const getMessage &actual, const getMessage &expected)
{
    COMPARE(chat_id_);
    COMPARE(message_id_);
}

void compare(const getForumTopic &actual, const getForumTopic &expected)
{
    COMPARE(chat_id_);
    COMPARE(forum_topic_id_);
}

void compare(const getForumTopics &actual, const getForumTopics &expected)
{
    COMPARE(chat_id_);
    COMPARE_REDACTED(query_);
    COMPARE(offset_date_);
    COMPARE(offset_message_id_);
    COMPARE(offset_forum_topic_id_);
    COMPARE(limit_);
}

void compare(const getForumTopicHistory &actual, const getForumTopicHistory &expected)
{
    COMPARE(chat_id_);
    COMPARE(forum_topic_id_);
    COMPARE(from_message_id_);
    COMPARE(offset_);
    COMPARE(limit_);
}

void compare(const sendChatAction &actual, const sendChatAction &expected)
{
    COMPARE(chat_id_);
    COMPARE(action_ != nullptr);
    if (actual.action_ != nullptr) {
        COMPARE(action_->get_id());
    }
}

void compare(const users &actual, const users &expected)
{
    COMPARE(total_count_);
    COMPARE(user_ids_);
}

void compare(const getChats &actual, const getChats &expected)
{
    COMPARE(chat_list_ != nullptr);
    COMPARE(limit_);
}

void compare(const loadChats &actual, const loadChats &expected)
{
    COMPARE(chat_list_ != nullptr);
    COMPARE(limit_);
}

void compare(const downloadFile &actual, const downloadFile &expected)
{
    COMPARE(file_id_);
    COMPARE(priority_);
}

void compare(const preliminaryUploadFile &actual, const preliminaryUploadFile &expected)
{
    COMPARE(file_ != nullptr);
    if (actual.file_ != nullptr) {
        compare(*actual.file_, *expected.file_);
    }
    COMPARE(file_type_ != nullptr);
    if (actual.file_type_ != nullptr) {
        COMPARE(file_type_->get_id());
    }
    COMPARE(priority_);
}

void compare(
    const cancelPreliminaryUploadFile &actual,
    const cancelPreliminaryUploadFile &expected)
{
    COMPARE(file_id_);
}


void compare(const changeImportedContacts &actual, const changeImportedContacts &expected)
{
    ASSERT_EQ(expected.contacts_.size(), actual.contacts_.size());
}

void compare(const registerUser &actual, const registerUser &expected)
{
    COMPARE_REDACTED(first_name_);
    COMPARE_REDACTED(last_name_);
}

void compare(const addContact &actual, const addContact &expected)
{
    COMPARE(user_id_);
    COMPARE(share_phone_number_);
}

void compare_func(const td::td_api::Function &actual, const td::td_api::Function &expected)
{
    if (expected.get_id() != actual.get_id()) {
        FAIL() << "Request type mismatch: received "
               << ::requestTypeToString(actual) << ", expected "
               << ::requestTypeToString(expected)
               << " (request values redacted)";
    }

#define C(class) case class::ID: \
    compare(static_cast<const class &>(actual), static_cast<const class &>(expected)); \
    break;

    switch (actual.get_id()) {
        C(setTdlibParameters)
        C(setAuthenticationPhoneNumber)
        C(checkAuthenticationCode)
        C(checkAuthenticationPassword)
        C(setAuthenticationEmailAddress)
        C(checkAuthenticationEmailCode)
        C(registerUser)
        C(changeImportedContacts)
        case getContacts::ID: break;
        C(getChats)
        C(loadChats)
        C(viewMessages)
        C(downloadFile)
        C(preliminaryUploadFile)
        C(cancelPreliminaryUploadFile)
        case sendMessage::ID:
            compare(static_cast<const sendMessage &>(actual), static_cast<const sendMessage &>(expected));
            break;
        C(getBasicGroupFullInfo)
        C(joinChatByInviteLink)
        C(getMessage)
        C(getForumTopic)
        C(getForumTopics)
        C(getForumTopicHistory)
        C(sendChatAction)
        C(addProxy)
        C(removeProxy)
        C(addContact)
        case disableProxy::ID: break;
        case getProxies::ID: break;
    }
}

object_ptr<user> makeUser(std::int32_t id_, std::string const &first_name_,
                          std::string const &last_name_,
                          std::string const &phone_number_,
                          object_ptr<UserStatus> status_)
{
    auto result = make_object<user>();
    result->id_ = id_;
    result->first_name_ = first_name_;
    result->last_name_ = last_name_;
    result->phone_number_ = phone_number_;
    result->status_ = std::move(status_);
    result->type_ = make_object<userTypeRegular>();
    result->is_contact_ = false;
    return result;
}

object_ptr<users> makeUsers(std::vector<int64_t> user_ids)
{
    auto result = make_object<users>();
    result->total_count_ = static_cast<int32_t>(user_ids.size());
    result->user_ids_ = std::move(user_ids);
    return result;
}

object_ptr<chat> makeChat(std::int64_t id_,
                          object_ptr<ChatType> &&type_,
                          std::string const &title_,
                          object_ptr<message> &&last_message_,
                          std::int32_t unread_count_,
                          std::int64_t last_read_inbox_message_id_,
                          std::int64_t last_read_outbox_message_id_)
{
    auto result = make_object<chat>();
    result->id_ = id_;
    result->type_ = std::move(type_);
    result->title_ = title_;
    result->last_message_ = std::move(last_message_);
    result->unread_count_ = unread_count_;
    result->last_read_inbox_message_id_ = last_read_inbox_message_id_;
    result->last_read_outbox_message_id_ = last_read_outbox_message_id_;
    result->permissions_ = make_object<chatPermissions>();
    result->permissions_->can_send_basic_messages_ = true;
    result->permissions_->can_send_audios_ = true;
    result->permissions_->can_send_documents_ = true;
    result->permissions_->can_send_photos_ = true;
    result->permissions_->can_send_videos_ = true;
    result->permissions_->can_send_video_notes_ = true;
    result->permissions_->can_send_voice_notes_ = true;
    result->permissions_->can_send_polls_ = true;
    result->permissions_->can_send_other_messages_ = true;
    result->permissions_->can_add_link_previews_ = true;
    result->permissions_->can_change_info_ = true;
    result->permissions_->can_invite_users_ = true;
    result->permissions_->can_pin_messages_ = true;
    result->permissions_->can_create_topics_ = true;
    result->notification_settings_ = make_object<chatNotificationSettings>();
    return result;
}

void addChatPosition(object_ptr<chat> &chat, object_ptr<ChatList> &&chatList, std::int64_t order)
{
    chat->positions_.push_back(make_object<chatPosition>(std::move(chatList), order, false, nullptr));
}

object_ptr<updateChatPosition> makeUpdateChatListMain(int64_t chatId)
{
    return makeUpdateChatList(chatId, make_object<chatListMain>());
}

object_ptr<updateChatPosition> makeUpdateChatList(int64_t chatId, object_ptr<ChatList> &&chatList)
{
    return make_object<updateChatPosition>(
        chatId,
        make_object<chatPosition>(std::move(chatList), 1, false, nullptr)
    );
}

object_ptr<updateChatPosition> makeUpdateRemoveFromChatList(int64_t chatId, object_ptr<ChatList> &&removeFrom)
{
    return make_object<updateChatPosition>(
        chatId,
        make_object<chatPosition>(std::move(removeFrom), 0, false, nullptr)
    );
}

object_ptr<loadChats> getChatsRequest()
{
    return make_object<loadChats>(make_object<chatListMain>(), 200);
}

object_ptr<Object> getChatsNoChatsResponse()
{
    return make_object<error>(404, "Not Found");
}

object_ptr<preliminaryUploadFile> uploadFile(object_ptr<InputFile> &&file,
                                             object_ptr<FileType> &&fileType,
                                             std::int32_t priority)
{
    return make_object<preliminaryUploadFile>(std::move(file), std::move(fileType), priority);
}

object_ptr<message> makeMessage(std::int64_t id_, std::int32_t sender_user_id_, std::int64_t chat_id_,
                                bool is_outgoing_, std::int32_t date_, object_ptr<MessageContent> &&content_,
                                object_ptr<MessageTopic> &&topic_id_)
{
    auto result = make_object<message>();
    result->id_ = id_;
    result->sender_id_ = make_object<messageSenderUser>(sender_user_id_);
    result->chat_id_ = chat_id_;
    result->topic_id_ = std::move(topic_id_);
    result->sending_state_ = is_outgoing_ ? make_object<messageSendingStatePending>(0) : nullptr;
    result->is_outgoing_ = is_outgoing_;
    result->date_ = date_;
    result->content_ = std::move(content_);
    return result;
}

object_ptr<messageReplyToMessage> makeMessageReplyTo(std::int64_t chat_id, std::int64_t message_id)
{
    return make_object<messageReplyToMessage>(chat_id, message_id, nullptr, 0, "", nullptr, 0, nullptr);
}

object_ptr<messageText> makeTextMessage(const std::string &text)
{
    auto result = make_object<messageText>();
    result->text_ = make_object<formattedText>(text, std::vector<object_ptr<textEntity>>());
    result->link_preview_options_ = nullptr;
    return result;
}

object_ptr<photoSize> makePhotoSize(std::string const &type,
                                    object_ptr<file> &&photo,
                                    std::int32_t width,
                                    std::int32_t height,
                                    std::vector<std::int32_t> progressiveSizes)
{
    return make_object<photoSize>(type, std::move(photo), width, height, std::move(progressiveSizes));
}

object_ptr<messagePhoto> makeMessagePhoto(object_ptr<photo> &&photo,
                                          object_ptr<formattedText> &&caption,
                                          bool is_secret)
{
    return make_object<messagePhoto>(std::move(photo), nullptr, std::move(caption), false, false, is_secret);
}

object_ptr<sticker> makeSticker(std::int64_t id,
                                std::int32_t width,
                                std::int32_t height,
                                std::string const &emoji,
                                bool isAnimated,
                                bool isMask,
                                const void *,
                                object_ptr<thumbnail> &&thumbnail,
                                object_ptr<file> &&stickerFile)
{
    object_ptr<StickerFormat> format = isAnimated
        ? td::move_tl_object_as<StickerFormat>(make_object<stickerFormatTgs>())
        : td::move_tl_object_as<StickerFormat>(make_object<stickerFormatWebp>());
    object_ptr<StickerFullType> fullType = isMask
        ? td::move_tl_object_as<StickerFullType>(make_object<stickerFullTypeMask>())
        : td::move_tl_object_as<StickerFullType>(make_object<stickerFullTypeRegular>());
    return make_object<sticker>(
        id, 0, width, height, emoji, std::move(format), std::move(fullType),
        std::move(thumbnail), std::move(stickerFile)
    );
}

object_ptr<messageSticker> makeMessageSticker(object_ptr<sticker> &&sticker)
{
    return make_object<messageSticker>(std::move(sticker), false);
}

object_ptr<secretChat> makeSecretChat(std::int32_t id,
                                      std::int64_t userId,
                                      object_ptr<SecretChatState> &&state,
                                      bool isOutbound,
                                      std::int32_t,
                                      std::string const &keyHash,
                                      std::int32_t layer)
{
    return make_object<secretChat>(id, userId, std::move(state), isOutbound, keyHash, layer);
}

object_ptr<photo> makePhotoRemote(int32_t fileId, unsigned size, unsigned width, unsigned height)
{
    auto result = make_object<photo>();
    auto sz = make_object<photoSize>("x", make_object<file>(fileId, size, size, nullptr, make_object<remoteFile>("", "", false, true, size)), (int32)width, (int32)height, std::vector<int32_t>());
    result->sizes_.push_back(std::move(sz));
    return result;
}

object_ptr<photo> makePhotoLocal(int32_t fileId, unsigned size, const std::string &path,
                                 unsigned width, unsigned height)
{
    auto result = make_object<photo>();
    auto sz = make_object<photoSize>("x", make_object<file>(fileId, size, size, make_object<localFile>(path, true, true, false, true, 0, size, size), make_object<remoteFile>("", "", false, true, size)), (int32)width, (int32)height, std::vector<int32_t>());
    result->sizes_.push_back(std::move(sz));
    return result;
}

object_ptr<photo> makePhotoUploading(int32_t fileId, unsigned size, unsigned uploaded, const std::string &path,
                                     unsigned width, unsigned height)
{
    auto result = make_object<photo>();
    auto sz = make_object<photoSize>("x", make_object<file>(fileId, size, size, make_object<localFile>(path, true, true, false, true, 0, size, size), make_object<remoteFile>("", "", true, false, uploaded)), (int32)width, (int32)height, std::vector<int32_t>());
    result->sizes_.push_back(std::move(sz));
    return result;
}

object_ptr<chatMember> makeChatMember(int32_t userId, int32_t inviteUserId, time_t joinTime,
                                      object_ptr<ChatMemberStatus> &&memberStatus, const void *)
{
    auto result = make_object<chatMember>();
    result->member_id_ = make_object<messageSenderUser>(userId);
    result->inviter_user_id_ = inviteUserId;
    result->joined_chat_date_ = (int32_t)joinTime;
    result->status_ = std::move(memberStatus);
    return result;
}

object_ptr<basicGroupFullInfo> makeBasicGroupFullInfo(const std::string &description,
                                                      int64_t creatorUserId,
                                                      std::vector<object_ptr<chatMember>> &&members,
                                                      const std::string &inviteLink)
{
    object_ptr<chatInviteLink> link = inviteLink.empty() ? nullptr : makeChatInviteLink(inviteLink);
    return make_object<basicGroupFullInfo>(
        nullptr, description, creatorUserId, std::move(members),
        false, false, std::move(link), std::vector<object_ptr<botCommands>>()
    );
}

object_ptr<supergroup> makeSupergroup(int64_t id,
                                      object_ptr<ChatMemberStatus> &&status,
                                      int32_t memberCount,
                                      bool isChannel)
{
    return make_object<supergroup>(
        id, nullptr, 0, std::move(status), memberCount, 0,
        false, false, false, false, false, false, false, false,
        isChannel, false, false, false, false, nullptr, false, false, nullptr, 0, nullptr
    );
}

object_ptr<supergroup> makeForumSupergroup(int64_t id,
                                           object_ptr<ChatMemberStatus> &&status,
                                           int32_t memberCount)
{
    auto result = makeSupergroup(id, std::move(status), memberCount);
    result->is_forum_ = true;
    return result;
}

object_ptr<forumTopicInfo> makeForumTopicInfo(int64_t chatId,
                                              int32_t forumTopicId,
                                              const std::string &name,
                                              bool isGeneral,
                                              bool isClosed,
                                              bool isHidden,
                                              int64_t creatorUserId)
{
    return make_object<forumTopicInfo>(
        chatId,
        forumTopicId,
        name,
        make_object<forumTopicIcon>(0, 0),
        0,
        make_object<messageSenderUser>(creatorUserId),
        isGeneral,
        false,
        isClosed,
        isHidden,
        false
    );
}

object_ptr<forumTopic> makeForumTopic(object_ptr<forumTopicInfo> &&info,
                                      object_ptr<message> &&lastMessage,
                                      int64_t order,
                                      bool isPinned)
{
    return make_object<forumTopic>(
        std::move(info),
        std::move(lastMessage),
        order,
        isPinned,
        0,
        0,
        0,
        0,
        0,
        0,
        make_object<chatNotificationSettings>(),
        nullptr
    );
}

object_ptr<forumTopics> makeForumTopicsPage(
    int32_t totalCount,
    std::vector<object_ptr<forumTopic>> &&topics,
    int32_t nextOffsetDate,
    int64_t nextOffsetMessageId,
    int32_t nextOffsetForumTopicId)
{
    return make_object<forumTopics>(
        totalCount,
        std::move(topics),
        nextOffsetDate,
        nextOffsetMessageId,
        nextOffsetForumTopicId
    );
}

object_ptr<createChatInviteLink> makeInviteLinkRequest(int64_t chatId)
{
    auto result = make_object<createChatInviteLink>();
    result->chat_id_ = chatId;
    return result;
}

object_ptr<chatInviteLink> makeChatInviteLink(const std::string &link)
{
    auto result = make_object<chatInviteLink>();
    result->invite_link_ = link;
    return result;
}

TestTransceiver::TestTransceiver()
    : m_transportContext(g_main_context_new())
{
}

TestTransceiver::~TestTransceiver()
{
    for (GSource *source: m_timeoutSources) {
        g_source_destroy(source);
        g_source_unref(source);
    }
    g_main_context_unref(m_transportContext);
}

void TestTransceiver::send(td::Client::Request &&request)
{
    m_lastReceivedRequestId = request.id;
    m_requests.push(std::move(request));
}

uint64_t TestTransceiver::verifyRequest(const td::td_api::Function &request)
{
    expectedRequest = &request;
    EXPECT_FALSE(m_requests.empty()) << "Expected Request, but no requests received";
    if (m_requests.empty()) return 0;
    auto actual = std::move(m_requests.front());
    m_requests.pop();
    td::td_api::compare_func(*actual.function, *expectedRequest);
    if (actual.function->get_id() == sendMessage::ID) {
        const auto &send = static_cast<const sendMessage &>(*actual.function);
        if (send.input_message_content_ &&
            send.input_message_content_->get_id() == inputMessagePhoto::ID) {
            const auto &photo = static_cast<const inputMessagePhoto &>(*send.input_message_content_);
            if (photo.photo_ &&
                photo.photo_->photo_ &&
                photo.photo_->photo_->get_id() == inputFileLocal::ID) {
                m_inputPhotoPaths.push_back(static_cast<const inputFileLocal &>(*photo.photo_->photo_).path_);
            }
        }
    }
    expectedRequest = nullptr;
    if ((actual.function->get_id() != viewMessages::ID) &&
        (actual.function->get_id() != getContacts::ID)) {
        m_verifiedRequestIds.push(actual.id);
    }
    return actual.id;
}

std::vector<uint64_t> TestTransceiver::verifyRequests(std::vector<td::td_api::object_ptr<td::td_api::Function>> &&requests)
{
    std::vector<uint64_t> requestIds;
    for (auto &req : requests) {
        requestIds.push_back(verifyRequest(*req));
    }
    verifyNoRequests();
    return requestIds;
}

void TestTransceiver::verifyRequests(const std::vector<const td::td_api::Function *> requests)
{
    for (const auto *req : requests) {
        verifyRequest(*req);
    }
    verifyNoRequests();
}

void TestTransceiver::reply(td::td_api::object_ptr<td::td_api::Object> object)
{
    uint64_t requestId = m_lastReceivedRequestId;
    if (!m_verifiedRequestIds.empty()) {
        requestId = m_verifiedRequestIds.front();
        m_verifiedRequestIds.pop();
    } else if (!m_requests.empty()) {
        requestId = m_requests.front().id;
        m_requests.pop();
    }
    receive(td::Client::Response{requestId, std::move(object)});
}

void TestTransceiver::reply(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::queue<uint64_t> remaining;
    bool found = false;
    while (!m_verifiedRequestIds.empty()) {
        if (m_verifiedRequestIds.front() != requestId) {
            remaining.push(m_verifiedRequestIds.front());
        } else {
            found = true;
        }
        m_verifiedRequestIds.pop();
    }
    m_verifiedRequestIds = std::move(remaining);

    if (!found) {
        std::queue<td::Client::Request> remainingRequests;
        while (!m_requests.empty()) {
            if (m_requests.front().id != requestId) {
                remainingRequests.push(std::move(m_requests.front()));
            } else {
                found = true;
            }
            m_requests.pop();
        }
        m_requests = std::move(remainingRequests);
    }

    receive(td::Client::Response{requestId, std::move(object)});
}

void TestTransceiver::runTimeouts()
{
    while (true) {
        pruneTimeoutSources();

        GSource *source = nullptr;
        for (GSource *candidate: m_timeoutSources) {
            if (candidate != g_main_current_source() &&
                !g_source_is_destroyed(candidate) &&
                g_source_get_context(candidate) ==
                    m_transportContext) {
                source = candidate;
                break;
            }
        }

        if (!source)
            return;

        ManualTimeoutSource *manualSource =
            reinterpret_cast<ManualTimeoutSource *>(source);
        manualSource->armed = TRUE;
        g_main_context_wakeup(m_transportContext);

        while (!g_source_is_destroyed(source) &&
               g_main_context_iteration(
                   m_transportContext, FALSE)) {
        }

        // A selected attached source must dispatch once armed. Avoid
        // spinning if runTimeouts() is called recursively from a source
        // which GLib will not dispatch again until the outer call returns.
        if (!g_source_is_destroyed(source))
            return;
    }
}

GMainContext *TestTransceiver::transportContext()
{
    return m_transportContext;
}

GSource *TestTransceiver::createTimeoutSource(unsigned)
{
    static GSourceFuncs sourceFunctions = {
        prepareTimeoutSource,
        checkTimeoutSource,
        dispatchTimeoutSource,
        nullptr,
        nullptr,
        nullptr
    };
    GSource *source = g_source_new(
        &sourceFunctions, sizeof(ManualTimeoutSource));
    reinterpret_cast<ManualTimeoutSource *>(source)->armed = FALSE;

    try {
        m_timeoutSources.push_back(source);
    } catch (...) {
        g_source_unref(source);
        throw;
    }
    // TdTransport receives the original reference. The fake retains one
    // observer reference so cancellation can never leave a dangling pointer
    // in the deterministic timeout queue.
    g_source_ref(source);
    return source;
}

gboolean TestTransceiver::prepareTimeoutSource(
    GSource *source, gint *timeout)
{
    if (timeout)
        *timeout = -1;
    return reinterpret_cast<ManualTimeoutSource *>(source)->armed;
}

gboolean TestTransceiver::checkTimeoutSource(GSource *source)
{
    return reinterpret_cast<ManualTimeoutSource *>(source)->armed;
}

gboolean TestTransceiver::dispatchTimeoutSource(
    GSource *,
    GSourceFunc callback,
    gpointer userData)
{
    return callback ? callback(userData) : FALSE;
}

void TestTransceiver::pruneTimeoutSources()
{
    auto source = m_timeoutSources.begin();
    while (source != m_timeoutSources.end()) {
        if (g_source_is_destroyed(*source)) {
            g_source_unref(*source);
            source = m_timeoutSources.erase(source);
        } else {
            ++source;
        }
    }
}

void TestTransceiver::verifyNoRequests()
{
    EXPECT_TRUE(m_requests.empty()) << "Unexpected request: ";
}

void TestTransceiver::forgetVerifiedRequests()
{
    std::queue<uint64_t> empty;
    m_verifiedRequestIds.swap(empty);
}

std::string TestTransceiver::addInputPhoto(const void *data, size_t size)
{
    std::string path = "/tmp/test_photo_" + std::to_string(m_inputPhotoPaths.size());
    m_inputPhotoPaths.push_back(path);
    return path;
}

static const char sensitiveMarkerA[] =
    "SYNTHETIC_AUTH_VALUE_A_DO_NOT_PRINT";
static const char sensitiveMarkerB[] =
    "SYNTHETIC_AUTH_VALUE_B_DO_NOT_PRINT";

static std::string captureRequestComparisonFailure(
    const Function &actual,
    const Function &expected)
{
    ::testing::TestPartResultArray failures;

    {
        ::testing::ScopedFakeTestPartResultReporter reporter(
            ::testing::ScopedFakeTestPartResultReporter::
                INTERCEPT_ONLY_CURRENT_THREAD,
            &failures);
        compare_func(actual, expected);
    }

    if (failures.size() != 1) {
        ADD_FAILURE()
            << "Expected exactly one intercepted request comparison failure";
        return "";
    }

    return failures.GetTestPartResult(0).message();
}

static void assertSensitiveMarkersAreRedacted(const std::string &message)
{
    if (message.find(sensitiveMarkerA) != std::string::npos ||
        message.find(sensitiveMarkerB) != std::string::npos)
    {
        ADD_FAILURE()
            << "Request comparison failure exposed a synthetic marker";
    }
}

TEST(TestTransceiverHarness, RedactsAuthenticationMismatchDiagnostics)
{
    auto actualPassword =
        make_object<checkAuthenticationPassword>(sensitiveMarkerA);
    auto expectedPassword =
        make_object<checkAuthenticationPassword>(sensitiveMarkerB);

    std::string sameTypeFailure = captureRequestComparisonFailure(
        *actualPassword, *expectedPassword);
    EXPECT_NE(
        sameTypeFailure.find("password_ differs (values redacted)"),
        std::string::npos);
    assertSensitiveMarkersAreRedacted(sameTypeFailure);

    auto actualEmail =
        make_object<setAuthenticationEmailAddress>(sensitiveMarkerA);
    auto expectedEmail =
        make_object<setAuthenticationEmailAddress>(sensitiveMarkerB);
    std::string emailFailure = captureRequestComparisonFailure(
        *actualEmail, *expectedEmail);
    EXPECT_NE(
        emailFailure.find("email_address_ differs (values redacted)"),
        std::string::npos);
    assertSensitiveMarkersAreRedacted(emailFailure);

    auto actualEmailCode =
        make_object<checkAuthenticationEmailCode>(
            make_object<emailAddressAuthenticationCode>(
                sensitiveMarkerA));
    auto expectedEmailCode =
        make_object<checkAuthenticationEmailCode>(
            make_object<emailAddressAuthenticationCode>(
                sensitiveMarkerB));
    std::string emailCodeFailure = captureRequestComparisonFailure(
        *actualEmailCode, *expectedEmailCode);
    EXPECT_NE(
        emailCodeFailure.find(
            "email authentication code differs (values redacted)"),
        std::string::npos);
    assertSensitiveMarkersAreRedacted(emailCodeFailure);

    auto actualParameters = make_object<setTdlibParameters>();
    auto expectedParameters = make_object<setTdlibParameters>();
    actualParameters->api_hash_ = sensitiveMarkerA;
    expectedParameters->api_hash_ = sensitiveMarkerB;
    std::string apiHashFailure = captureRequestComparisonFailure(
        *actualParameters, *expectedParameters);
    EXPECT_NE(
        apiHashFailure.find("api_hash_ differs (values redacted)"),
        std::string::npos);
    assertSensitiveMarkersAreRedacted(apiHashFailure);

    auto expectedPhone =
        make_object<setAuthenticationPhoneNumber>(
            sensitiveMarkerB, nullptr);
    std::string typeFailure = captureRequestComparisonFailure(
        *actualPassword, *expectedPhone);
    EXPECT_NE(
        typeFailure.find("Request type mismatch"),
        std::string::npos);
    assertSensitiveMarkersAreRedacted(typeFailure);
}

TEST(TestTransceiverHarness, MockSendMessagePreservesTypedTopic)
{
    auto request = Mock_SendMessage(
        123,
        make_object<messageTopicForum>(42),
        nullptr,
        nullptr,
        nullptr
    );

    ASSERT_NE(nullptr, request->topic_id_);
    ASSERT_EQ(messageTopicForum::ID, request->topic_id_->get_id());
    EXPECT_EQ(
        42,
        static_cast<const messageTopicForum &>(*request->topic_id_).forum_topic_id_
    );
}

TEST(TestTransceiverHarness, MockViewMessagesPreservesSource)
{
    auto request = Mock_ViewMessages(
        123,
        {10, 11},
        true,
        make_object<messageSourceForumTopicHistory>()
    );

    EXPECT_EQ((std::vector<int64_t>{10, 11}), request->message_ids_);
    ASSERT_NE(nullptr, request->source_);
    EXPECT_EQ(messageSourceForumTopicHistory::ID, request->source_->get_id());
}

TEST(TestTransceiverHarness, ManualTimeoutsAreIsolatedAndOrdered)
{
    struct CallbackData {
        std::vector<int> *order;
        int value;
    };
    const auto recordTimeout = [](gpointer userData) -> gboolean {
        CallbackData *data = static_cast<CallbackData *>(userData);
        data->order->push_back(data->value);
        return FALSE;
    };
    const auto recordUnrelatedSource = [](gpointer userData) -> gboolean {
        *static_cast<bool *>(userData) = true;
        return FALSE;
    };

    TestTransceiver backend;
    std::vector<int> order;
    CallbackData firstData{&order, 1};
    CallbackData secondData{&order, 2};

    GSource *first = backend.createTimeoutSource(30);
    g_source_set_callback(first, recordTimeout, &firstData, nullptr);
    g_source_attach(first, backend.transportContext());
    g_source_unref(first);

    GSource *second = backend.createTimeoutSource(1);
    g_source_set_callback(second, recordTimeout, &secondData, nullptr);
    g_source_attach(second, backend.transportContext());
    g_source_unref(second);

    bool unrelatedSourceCalled = false;
    GSource *unrelatedSource = g_idle_source_new();
    g_source_set_callback(
        unrelatedSource,
        recordUnrelatedSource,
        &unrelatedSourceCalled,
        nullptr);
    g_source_attach(unrelatedSource, g_main_context_default());

    EXPECT_FALSE(g_main_context_iteration(
        backend.transportContext(), FALSE));
    backend.runTimeouts();

    EXPECT_EQ((std::vector<int>{1, 2}), order);
    EXPECT_FALSE(unrelatedSourceCalled);

    g_source_destroy(unrelatedSource);
    g_source_unref(unrelatedSource);
}

TEST(TestTransceiverHarness, IgnoresResponseAfterOwnerIsDestroyed)
{
    TestTransceiver backend;
    {
        TdTransceiver transceiver(nullptr, nullptr, nullptr, &backend);
    }

    backend.update(make_object<updateConnectionState>(make_object<connectionStateReady>()));
}

TEST(TestTransceiverHarness, ReplyDrainsOnlyTransportDeliveries)
{
    TestTransceiver backend;
    TdTransceiver transceiver(nullptr, nullptr, nullptr, &backend);
    bool responseCalled = false;
    bool unrelatedSourceCalled = false;
    const auto recordUnrelatedSource = [](gpointer userData) -> gboolean {
        *static_cast<bool *>(userData) = true;
        return FALSE;
    };

    transceiver.sendQuery(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            responseCalled = true;
        });
    backend.verifyRequest(getMe());

    GSource *unrelatedSource = g_idle_source_new();
    g_source_set_callback(
        unrelatedSource,
        recordUnrelatedSource,
        &unrelatedSourceCalled,
        nullptr);
    g_source_attach(unrelatedSource, g_main_context_default());

    backend.reply(make_object<ok>());

    EXPECT_TRUE(responseCalled);
    EXPECT_FALSE(unrelatedSourceCalled);

    g_source_destroy(unrelatedSource);
    g_source_unref(unrelatedSource);
}

TEST(TestTransceiverHarness, TimeoutCallbackMayDestroyTransceiver)
{
    TestTransceiver backend;
    std::unique_ptr<TdTransceiver> transceiver(
        new TdTransceiver(nullptr, nullptr, nullptr, &backend));
    bool callbackCalled = false;

    transceiver->sendQueryWithTimeout(
        make_object<getMe>(),
        [&](uint64_t, object_ptr<Object>) {
            callbackCalled = true;
            transceiver.reset();
        },
        1);
    backend.verifyRequest(getMe());

    backend.runTimeouts();

    EXPECT_TRUE(callbackCalled);
    EXPECT_EQ(nullptr, transceiver);
}

}
}
