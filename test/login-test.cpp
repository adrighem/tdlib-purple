#include "fixture.h"

class LoginTest: public CommTest {};

TEST_F(LoginTest, Login)
{
    login();
}

TEST_F(LoginTest, ConnectionReadyBeforeAuthReady)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequests({
        make_object<disableProxy>(),
        make_object<getProxies>(),
        make_object<setTdlibParameters>(
            false,
            std::string(purple_user_dir()) + G_DIR_SEPARATOR_S +
            "tdlib" + G_DIR_SEPARATOR_S + "+" + selfPhoneNumber,
            "",
            "",
            false,
            false,
            false,
            true, // use secret chats
            0,
            "",
            "",
            "",
            "",
            ""
        )
    });
    tgl.reply(make_object<ok>()); // disableProxy (ignored)
    tgl.reply(make_object<addedProxies>(std::vector<object_ptr<addedProxy>>())); // getProxies
    tgl.reply(make_object<ok>()); // setTdlibParameters

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitPhoneNumber>()));

    tgl.verifyRequest(setAuthenticationPhoneNumber("+" + selfPhoneNumber, nullptr));
    tgl.reply(make_object<ok>());
    tgl.update(make_object<updateConnectionState>(make_object<connectionStateConnecting>()));
    tgl.update(make_object<updateConnectionState>(make_object<connectionStateReady>()));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateReady>()));
    prpl.verifyEvents(ConnectionSetStateEvent(connection, PURPLE_CONNECTED));

    tgl.verifyRequest(getContacts());
    tgl.update(make_object<updateUser>(makeUser(
        selfId,
        selfFirstName,
        selfLastName,
        selfPhoneNumber, // Phone number here without + to make it more interesting
        make_object<userStatusOffline>()
    )));
    tgl.reply(makeUsers({}));

    tgl.verifyRequest(*getChatsRequest());
    prpl.verifyNoEvents();
    tgl.reply(getChatsNoChatsResponse());

    prpl.verifyEvents(
        AccountSetAliasEvent(account, selfFirstName + " " + selfLastName),
        ShowAccountEvent(account)
    );
}

TEST_F(LoginTest, RegisterNewAccount_WithAlias_ConnectionReadyBeforeAuthReady)
{
    purple_account_set_alias(account, (selfFirstName + " " + selfLastName).c_str());
    prpl.discardEvents();
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequests({
        make_object<disableProxy>(),
        make_object<getProxies>(),
        make_object<setTdlibParameters>(
            false,
            std::string(purple_user_dir()) + G_DIR_SEPARATOR_S +
            "tdlib" + G_DIR_SEPARATOR_S + "+" + selfPhoneNumber,
            "",
            "",
            false,
            false,
            false,
            true, // use secret chats
            0,
            "",
            "",
            "",
            "",
            ""
        )
    });
    tgl.reply(make_object<ok>()); // disableProxy (ignored)
    tgl.reply(make_object<addedProxies>(std::vector<object_ptr<addedProxy>>())); // getProxies
    tgl.reply(make_object<ok>()); // setTdlibParameters

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitPhoneNumber>()));

    tgl.verifyRequest(setAuthenticationPhoneNumber("+" + selfPhoneNumber, nullptr));
    tgl.reply(make_object<ok>());

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitCode>(
            make_object<authenticationCodeInfo>(
                selfPhoneNumber,
                make_object<authenticationCodeTypeTelegramMessage>(5),
                make_object<authenticationCodeTypeSms>(5),
                1800
            )
        )
    ));

    prpl.verifyEvents(RequestInputEvent(connection, account, NULL, NULL));
    prpl.inputEnter("12345");
    tgl.verifyRequest(checkAuthenticationCode("12345"));
    tgl.reply(make_object<ok>());

    tgl.update(make_object<updateConnectionState>(make_object<connectionStateConnecting>()));
    tgl.update(make_object<updateConnectionState>(make_object<connectionStateReady>()));

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitRegistration>(
            make_object<termsOfService>(
                make_object<formattedText>(
                    "Terms of service",
                    std::vector<object_ptr<textEntity>>()
                ),
                0, false
            )
        )
    ));

    tgl.verifyRequest(registerUser(selfFirstName, selfLastName, false));
    tgl.reply(make_object<ok>());
    prpl.verifyNoEvents();

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateReady>()));
    prpl.verifyEvents(ConnectionSetStateEvent(connection, PURPLE_CONNECTED));

    tgl.verifyRequest(getContacts());
    tgl.update(make_object<updateUser>(makeUser(
        selfId,
        selfFirstName,
        selfLastName,
        selfPhoneNumber, // Phone number here without + to make it more interesting
        make_object<userStatusOffline>()
    )));
    tgl.reply(makeUsers({}));

    tgl.verifyRequest(*getChatsRequest());
    prpl.verifyNoEvents();
    tgl.reply(getChatsNoChatsResponse());

    prpl.verifyEvents(
        AccountSetAliasEvent(account, selfFirstName + " " + selfLastName),
        ShowAccountEvent(account)
    );
}

TEST_F(LoginTest, RegisterNewAccount_NoAlias)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequests({
        make_object<disableProxy>(),
        make_object<getProxies>(),
        make_object<setTdlibParameters>(
            false,
            std::string(purple_user_dir()) + G_DIR_SEPARATOR_S +
            "tdlib" + G_DIR_SEPARATOR_S + "+" + selfPhoneNumber,
            "",
            "",
            false,
            false,
            false,
            true, // use secret chats
            0,
            "",
            "",
            "",
            "",
            ""
        )
    });
    tgl.reply(make_object<ok>()); // disableProxy (ignored)
    tgl.reply(make_object<addedProxies>(std::vector<object_ptr<addedProxy>>())); // getProxies
    tgl.reply(make_object<ok>()); // setTdlibParameters

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitPhoneNumber>()));

    tgl.verifyRequest(setAuthenticationPhoneNumber("+" + selfPhoneNumber, nullptr));
    tgl.reply(make_object<ok>());

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitCode>(
            make_object<authenticationCodeInfo>(
                selfPhoneNumber,
                make_object<authenticationCodeTypeTelegramMessage>(5),
                make_object<authenticationCodeTypeSms>(5),
                1800
            )
        )
    ));

    prpl.verifyEvents(RequestInputEvent(connection, account, NULL, NULL));
    prpl.inputEnter("12345");
    tgl.verifyRequest(checkAuthenticationCode("12345"));
    tgl.reply(make_object<ok>());

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitRegistration>(
            make_object<termsOfService>(
                make_object<formattedText>(
                    "Terms of service",
                    std::vector<object_ptr<textEntity>>()
                ),
                0, false
            )
        )
    ));

    tgl.verifyNoRequests();
    prpl.verifyEvents(RequestInputEvent(connection, account, NULL, NULL));

    prpl.inputEnter((selfFirstName + "     " + selfLastName).c_str());
    tgl.verifyRequest(registerUser(selfFirstName, selfLastName, false));
    tgl.reply(make_object<ok>());
    tgl.update(make_object<updateUser>(makeUser(selfId, selfFirstName, selfLastName, selfPhoneNumber, make_object<userStatusOffline>())));

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateReady>()));
    prpl.verifyEvents(ConnectionSetStateEvent(connection, PURPLE_CONNECTED));

    tgl.update(make_object<updateConnectionState>(make_object<connectionStateReady>()));
    tgl.verifyRequest(getContacts());
    tgl.reply(makeUsers({}));

    tgl.verifyRequest(*getChatsRequest());
    tgl.reply(getChatsNoChatsResponse());

    prpl.verifyEvents(
        AccountSetAliasEvent(account, selfFirstName + " " + selfLastName),
        ShowAccountEvent(account)
    );
}

TEST_F(LoginTest, TwoFactorAuthentication)
{
    purple_account_set_alias(account, (selfFirstName + " " + selfLastName).c_str());
    prpl.discardEvents();
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequests({
        make_object<disableProxy>(),
        make_object<getProxies>(),
        make_object<setTdlibParameters>(
            false,
            std::string(purple_user_dir()) + G_DIR_SEPARATOR_S +
            "tdlib" + G_DIR_SEPARATOR_S + "+" + selfPhoneNumber,
            "",
            "",
            false,
            false,
            false,
            true, // use secret chats
            0,
            "",
            "",
            "",
            "",
            ""
        )
    });
    tgl.reply(make_object<ok>()); // disableProxy (ignored)
    tgl.reply(make_object<addedProxies>(std::vector<object_ptr<addedProxy>>())); // getProxies
    tgl.reply(make_object<ok>()); // setTdlibParameters

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitPhoneNumber>()));

    tgl.verifyRequest(setAuthenticationPhoneNumber("+" + selfPhoneNumber, nullptr));
    tgl.reply(make_object<ok>());

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitCode>(
            make_object<authenticationCodeInfo>(
                selfPhoneNumber,
                make_object<authenticationCodeTypeTelegramMessage>(5),
                make_object<authenticationCodeTypeSms>(5),
                1800
            )
        )
    ));

    prpl.verifyEvents(RequestInputEvent(connection, account, NULL, NULL));
    prpl.inputEnter("12345");
    tgl.verifyRequest(checkAuthenticationCode("12345"));
    tgl.reply(make_object<ok>());

    tgl.update(make_object<updateConnectionState>(make_object<connectionStateConnecting>()));
    tgl.update(make_object<updateConnectionState>(make_object<connectionStateReady>()));
    prpl.verifyNoEvents();

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitPassword>(
        "hint", true, false, "user@example.com"
    )));

    prpl.verifyEvents(RequestInputEvent(connection, account, NULL, NULL));
    prpl.inputEnter("password");
    tgl.verifyRequest(checkAuthenticationPassword("password"));
    tgl.reply(make_object<ok>());
    tgl.update(make_object<updateUser>(makeUser(selfId, selfFirstName, selfLastName, selfPhoneNumber, make_object<userStatusOffline>())));

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateReady>()));
    prpl.verifyEvents(ConnectionSetStateEvent(connection, PURPLE_CONNECTED));

    tgl.verifyRequest(getContacts());
    tgl.reply(makeUsers({}));

    tgl.verifyRequest(*getChatsRequest());
    tgl.reply(getChatsNoChatsResponse());

    prpl.verifyEvents(
        AccountSetAliasEvent(account, selfFirstName + " " + selfLastName),
        ShowAccountEvent(account)
    );
}

TEST_F(LoginTest, RenameBuddyAtConnect)
{
    purple_blist_add_buddy(purple_buddy_new(account, purpleUserName(0).c_str(), "whatever"), NULL,
                           &standardPurpleGroup, NULL);
    prpl.discardEvents();

    std::vector<object_ptr<Object>> extraUpdates;
    extraUpdates.push_back(standardUpdateUser(0u));
    extraUpdates.push_back(standardPrivateChat(0));
    extraUpdates.push_back(makeUpdateChatListMain(chatIds[0]));

    login(
        std::move(extraUpdates),
        make_object<users>(1, std::vector<int64_t>{userIds[0]}),
        make_object<chats>(1, std::vector<int64_t>(1, chatIds[0])),
        {
            std::make_shared<AliasBuddyEvent>(purpleUserName(0), userFirstNames[0] + " " + userLastNames[0]),
        }, {},
        {
            std::make_shared<UserStatusEvent>(account, purpleUserName(0), PURPLE_STATUS_AWAY),
            std::make_shared<AccountSetAliasEvent>(account, selfFirstName + " " + selfLastName),
            std::make_shared<ShowAccountEvent>(account)
        }
    );
}

TEST_F(LoginTest, RenameBuddy)
{
    loginWithOneContact();

    purple_blist_alias_buddy(purple_find_buddy(account, purpleUserName(0).c_str()), "New Name");
    prpl.discardEvents();
    pluginInfo().alias_buddy(connection, purpleUserName(0).c_str(), "New Name");

    tgl.verifyRequest(addContact(userIds[0], make_object<importedContact>(
        "", "New", "Name", nullptr
    ), true));

    tgl.update(make_object<updateChatTitle>(chatIds[0], "New Name"));
    object_ptr<td::td_api::updateUser> updateUser = td::move_tl_object_as<td::td_api::updateUser>(this->standardUpdateUser(0u));
    updateUser->user_->first_name_ = "New";
    updateUser->user_->last_name_ = "Name";
    tgl.update(std::move(updateUser));
}

TEST_F(LoginTest, BuddyRenamedByServer)
{
    loginWithOneContact();

    tgl.update(make_object<updateChatTitle>(chatIds[0], "New Name"));
    prpl.verifyEvents(AliasBuddyEvent(purpleUserName(0), "New Name"));

    object_ptr<td::td_api::updateUser> updateUser = td::move_tl_object_as<td::td_api::updateUser>(standardUpdateUser(0u));
    updateUser->user_->first_name_ = "New";
    updateUser->user_->last_name_ = "Name";
    tgl.update(std::move(updateUser));
}

TEST_F(LoginTest, AddedProxyCofiguration)
{
    char host[] = "host";
    const int port = 10;
    char username[] = "username";
    char password[] = "password";
    PurpleProxyInfo purpleProxy;
    purpleProxy.type = PURPLE_PROXY_SOCKS5;
    purpleProxy.host = host;
    purpleProxy.port = port;
    purpleProxy.username = username;
    purpleProxy.password = password;
    account->proxy_info = &purpleProxy;

    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequests({
        make_object<disableProxy>(),
        make_object<addProxy>(make_object<proxy>(host, port, make_object<proxyTypeSocks5>(username, password)), true, ""),
        make_object<getProxies>(),
        make_object<setTdlibParameters>(
            false,
            std::string(purple_user_dir()) + G_DIR_SEPARATOR_S +
            "tdlib" + G_DIR_SEPARATOR_S + "+" + selfPhoneNumber,
            "",
            "",
            false,
            false,
            false,
            true, // use secret chats
            0,
            "",
            "",
            "",
            "",
            ""
        )
    });

    tgl.reply(make_object<ok>()); // reply to disableProxy
    tgl.reply(make_object<addedProxy>(2, 0, false, "", nullptr));
    std::vector<object_ptr<addedProxy>> proxyList;
    proxyList.push_back(make_object<addedProxy>(2, 0, true, "", nullptr));
    tgl.reply(make_object<addedProxies>(std::move(proxyList)));
    tgl.reply(make_object<ok>());
}

TEST_F(LoginTest, ChangedProxyCofiguration)
{
    char host[] = "host";
    const int port = 10;
    char username[] = "username";
    char password[] = "password";
    PurpleProxyInfo purpleProxy;
    purpleProxy.type = PURPLE_PROXY_SOCKS5;
    purpleProxy.host = host;
    purpleProxy.port = port;
    purpleProxy.username = username;
    purpleProxy.password = password;
    account->proxy_info = &purpleProxy;

    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequests({
        make_object<disableProxy>(),
        make_object<addProxy>(make_object<proxy>(host, port, make_object<proxyTypeSocks5>(username, password)), true, ""),
        make_object<getProxies>(),
        make_object<setTdlibParameters>(
            false,
            std::string(purple_user_dir()) + G_DIR_SEPARATOR_S +
            "tdlib" + G_DIR_SEPARATOR_S + "+" + selfPhoneNumber,
            "",
            "",
            false,
            false,
            false,
            true, // use secret chats
            0,
            "",
            "",
            "",
            "",
            ""
        )
    });

    tgl.reply(make_object<ok>()); // reply to disableProxy
    tgl.reply(make_object<addedProxy>(2, 0, false, "", nullptr));
    std::vector<object_ptr<addedProxy>> proxyList;
    proxyList.push_back(make_object<addedProxy>(1, 0, false, "", nullptr));
    proxyList.push_back(make_object<addedProxy>(2, 0, true, "", nullptr));
    tgl.reply(make_object<addedProxies>(std::move(proxyList)));
    tgl.reply(make_object<ok>());

    tgl.verifyRequest(removeProxy(1));
    tgl.reply(make_object<ok>());
}

TEST_F(LoginTest, RemovedProxyCofiguration)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequests({
        make_object<disableProxy>(),
        make_object<getProxies>(),
        make_object<setTdlibParameters>(
            false,
            std::string(purple_user_dir()) + G_DIR_SEPARATOR_S +
            "tdlib" + G_DIR_SEPARATOR_S + "+" + selfPhoneNumber,
            "",
            "",
            false,
            false,
            false,
            true, // use secret chats
            0,
            "",
            "",
            "",
            "",
            ""
        )
    });

    tgl.reply(make_object<ok>()); // reply to disableProxy
    std::vector<object_ptr<addedProxy>> proxyList;
    proxyList.push_back(make_object<addedProxy>(1, 0, false, "", nullptr));
    tgl.reply(make_object<addedProxies>(std::move(proxyList)));
    tgl.reply(make_object<ok>());

    tgl.verifyRequest(removeProxy(1));
    tgl.reply(make_object<ok>());
}

TEST_F(LoginTest, getChatsSequence)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequests({
        make_object<disableProxy>(),
        make_object<getProxies>(),
        make_object<setTdlibParameters>(
            false,
            std::string(purple_user_dir()) + G_DIR_SEPARATOR_S +
            "tdlib" + G_DIR_SEPARATOR_S + "+" + selfPhoneNumber,
            "",
            "",
            false,
            false,
            false,
            true, // use secret chats
            0,
            "",
            "",
            "",
            "",
            ""
        )
    });
    tgl.reply(make_object<ok>()); // disableProxy (ignored)
    tgl.reply(make_object<addedProxies>(std::vector<object_ptr<addedProxy>>())); // getProxies
    tgl.reply(make_object<ok>()); // setTdlibParameters

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitPhoneNumber>()));

    tgl.verifyRequest(setAuthenticationPhoneNumber("+" + selfPhoneNumber, nullptr));
    tgl.reply(make_object<ok>());
    tgl.update(make_object<updateConnectionState>(make_object<connectionStateConnecting>()));
    tgl.update(make_object<updateConnectionState>(make_object<connectionStateReady>()));
    tgl.verifyNoRequests();
    prpl.verifyNoEvents();

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateReady>()));
    prpl.verifyEvents(ConnectionSetStateEvent(connection, PURPLE_CONNECTED));

    tgl.verifyRequest(getContacts());
    tgl.update(make_object<updateUser>(makeUser(
        selfId,
        selfFirstName,
        selfLastName,
        selfPhoneNumber, // Phone number here without + to make it more interesting
        make_object<userStatusOffline>()
    )));
    tgl.reply(makeUsers({}));

    tgl.verifyRequest(loadChats(make_object<chatListMain>(), 200));

    object_ptr<updateNewChat> chat1 = td::move_tl_object_as<updateNewChat>(standardPrivateChat(0, make_object<chatListMain>()));
    object_ptr<updateNewChat> chat2 = td::move_tl_object_as<updateNewChat>(standardPrivateChat(1, make_object<chatListMain>()));
    object_ptr<updateNewChat> chat3 = td::move_tl_object_as<updateNewChat>(standardPrivateChat(1, make_object<chatListMain>()));
    chat3->chat_->id_ = chatIds[1]+1;
    tgl.update(std::move(chat1));
    tgl.update(std::move(chat2));
    tgl.update(std::move(chat3));
    tgl.update(make_object<updateChatPosition>(
        chatIds[1]+1, make_object<chatPosition>(
            make_object<chatListMain>(), 15, false, nullptr
        )
    ));
    tgl.update(make_object<updateChatPosition>(
        chatIds[0], make_object<chatPosition>(
            make_object<chatListArchive>(), 10, false, nullptr
        )
    ));
    tgl.reply(make_object<ok>());

    tgl.verifyRequest(*getChatsRequest());
    tgl.update(standardPrivateChat(1));
    tgl.reply(getChatsNoChatsResponse());

    // updateUser were missing (not realistic though), so no buddies
    prpl.verifyEvents(
        AccountSetAliasEvent(account, selfFirstName + " " + selfLastName),
        ShowAccountEvent(account)
    );
}

TEST_F(LoginTest, KeepInlineDownloads)
{
    purple_account_set_bool(account, "keep-inline-downloads", TRUE);
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequests({
        make_object<disableProxy>(),
        make_object<getProxies>(),
        make_object<setTdlibParameters>(
            false,
            std::string(purple_user_dir()) + G_DIR_SEPARATOR_S +
            "tdlib" + G_DIR_SEPARATOR_S + "+" + selfPhoneNumber,
            "",
            "",
            false,
            false,
            false,
            true, // use secret chats
            0,
            "",
            "",
            "",
            "",
            ""
        )
    });
}

TEST_F(LoginTest, IncomingGroupChatMessageAtLoginWhileChatListStillNull)
{
    std::cout << "DEBUG: test start\n";
    const int32_t     groupId             = 700;
    const int64_t     groupChatId         = 7000;
    const std::string groupChatTitle      = "Title";
    const std::string groupChatPurpleName = "chat" + std::to_string(groupChatId);
    constexpr int64_t messageId    = 10000;
    constexpr int32_t date         = 123456;
    constexpr int     purpleChatId = 1;

    GHashTable *components = g_hash_table_new_full (g_str_hash, g_str_equal, NULL, g_free);
    g_hash_table_insert(components, (char *)"id", g_strdup((groupChatPurpleName).c_str()));
    purple_blist_add_chat(purple_chat_new(account, groupChatTitle.c_str(), components), NULL, NULL);
    prpl.discardEvents();

    std::vector<object_ptr<Object>> extraUpdates;
    extraUpdates.push_back(standardUpdateUser(0u));
    extraUpdates.push_back(make_object<updateBasicGroup>(make_object<basicGroup>(
        groupId, 2, make_object<chatMemberStatusMember>(), true, 0
    )));
    extraUpdates.push_back(make_object<updateNewChat>(makeChat(
        groupChatId, make_object<chatTypeBasicGroup>(groupId), groupChatTitle, nullptr, 0, 0, 0
    )));
    extraUpdates.push_back(make_object<updateNewMessage>(makeMessage(
        messageId, userIds[0], groupChatId, false, date, makeTextMessage("text")
    )));
    extraUpdates.push_back(makeUpdateChatListMain(groupChatId));

    std::vector<object_ptr<td::TlObject>> postUpdateRequests;
    postUpdateRequests.push_back(make_object<viewMessages>(groupChatId, std::vector<int64_t>(1, messageId), nullptr, true));
    postUpdateRequests.push_back(make_object<getBasicGroupFullInfo>(groupId));

    login(
        std::move(extraUpdates),
        makeUsers({}), make_object<ok>(),
        {
            // Removal is unnecessary but nothing too bad is happening
            std::make_shared<RemoveChatEvent>(groupChatPurpleName, ""),
            std::make_shared<ServGotJoinedChatEvent>(connection, purpleChatId, groupChatPurpleName, groupChatPurpleName),
            std::make_shared<ConvSetTitleEvent>(groupChatPurpleName, groupChatTitle),
            std::make_shared<ServGotChatEvent>(connection, purpleChatId, userFirstNames[0] + " " + userLastNames[0],
                             "text", PURPLE_MESSAGE_RECV, date),
            std::make_shared<AddChatEvent>(groupChatPurpleName, groupChatTitle, account, nullptr, nullptr)
        },
        std::move(postUpdateRequests)
    );
}

