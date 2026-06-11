#include "fixture.h"
#include "libpurple-mock.h"
#include "tdlib-purple.h"
#include "purple-events.h"
#include <cstring>

using namespace td::td_api;

void fixture_init()
{
    static PurplePlugin purplePlugin;
    static bool initialized = false;
    if (initialized)
        return;

    memset(&purplePlugin, 0, sizeof(purplePlugin));
    purple_init_plugin(&purplePlugin);
    if (purplePlugin.info && purplePlugin.info->load)
        purplePlugin.info->load(&purplePlugin);
    tgprpl_set_single_thread();
    initialized = true;
}

CommTest::CommTest()
: prpl(g_purpleEvents)
{
    fixture_init();
    account = purple_account_new(("+" + selfPhoneNumber).c_str(), "prpl-telegram");
    connection = new PurpleConnection;
    connection->state = PURPLE_DISCONNECTED;
    connection->account = account;
    purple_connection_set_protocol_data(connection, NULL);
    account->gc = connection;
    tgprpl_set_test_backend(&tgl);
    prpl.discardEvents();
}

CommTest::~CommTest()
{
    if (purple_connection_get_protocol_data(connection))
        pluginInfo().close(connection);
    delete connection;
    purple_account_destroy(account);
}

void CommTest::SetUp() {
    setUiName("pidgin");
}
void CommTest::TearDown()
{
    if (purple_connection_get_protocol_data(connection))
        pluginInfo().close(connection);
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();
    tgl.runTimeouts();
}

void CommTest::login(std::vector<object_ptr<Object>> &&extraUpdates,
                     object_ptr<users> &&getContactsReply,
                     object_ptr<Object> &&getChatsReply,
                     std::vector<std::shared_ptr<PurpleEvent>> &&postUpdateEvents,
                     std::vector<object_ptr<BaseObject>> &&postUpdateSequence,
                     std::vector<std::shared_ptr<PurpleEvent>> &&postChatListEvents)
{
    this->pluginInfo().login(account);

    tgl.update(make_object<updateConnectionState>(make_object<connectionStateReady>()));
    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));

    // Verifying connection events from login()
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    std::vector<object_ptr<Function>> loginReqs;
    loginReqs.push_back(td::move_tl_object_as<Function>(make_object<disableProxy>()));
    loginReqs.push_back(td::move_tl_object_as<Function>(make_object<getProxies>()));
    loginReqs.push_back(td::move_tl_object_as<Function>(this->makeDefaultParams()));
    tgl.verifyRequests(std::move(loginReqs));

    tgl.reply(make_object<ok>());
    tgl.reply(make_object<addedProxies>(std::vector<object_ptr<addedProxy>>()));
    tgl.reply(make_object<ok>());

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateReady>()));

    // onLoggedIn() generates these events
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTED)
    );

    uint64_t contactRequestId = tgl.verifyRequest(*make_object<getContacts>());
    tgl.update(make_object<updateUser>(makeUser(selfId, selfFirstName, selfLastName, selfPhoneNumber, make_object<userStatusOffline>())));

    for (auto &update: extraUpdates) {
        tgl.update(std::move(update));
    }
    prpl.verifyEvents2(std::move(postUpdateEvents));

    for (auto &item: postUpdateSequence) {
        if (!item) {
            continue;
        }
        switch (item->get_id()) {
        case getBasicGroupFullInfo::ID:
        case getSupergroupFullInfo::ID:
        case getSupergroupMembers::ID:
        case viewMessages::ID:
            tgl.verifyRequest(*static_cast<Function *>(item.get()));
            break;
        default: {
            BaseObject *raw = item.release();
            tgl.reply(object_ptr<Object>(static_cast<Object *>(raw)));
            break;
        }
        }
    }
    tgl.verifyNoRequests();
    tgl.forgetVerifiedRequests();

    tgl.update(make_object<updateConnectionState>(make_object<connectionStateReady>()));

    if (!getContactsReply) {
        getContactsReply = make_object<users>(0, std::vector<int64_t>());
    }
    tgl.reply(contactRequestId, std::move(getContactsReply));

    uint64_t chatsRequestId = tgl.verifyRequest(*getChatsRequest());
    if (!getChatsReply) {
        getChatsReply = make_object<error>(404, "Not Found");
    }
    bool hasChats = (getChatsReply && getChatsReply->get_id() == td::td_api::ok::ID);
    tgl.reply(chatsRequestId, std::move(getChatsReply));

    if (hasChats) {
        tgl.verifyRequest(*getChatsRequest());
        tgl.reply(getChatsNoChatsResponse());
    }

    if ((postChatListEvents.size() == 1) && (postChatListEvents.front() == nullptr)) {
        std::vector<std::shared_ptr<PurpleEvent>> expectedEvents;
        expectedEvents.push_back(std::make_shared<AccountSetAliasEvent>(account, selfFirstName + " " + selfLastName));
        expectedEvents.push_back(std::make_shared<ShowAccountEvent>(account));
        prpl.verifyEvents2(std::move(expectedEvents));
    } else {
        prpl.verifyEvents2(std::move(postChatListEvents));
    }
}

void CommTest::loginWithOneContact()
{
    std::vector<object_ptr<Object>> updates;
    updates.push_back(td::move_tl_object_as<Object>(standardUpdateUser(0)));
    updates.push_back(standardPrivateChat(0));
    updates.push_back(td::move_tl_object_as<Object>(makeUpdateChatListMain(chatIds[0])));

    std::vector<std::shared_ptr<PurpleEvent>> postUpdates;
    postUpdates.push_back(std::make_shared<AddBuddyEvent>(purpleUserName(0), userFirstNames[0] + " " + userLastNames[0], account, nullptr, nullptr, nullptr));

    std::vector<std::shared_ptr<PurpleEvent>> events;
    events.push_back(std::make_shared<UserStatusEvent>(account, purpleUserName(0), PURPLE_STATUS_AWAY));
    events.push_back(std::make_shared<AccountSetAliasEvent>(account, selfFirstName + " " + selfLastName));
    events.push_back(std::make_shared<ShowAccountEvent>(account));

    login(
        std::move(updates),
        std::move(make_object<users>(1, std::vector<int64_t>(1, userIds[0]))),
        std::move(make_object<error>(404, "Not Found")), // loadChats loops while returning ok, so return 404 to stop
        std::move(postUpdates), std::vector<object_ptr<BaseObject>>(), std::move(events)
    );
}

object_ptr<updateUser> CommTest::standardUpdateUser(unsigned index)
{
    auto user = makeUser(userIds[index], userFirstNames[index], userLastNames[index], userPhones[index], make_object<userStatusOffline>());
    user->is_contact_ = true;
    return make_object<updateUser>(std::move(user));
}

object_ptr<updateUser> CommTest::standardUpdateUserNoPhone(unsigned index)
{
    return make_object<updateUser>(makeUser(userIds[index], userFirstNames[index], userLastNames[index], "", make_object<userStatusOffline>()));
}

object_ptr<Object> CommTest::standardPrivateChat(unsigned index, object_ptr<ChatList> chatList)
{
    auto chatType = make_object<chatTypePrivate>(userIds[index]);
    auto chatObj = makeChat(chatIds[index], std::move(chatType), userFirstNames[index] + " " + userLastNames[index], nullptr, 0, 0, 0);
    if (chatList) {
        auto pos = make_object<chatPosition>(std::move(chatList), 1, false, nullptr);
        chatObj->positions_.push_back(std::move(pos));
    }
    return td::move_tl_object_as<Object>(make_object<updateNewChat>(std::move(chatObj)));
}

object_ptr<setTdlibParameters> CommTest::makeDefaultParams()
{
    return make_object<setTdlibParameters>(
        false, "purple_user_dir/tdlib/+1234567", "", "", false, false, false, true, 0, "", "en", "Desktop", "1.0", "1.0"
    );
}

PurplePluginProtocolInfo &CommTest::pluginInfo()
{
    extern PurplePluginProtocolInfo prpl_info;
    return prpl_info;
}

std::string purpleUserName(unsigned index)
{
    return "id" + std::to_string(100 + index);
}

void checkFile(const char *filename, void *content, unsigned size)
{
    gchar *actualContent;
    gsize  actualSize;
    ASSERT_TRUE(g_file_get_contents(filename, &actualContent, &actualSize, NULL)) << filename << " does not exist";
    ASSERT_EQ(actualSize, size) << "Wrong file size for " << filename;
    ASSERT_EQ(0, memcmp(content, actualContent, size)) << "Wrong content for " << filename;
    g_free(actualContent);
}
