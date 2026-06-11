#ifndef _FIXTURE_H_
#define _FIXTURE_H_

#include <td/telegram/td_api.h>
#include "test-transceiver.h"
#include "purple-events.h"
#include "identifiers.h"
#include <gtest/gtest.h>
#include <string>
#include <vector>
#include <memory>

class CommTest: public testing::Test {
public:
    CommTest();
    virtual ~CommTest();

    PurpleEventReceiver &prpl;
    ::td::td_api::TestTransceiver      tgl;
    PurpleAccount       *account;
    PurpleConnection    *connection;

    const std::string selfPhoneNumber   = "1234567";
    const int         selfId            = 1;
    const std::string selfFirstName     = "Isaac";
    const std::string selfLastName      = "Newton";
    const std::string selfPurpleName    = "id1";
    const std::string replyPattern      = "<b>&gt; {} wrote:</b>\n&gt; {}\n{}";

    const int32_t     userIds[2]        = {100, 101};
    const int64_t     chatIds[2]        = {1000, 1001};
    const std::string userPhones[2]     = {"00001", "00002"};
    const std::string userFirstNames[2] = {"Gottfried", "Galileo"};
    const std::string userLastNames[2]  = {"Leibniz", "Galilei"};

    const int64_t     msgIdNew[3]       = {100, 101, 102};
    const int64_t     msgIdOld[3]       = {10, 11, 12};

    void SetUp() override;
    void TearDown() override;

    void login(std::vector<::td::td_api::object_ptr<::td::td_api::Object>> &&extraUpdates = std::vector<::td::td_api::object_ptr<::td::td_api::Object>>(),
               ::td::td_api::object_ptr<::td::td_api::users> &&getContactsReply = nullptr,
               ::td::td_api::object_ptr<::td::td_api::Object> &&getChatsReply = nullptr,
               std::vector<std::shared_ptr<PurpleEvent>> &&postUpdateEvents = std::vector<std::shared_ptr<PurpleEvent>>(),
               std::vector<::td::td_api::object_ptr<::td::td_api::BaseObject>> &&postUpdateSequence = std::vector<::td::td_api::object_ptr<::td::td_api::BaseObject>>(),
               std::vector<std::shared_ptr<PurpleEvent>> &&postChatListEvents = std::vector<std::shared_ptr<PurpleEvent>>(1, nullptr));

    template<typename... Args>
    void loginV(::td::td_api::object_ptr<::td::td_api::users> &&getContactsReply,
                ::td::td_api::object_ptr<::td::td_api::Object> &&getChatsReply,
                Args&&... extraUpdates) {
        std::vector<::td::td_api::object_ptr<::td::td_api::Object>> updates;
        (updates.push_back(::td::move_tl_object_as<::td::td_api::Object>(std::move(extraUpdates))), ...);
        login(std::move(updates), std::move(getContactsReply), std::move(getChatsReply));
    }

    template<typename... Args>
    void loginFullV(std::vector<::td::td_api::object_ptr<::td::td_api::Object>> &&extraUpdates,
                    ::td::td_api::object_ptr<::td::td_api::users> &&getContactsReply,
                    ::td::td_api::object_ptr<::td::td_api::Object> &&getChatsReply,
                    std::vector<std::shared_ptr<PurpleEvent>> &&postUpdateEvents,
                    std::vector<::td::td_api::object_ptr<::td::td_api::BaseObject>> &&postUpdateSequence,
                    std::vector<std::shared_ptr<PurpleEvent>> &&postChatListEvents) {
        login(std::move(extraUpdates), std::move(getContactsReply), std::move(getChatsReply),
              std::move(postUpdateEvents), std::move(postUpdateSequence), std::move(postChatListEvents));
    }
    void loginWithOneContact();
    void runTimeouts() { tgl.runTimeouts(); }

    ::td::td_api::object_ptr<::td::td_api::updateUser>     standardUpdateUser(unsigned index);
    ::td::td_api::object_ptr<::td::td_api::updateUser>     standardUpdateUserNoPhone(unsigned index);
    ::td::td_api::object_ptr<::td::td_api::Object>         standardPrivateChat(unsigned index, ::td::td_api::object_ptr<::td::td_api::ChatList> chatList = nullptr);
    ::td::td_api::object_ptr<::td::td_api::setTdlibParameters> makeDefaultParams();
    PurplePluginProtocolInfo  &pluginInfo();
};

std::string purpleUserName(unsigned index);

void checkFile(const char *filename, void *content, unsigned size);

#endif
