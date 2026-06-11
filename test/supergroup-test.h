#ifndef _SUPERGROUP_TEST_H
#define _SUPERGROUP_TEST_H

#include "fixture.h"

class SupergroupTest: public CommTest {
protected:
    const int32_t     groupId             = 700;
    const int64_t     groupChatId         = -7000;
    const std::string groupChatTitle      = "Title";
    const std::string groupChatPurpleName = "chat" + std::to_string(groupChatId);

    void loginWithSupergroup(::td::td_api::object_ptr<::td::td_api::supergroupFullInfo> fullInfo = nullptr,
                             ::td::td_api::object_ptr<::td::td_api::chatMembers> recentMembers = nullptr,
                             ::td::td_api::object_ptr<::td::td_api::chatMembers> administrators = nullptr);
};

#endif
