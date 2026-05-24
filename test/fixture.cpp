#include "fixture.h"
#include "format.h"
#include "purple-info.h"
#include "tdlib-purple.h"
#include "printout.h"
#include <iostream>

CommTest::CommTest()
{
    purplePlugin.info = getPluginInfo();
    purple_init_plugin(&purplePlugin);
}

void CommTest::SetUp()
{
    tgprpl_set_test_backend(&tgl);
    prpl.discardEvents();
    account = purple_account_new(("+" + selfPhoneNumber).c_str(), NULL);
    connection = new PurpleConnection;
    connection->state = PURPLE_DISCONNECTED;
    connection->account = account;
    purple_connection_set_protocol_data(connection, NULL);
    account->gc = connection;
    prpl.discardEvents();
    setUiName("Pidgin");
}

void CommTest::TearDown()
{
    if (purple_connection_get_protocol_data(connection))
        pluginInfo().close(connection);
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    delete connection;
    account->gc = NULL;
    tgl.runTimeouts();
    purple_account_destroy(account);
    account = NULL;
    clearFakeFiles();
}

static bool isFunction(const td::TlObject &object)
{
    return requestToString(object).substr(0, 3) != "Id ";
}

static bool isObject(const td::TlObject &object)
{
    return responseToString(object).substr(0, 3) != "Id ";
}

void CommTest::login(std::vector<object_ptr<Object>> extraUpdates, object_ptr<users> getContactsReply,
                     object_ptr<Object> getChatsReply,
                     std::vector<std::shared_ptr<PurpleEvent>> postUpdateEvents,
                     std::vector<object_ptr<td::TlObject>> postUpdateRequestsAndResponses,
                     std::vector<std::shared_ptr<PurpleEvent>> postChatListEvents)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.verifyRequests({
        td::move_tl_object_as<Function>(make_object<disableProxy>()),
        td::move_tl_object_as<Function>(make_object<getProxies>()),
        td::move_tl_object_as<Function>(makeDefaultParams())
    });
    tgl.reply(make_object<ok>()); // disableProxy (ignored)
    tgl.reply(make_object<addedProxies>(std::vector<object_ptr<addedProxy>>())); // getProxies
    tgl.reply(make_object<ok>()); // setTdlibParameters

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateReady>()));
    prpl.verifyEvents(ConnectionSetStateEvent(connection, PURPLE_CONNECTED));
    uint64_t contactRequestId = tgl.verifyRequest(*make_object<getContacts>());

    tgl.update(make_object<updateConnectionState>(make_object<connectionStateConnecting>()));
    tgl.update(make_object<updateConnectionState>(make_object<connectionStateUpdating>()));
    prpl.verifyNoEvents();
    tgl.verifyNoRequests();

    tgl.update(make_object<updateUser>(makeUser(
        selfId,
        selfFirstName,
        selfLastName,
        selfPhoneNumber, // Phone number here without + to make it more interesting
        make_object<userStatusOffline>()
    )));
    for (auto &update: extraUpdates) {
        tgl.update(std::move(update));
    }
    prpl.verifyEvents2(std::move(postUpdateEvents));

    std::vector<const Function *> postUpdateRequests;
    std::vector<object_ptr<td::TlObject>> postUpdateResponses;
    for (auto &object: postUpdateRequestsAndResponses) {
        if (isFunction(*object))
            postUpdateRequests.push_back(static_cast<const Function*>(object.get()));
        else
            postUpdateResponses.push_back(std::move(object));
    }

    tgl.update(make_object<updateConnectionState>(make_object<connectionStateReady>()));

    tgl.reply(contactRequestId, std::move(getContactsReply));

    // Verify requests triggered by getContactsResponse (e.g. getBasicGroupFullInfo)
    tgl.verifyRequests(postUpdateRequests);
    for (auto &response : postUpdateResponses) {
        tgl.reply(td::move_tl_object_as<Object>(std::move(response)));
    }

    tgl.verifyRequest(*getChatsRequest());
    bool hasChats = (getChatsReply && getChatsReply->get_id() == td::td_api::ok::ID);
    tgl.reply(std::move(getChatsReply));
    if (hasChats) {
        tgl.verifyRequest(*getChatsRequest());
        tgl.reply(getChatsNoChatsResponse());
    }

    if (postChatListEvents.empty()) {
        prpl.verifyEvents(
            AccountSetAliasEvent(account, selfFirstName + " " + selfLastName),
            ShowAccountEvent(account)
        );
    } else {
        prpl.verifyEvents2(std::move(postChatListEvents));
    }
}

void CommTest::loginWithOneContact()
{
    std::vector<object_ptr<Object>> extraUpdates;
    extraUpdates.push_back(standardUpdateUser(0u));
    extraUpdates.push_back(standardPrivateChat(0u));
    extraUpdates.push_back(makeUpdateChatListMain(chatIds[0]));

    std::vector<std::shared_ptr<PurpleEvent>> updateEvents;
    updateEvents.push_back(std::make_shared<AddBuddyEvent>(purpleUserName(0), userFirstNames[0] + " " + userLastNames[0], account, nullptr, nullptr, nullptr));

    std::vector<std::shared_ptr<PurpleEvent>> chatListEvents;
    chatListEvents.push_back(std::make_shared<UserStatusEvent>(account, purpleUserName(0), PURPLE_STATUS_AWAY));
    chatListEvents.push_back(std::make_shared<AccountSetAliasEvent>(account, selfFirstName + " " + selfLastName));
    chatListEvents.push_back(std::make_shared<ShowAccountEvent>(account));

    login(std::move(extraUpdates), makeUsers({userIds[0]}), make_object<ok>(), std::move(updateEvents), {}, std::move(chatListEvents));
}

object_ptr<Object> CommTest::standardUpdateUser(unsigned index)
{
    return make_object<updateUser>(makeUser(
        userIds[index], userFirstNames[index], userLastNames[index], userPhones[index],
        make_object<userStatusOffline>()
    ));
}

object_ptr<Object> CommTest::standardUpdateUserNoPhone(unsigned index)
{
    return make_object<updateUser>(makeUser(
        userIds[index], userFirstNames[index], userLastNames[index], "",
        make_object<userStatusOffline>()
    ));
}

object_ptr<Object> CommTest::standardPrivateChat(unsigned index, object_ptr<ChatList> chatList)
{
    return make_object<updateNewChat>(makeChat(
        chatIds[index],
        make_object<chatTypePrivate>(userIds[index]),
        userFirstNames[index] + " " + userLastNames[index],
        nullptr, 0, 0, 0
    ));
}

PurplePluginProtocolInfo &CommTest::pluginInfo()
{
    return *(PurplePluginProtocolInfo *)purplePlugin.info->extra_info;
}

object_ptr<td::td_api::setTdlibParameters> CommTest::makeDefaultParams()
{
    return make_object<setTdlibParameters>(
        false,
        std::string(purple_user_dir()) + G_DIR_SEPARATOR_S + "tdlib" + G_DIR_SEPARATOR_S + "+" + selfPhoneNumber,
        "",
        "",
        false,
        false,
        false,
        true,
        0,
        "",
        "",
        "",
        "",
        ""
    );
}

void checkFile(const char *filename, void *content, unsigned size)
{
    gchar *actualContent;
    gsize  actualSize;
    ASSERT_TRUE(g_file_get_contents(filename, &actualContent, &actualSize, NULL)) << filename << " does not exist";
    ASSERT_EQ(actualSize, size) << "Wrong file size for " << filename;
    ASSERT_EQ(0, memcmp(content, actualContent, size)) << "Wrong content for " << filename;
}
