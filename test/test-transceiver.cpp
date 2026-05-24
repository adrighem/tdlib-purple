#include "test-transceiver.h"
#include "format.h"
#include <td/telegram/td_api.h>
#include <gtest/gtest.h>
#include <iostream>

#define COMPARE(param) ASSERT_EQ(expected.param, actual.param)

namespace td {
namespace td_api {

void compare(const formattedText &actual, const formattedText &expected)
{
    ASSERT_EQ(expected.text_, actual.text_);
    ASSERT_EQ(expected.entities_.size(), actual.entities_.size());
}

void compare(const setTdlibParameters &actual, const setTdlibParameters &expected)
{
    COMPARE(database_directory_);
    COMPARE(use_secret_chats_);
}

void compare(const setAuthenticationPhoneNumber &actual, const setAuthenticationPhoneNumber &expected)
{
    COMPARE(phone_number_);
    COMPARE(settings_ != nullptr);
}

void compare(const checkAuthenticationCode &actual, const checkAuthenticationCode &expected)
{
    COMPARE(code_);
}

void compare(const checkAuthenticationPassword &actual, const checkAuthenticationPassword &expected)
{
    COMPARE(password_);
}

void compare(const proxyTypeSocks5 &actual, const proxyTypeSocks5 &expected)
{
    COMPARE(username_);
    COMPARE(password_);
}

void compare(const proxy &actual, const proxy &expected)
{
    COMPARE(server_);
    COMPARE(port_);
    COMPARE(type_ != nullptr);
    if (actual.type_ != nullptr) {
        COMPARE(type_->get_id());
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

void compare(const inputMessageDocument &actual, const inputMessageDocument &expected)
{
    COMPARE(document_ != nullptr);
    COMPARE(caption_ != nullptr);
}

void compare(const viewMessages &actual, const viewMessages &expected)
{
    COMPARE(chat_id_);
    COMPARE(message_ids_);
    COMPARE(force_read_);
}

void compare(const sendMessage &actual, const sendMessage &expected)
{
    COMPARE(chat_id_);
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
    COMPARE(invite_link_);
}

void compare(const getMessage &actual, const getMessage &expected)
{
    COMPARE(chat_id_);
    COMPARE(message_id_);
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

void compare(const registerUser &actual, const registerUser &expected)
{
    COMPARE(first_name_);
    COMPARE(last_name_);
}

void compare(const addContact &actual, const addContact &expected)
{
    COMPARE(user_id_);
    COMPARE(share_phone_number_);
}

std::string requestToString(const td::td_api::Function &request)
{
    return td::td_api::to_string(request);
}

void compare_func(const td::td_api::Function &actual, const td::td_api::Function &expected)
{
    ASSERT_EQ(expected.get_id(), actual.get_id()) <<
        requestToString(actual) << " expected " << requestToString(expected);

#define C(class) case class::ID: \
    compare(static_cast<const class &>(actual), static_cast<const class &>(expected)); \
    break;

    switch (actual.get_id()) {
        C(setTdlibParameters)
        C(setAuthenticationPhoneNumber)
        C(checkAuthenticationCode)
        C(checkAuthenticationPassword)
        C(registerUser)
        case getContacts::ID: break;
        C(getChats)
        C(loadChats)
        C(viewMessages)
        C(downloadFile)
        case sendMessage::ID:
            compare(static_cast<const sendMessage &>(actual), static_cast<const sendMessage &>(expected));
            break;
        C(getBasicGroupFullInfo)
        C(joinChatByInviteLink)
        C(getMessage)
        C(sendChatAction)
        C(addProxy)
        C(addContact)
        case disableProxy::ID: break;
        case getProxies::ID: break;
        C(removeProxy)
        default: EXPECT_TRUE(false) << "Unsupported request " << requestToString(actual);
    }
}

object_ptr<user> makeUser(std::int32_t id_, std::string const &first_name_,
                          std::string const &last_name_,
                          std::string const &phone_number_,
                          object_ptr<UserStatus> &&status_)
{
    auto result = make_object<user>();
    result->id_ = id_;
    result->first_name_ = first_name_;
    result->last_name_ = last_name_;
    result->phone_number_ = phone_number_;
    result->status_ = std::move(status_);
    result->type_ = make_object<userTypeRegular>();
    result->is_contact_ = true;
    return result;
}

object_ptr<users> makeUsers(std::vector<int64_t> user_ids)
{
    return make_object<users>(static_cast<int32_t>(user_ids.size()), std::move(user_ids));
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
    result->permissions_ = make_object<chatPermissions>(true, true, true, true, true, true, true, true, true, true, true, true, true, true, true, true);
    result->notification_settings_ = make_object<chatNotificationSettings>();
    return result;
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
    return make_object<error>(404, "No more chats");
}

object_ptr<message> makeMessage(std::int64_t id_, std::int32_t sender_user_id_, std::int64_t chat_id_,
                                bool is_outgoing_, std::int32_t date_, object_ptr<MessageContent> &&content_)
{
    auto result = make_object<message>();
    result->id_ = id_;
    result->sender_id_ = make_object<messageSenderUser>(sender_user_id_);
    result->chat_id_ = chat_id_;
    result->sending_state_ = is_outgoing_ ? make_object<messageSendingStatePending>(0) : nullptr;
    result->is_outgoing_ = is_outgoing_;
    result->date_ = date_;
    result->content_ = std::move(content_);
    return result;
}

object_ptr<messageText> makeTextMessage(const std::string &text)
{
    auto result = make_object<messageText>();
    result->text_ = make_object<formattedText>(text, std::vector<object_ptr<textEntity>>());
    return result;
}

object_ptr<photo> makePhotoRemote(int32_t fileId, unsigned size, unsigned width, unsigned height)
{
    std::vector<object_ptr<photoSize>> sizes;
    sizes.push_back(make_object<photoSize>(
        "whatever",
        make_object<file>(
            fileId, size, size,
            make_object<localFile>("", true, true, false, false, 0, 0, 0),
            make_object<remoteFile>("beh", "bleh", false, true, size)
        ),
        width, height,
        std::vector<int32_t>()
    ));
    return make_object<photo>(false, nullptr, std::move(sizes));
}

object_ptr<photo> makePhotoLocal(int32_t fileId, unsigned size, const std::string &path,
                                 unsigned width, unsigned height)
{
    std::vector<object_ptr<photoSize>> sizes;
    sizes.push_back(make_object<photoSize>(
        "whatever",
        make_object<file>(
            fileId, size, size,
            make_object<localFile>(path, true, true, false, true, 0, size, size),
            make_object<remoteFile>("beh", "bleh", false, true, size)
        ),
        width, height,
        std::vector<int32_t>()
    ));
    return make_object<photo>(false, nullptr, std::move(sizes));
}

object_ptr<photo> makePhotoUploading(int32_t fileId, unsigned size, unsigned uploaded, const std::string &path,
                                     unsigned width, unsigned height)
{
    EXPECT_TRUE(uploaded < size);

    std::vector<object_ptr<photoSize>> sizes;
    sizes.push_back(make_object<photoSize>(
        "whatever",
        make_object<file>(
            fileId, size, size,
            make_object<localFile>(path, true, true, false, true, 0, size, size),
            make_object<remoteFile>("beh", "bleh", false, false, uploaded)
        ),
        width, height,
        std::vector<int32_t>()
    ));
    return make_object<photo>(false, nullptr, std::move(sizes));
}

object_ptr<chatMember> makeChatMember(int32_t userId, int32_t inviteUserId, time_t joinTime,
                                      object_ptr<ChatMemberStatus> &&memberStatus, const void *)
{
    auto result = make_object<chatMember>();
    result->member_id_ = make_object<messageSenderUser>(userId);
    result->inviter_user_id_ = inviteUserId;
    result->joined_chat_date_ = static_cast<int32_t>(joinTime);
    result->status_ = std::move(memberStatus);
    return result;
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

}
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
    expectedRequest = nullptr;
    m_verifiedRequestIds.push(actual.id);
    return actual.id;
}

std::vector<uint64_t> TestTransceiver::verifyRequests(std::initializer_list<td::td_api::object_ptr<td::td_api::Function>> requests)
{
    std::vector<uint64_t> ids;
    for (const auto &req : requests) {
        ids.push_back(verifyRequest(*req));
    }
    return ids;
}

void TestTransceiver::verifyRequests(const std::vector<const td::td_api::Function *> requests)
{
    for (const auto *req : requests) {
        verifyRequest(*req);
    }
}

void TestTransceiver::reply(td::td_api::object_ptr<td::td_api::Object> object)
{
    uint64_t requestId = m_lastReceivedRequestId;
    if (!m_verifiedRequestIds.empty()) {
        requestId = m_verifiedRequestIds.front();
        m_verifiedRequestIds.pop();
    }
    receive(td::Client::Response{requestId, std::move(object)});
}

void TestTransceiver::reply(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    std::queue<uint64_t> remaining;
    while (!m_verifiedRequestIds.empty()) {
        if (m_verifiedRequestIds.front() != requestId) {
            remaining.push(m_verifiedRequestIds.front());
        }
        m_verifiedRequestIds.pop();
    }
    m_verifiedRequestIds = std::move(remaining);
    receive(td::Client::Response{requestId, std::move(object)});
}

void TestTransceiver::update(td::td_api::object_ptr<td::td_api::Object> object)
{
    receive(td::Client::Response{0, std::move(object)});
}

void TestTransceiver::runTimeouts()
{
    for (auto &timer : m_timers) {
        timer.function(timer.data);
    }
}

guint TestTransceiver::addTimeout(guint interval, GSourceFunc function, gpointer data)
{
    m_timers.push_back({m_nextTimerId++, function, data});
    return m_timers.back().id;
}

void TestTransceiver::cancelTimer(guint id)
{
    for (auto it = m_timers.begin(); it != m_timers.end(); ++it) {
        if (it->id == id) {
            m_timers.erase(it);
            return;
        }
    }
}

void TestTransceiver::verifyNoRequests()
{
    EXPECT_TRUE(m_requests.empty()) << "Unexpected request: ";
}