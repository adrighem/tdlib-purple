#include "fixture.h"
#include "application-credentials-test-backend.h"
#include "purple-info.h"
#include "td-client.h"
#include "tdlib-purple.h"
#include <td/telegram/Client.h>
#include <td/telegram/td_api.h>

using namespace td::td_api;

namespace {

static_assert(
    noexcept(PurpleTdClient::disableTdlibLogging()),
    "TDLib logging setup must not throw through the plugin load callback");

class FutureAuthorizationState final : public AuthorizationState {
public:
    std::int32_t get_id() const final
    {
        return 123456789;
    }

    void store(td::TlStorerToString &, const char *) const final
    {}
};

} // namespace

class LoginTest: public CommTest {
protected:
    void startAuthorization()
    {
        pluginInfo().login(account);
        prpl.verifyEvents(
            ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
            ConnectionUpdateProgressEvent(connection, 1, 2));

        tgl.update(make_object<updateAuthorizationState>(
            make_object<authorizationStateWaitTdlibParameters>()));
        tgl.verifyRequestsV(
            make_object<disableProxy>(),
            make_object<getProxies>(),
            makeDefaultParams());
        tgl.reply(make_object<ok>());
        tgl.reply(make_object<addedProxies>(
            std::vector<object_ptr<addedProxy>>()));
        tgl.reply(make_object<ok>());
    }
};

TEST_F(LoginTest, TdlibInternalLoggingIsDisabled)
{
    auto raisedVerbosity = td::Client::execute(
        {0, make_object<setLogVerbosityLevel>(1)});
    ASSERT_NE(raisedVerbosity.object, nullptr);
    ASSERT_EQ(raisedVerbosity.object->get_id(), ok::ID);

    PurplePluginInfo *info = getPluginInfo();
    ASSERT_NE(info, nullptr);
    ASSERT_NE(info->load, nullptr);
    ASSERT_TRUE(info->load(nullptr));

    auto verbosity = td::Client::execute(
        {0, make_object<getLogVerbosityLevel>()});

    ASSERT_NE(verbosity.object, nullptr);
    ASSERT_EQ(verbosity.object->get_id(), logVerbosityLevel::ID);

    const auto &level =
        static_cast<const logVerbosityLevel &>(*verbosity.object);
    EXPECT_EQ(level.verbosity_level_, 0);

    auto stream = td::Client::execute({0, make_object<getLogStream>()});
    ASSERT_NE(stream.object, nullptr);
    EXPECT_EQ(stream.object->get_id(), logStreamEmpty::ID);
}

TEST_F(LoginTest, Login)
{
    login({}, nullptr, make_object<error>(404, "Not Found"));
}

TEST_F(LoginTest, CompleteAccountCredentialsOverrideApplicationCredentials)
{
    const int32_t overrideApiId = 7654321;
    const std::string overrideApiHash =
        "fedcba9876543210fedcba9876543210";
    tdlib_purple_test_application_credentials_set_unavailable();
    purple_account_set_string(
        account, AccountOptions::ApiId,
        std::to_string(overrideApiId).c_str());
    purple_account_set_string(
        account, AccountOptions::ApiHash, overrideApiHash.c_str());

    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitTdlibParameters>()));
    auto expectedParameters = makeDefaultParams();
    expectedParameters->api_id_ = overrideApiId;
    expectedParameters->api_hash_ = overrideApiHash;
    tgl.verifyRequestsV(
        make_object<disableProxy>(),
        make_object<getProxies>(),
        std::move(expectedParameters)
    );
}

TEST_F(LoginTest, ApplicationCredentialsAreSnapshottedAtLogin)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    const std::string replacementHash =
        "abcdef0123456789abcdef0123456789";
    tdlib_purple_test_application_credentials_set(
        7654321, replacementHash.data(), replacementHash.size());

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequestsV(
        make_object<disableProxy>(),
        make_object<getProxies>(),
        makeDefaultParams()
    );
}

TEST_F(LoginTest, IncompleteAccountCredentialsRejectLoginBeforeTdlib)
{
    purple_account_set_string(account, AccountOptions::ApiId, "7654321");

    pluginInfo().login(account);

    EXPECT_EQ(purple_connection_get_protocol_data(connection), nullptr);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConnectionErrorEvent(
        connection,
        "Telegram application credentials are missing or invalid"));
}

TEST_F(LoginTest, MalformedAccountCredentialsRejectLoginBeforeTdlib)
{
    purple_account_set_string(account, AccountOptions::ApiId, "not-an-id");
    purple_account_set_string(
        account, AccountOptions::ApiHash,
        "fedcba9876543210fedcba9876543210");

    pluginInfo().login(account);

    EXPECT_EQ(purple_connection_get_protocol_data(connection), nullptr);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConnectionErrorEvent(
        connection,
        "Telegram application credentials are missing or invalid"));
}

TEST_F(LoginTest, UnavailableApplicationCredentialsRejectLoginBeforeTdlib)
{
    tdlib_purple_test_application_credentials_set_unavailable();

    pluginInfo().login(account);

    EXPECT_EQ(purple_connection_get_protocol_data(connection), nullptr);
    tgl.verifyNoRequests();
    prpl.verifyEvents(ConnectionErrorEvent(
        connection,
        "Telegram application credentials are missing or invalid"));
}

TEST_F(LoginTest, ConnectionReadyBeforeAuthReady)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2)
    );

    tgl.update(make_object<updateAuthorizationState>(make_object<authorizationStateWaitTdlibParameters>()));
    tgl.verifyRequestsV(
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
            applicationApiId,
            applicationApiHash,
            "",
            "",
            "",
            ""
        )
    );
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
    tgl.verifyRequestsV(
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
            applicationApiId,
            applicationApiHash,
            "",
            "",
            "",
            ""
        )
    );
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
    tgl.verifyRequestsV(
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
            applicationApiId,
            applicationApiHash,
            "",
            "",
            "",
            ""
        )
    );
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
    tgl.verifyRequestsV(
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
            applicationApiId,
            applicationApiHash,
            "",
            "",
            "",
            ""
        )
    );
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

    prpl.verifyEvents(RequestInputEvent(connection, account, NULL, NULL, TRUE));
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

TEST_F(LoginTest, EmailAuthenticationRetriesWithoutExposingErrorText)
{
    static const char responseMarker[] =
        "SYNTHETIC_EMAIL_ERROR_SECRET_DO_NOT_PRINT";
    startAuthorization();

    auto emailState = []() {
        return make_object<updateAuthorizationState>(
            make_object<authorizationStateWaitEmailAddress>(
                true, false));
    };
    tgl.update(emailState());
    tgl.update(emailState());
    prpl.verifyEvents(
        RequestInputEvent(connection, account, NULL, NULL));

    prpl.inputEnter("person@example.invalid");
    tgl.verifyRequest(setAuthenticationEmailAddress(
        "person@example.invalid"));
    prpl.captureNotifyEvents();
    tgl.reply(make_object<error>(400, responseMarker));
    prpl.verifyEvents(
        NotifyMessageEvent(
            account,
            PURPLE_NOTIFY_MSG_ERROR,
            "Authentication error",
            "Telegram rejected the authentication response. Please try again.",
            nullptr),
        RequestInputEvent(connection, account, NULL, NULL));

    prpl.inputEnter("retry@example.invalid");
    tgl.verifyRequest(setAuthenticationEmailAddress(
        "retry@example.invalid"));
    tgl.reply(make_object<ok>());

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitEmailCode>(
            false,
            false,
            make_object<emailAddressAuthenticationCodeInfo>(
                "p***@example.invalid", 6),
            nullptr)));
    prpl.verifyEvents(
        RequestInputEvent(connection, account, NULL, NULL));
    prpl.inputEnter("123456");
    tgl.verifyRequest(checkAuthenticationEmailCode(
        make_object<emailAddressAuthenticationCode>("123456")));
    tgl.reply(make_object<ok>());
}

TEST_F(LoginTest, RepeatedParameterStateDoesNotRepeatProxySetup)
{
    pluginInfo().login(account);
    prpl.verifyEvents(
        ConnectionSetStateEvent(connection, PURPLE_CONNECTING),
        ConnectionUpdateProgressEvent(connection, 1, 2));

    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitTdlibParameters>()));
    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitTdlibParameters>()));

    tgl.verifyRequestsV(
        make_object<disableProxy>(),
        make_object<getProxies>(),
        makeDefaultParams());
    tgl.reply(make_object<ok>());
    tgl.reply(make_object<addedProxies>(
        std::vector<object_ptr<addedProxy>>()));
    tgl.reply(make_object<ok>());
}

TEST_F(LoginTest, NullAuthorizationStateFailsExplicitly)
{
    startAuthorization();

    tgl.update(make_object<updateAuthorizationState>(nullptr));

    prpl.verifyEvents(ConnectionErrorEvent(
        connection,
        "Telegram returned an invalid authentication state"));
}

TEST_F(LoginTest, EmptyAuthenticationInputIsRedisplayed)
{
    startAuthorization();
    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitCode>()));
    prpl.verifyEvents(
        RequestInputEvent(connection, account, NULL, NULL));

    prpl.captureNotifyEvents();
    prpl.inputEnter("");
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        NotifyMessageEvent(
            account,
            PURPLE_NOTIFY_MSG_ERROR,
            "Authentication error",
            "Required authentication input was empty. Please try again.",
            nullptr),
        RequestInputEvent(connection, account, NULL, NULL));

    prpl.inputEnter("12345");
    tgl.verifyRequest(checkAuthenticationCode("12345"));
    tgl.reply(make_object<ok>());
}

TEST_F(LoginTest, RejectedAliasFallsBackToEditableRegistration)
{
    purple_account_set_alias(account, "Stored Alias");
    prpl.discardEvents();
    startAuthorization();
    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitRegistration>()));
    tgl.verifyRequest(registerUser("Stored", "Alias", false));

    prpl.captureNotifyEvents();
    tgl.reply(make_object<error>(
        400, "SYNTHETIC_REGISTRATION_ERROR_DO_NOT_PRINT"));
    tgl.verifyNoRequests();
    prpl.verifyEvents(
        NotifyMessageEvent(
            account,
            PURPLE_NOTIFY_MSG_ERROR,
            "Authentication error",
            "Telegram rejected the authentication response. Please try again.",
            nullptr),
        RequestInputEvent(connection, account, NULL, NULL));

    prpl.inputEnter("Corrected Name");
    tgl.verifyRequest(registerUser("Corrected", "Name", false));
    tgl.reply(make_object<ok>());
}

TEST_F(LoginTest, CancellingEmailAuthenticationEndsLogin)
{
    startAuthorization();
    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitEmailAddress>(
            false, false)));
    prpl.verifyEvents(
        RequestInputEvent(connection, account, NULL, NULL));

    prpl.inputCancel();

    prpl.verifyEvents(ConnectionErrorEvent(
        connection, "Authentication email required"));
    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateReady>()));
    prpl.verifyNoEvents();
}

TEST_F(LoginTest, PhoneRejectionIsStableAndDoesNotRetryAutomatically)
{
    startAuthorization();
    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitPhoneNumber>()));
    tgl.verifyRequest(setAuthenticationPhoneNumber(
        "+" + selfPhoneNumber, nullptr));

    tgl.reply(make_object<error>(
        400, "SYNTHETIC_PHONE_ERROR_SECRET_DO_NOT_PRINT"));

    prpl.verifyEvents(ConnectionErrorEvent(
        connection, "Telegram rejected the configured phone number"));
    tgl.verifyNoRequests();
}

TEST_F(LoginTest, UnsupportedPremiumAuthenticationFailsExplicitly)
{
    startAuthorization();
    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitPremiumPurchase>(
            "product", 30, "support@example.invalid", "subject")));

    prpl.verifyEvents(ConnectionErrorEvent(
        connection,
        "Telegram Premium authentication is not supported"));
    tgl.verifyNoRequests();
}

TEST_F(LoginTest, UnsupportedQrAuthenticationFailsExplicitly)
{
    startAuthorization();
    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateWaitOtherDeviceConfirmation>(
            "tg://login?token=SYNTHETIC_QR_CREDENTIAL")));

    prpl.verifyEvents(ConnectionErrorEvent(
        connection,
        "QR authentication is not supported by Purple 2"));
    tgl.verifyNoRequests();
}

TEST_F(LoginTest, TerminalAuthorizationStateFailsBeforeReady)
{
    startAuthorization();
    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateClosing>()));

    prpl.verifyEvents(ConnectionErrorEvent(
        connection,
        "Telegram authorization ended before login completed"));
    tgl.update(make_object<updateAuthorizationState>(
        make_object<authorizationStateClosed>()));
    prpl.verifyNoEvents();
}

TEST_F(LoginTest, FutureAuthorizationStateFailsExplicitly)
{
    startAuthorization();
    object_ptr<AuthorizationState> future(
        new FutureAuthorizationState());
    tgl.update(make_object<updateAuthorizationState>(
        std::move(future)));

    prpl.verifyEvents(ConnectionErrorEvent(
        connection,
        "Telegram requested an unsupported authentication step"));
    tgl.verifyNoRequests();
}

TEST_F(LoginTest, LocalBuddyAliasPreservedAtConnect)
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
        make_object<error>(404, "Not Found"),
        {}, {},
        {
            std::make_shared<UserStatusEvent>(account, purpleUserName(0), PURPLE_STATUS_AWAY),
            std::make_shared<AccountSetAliasEvent>(account, selfFirstName + " " + selfLastName),
            std::make_shared<ShowAccountEvent>(account)
        }
    );

    PurpleBuddy *buddy = purple_find_buddy(account, purpleUserName(0).c_str());
    ASSERT_NE(nullptr, buddy);
    ASSERT_STREQ("whatever", purple_buddy_get_alias_only(buddy));
    ASSERT_STREQ((userFirstNames[0] + " " + userLastNames[0]).c_str(), purple_buddy_get_server_alias(buddy));
    ASSERT_STREQ("whatever", purple_buddy_get_alias(buddy));
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

TEST_F(LoginTest, RenameBuddyKeepsUtf8)
{
    loginWithOneContact();

    const std::string alias = "Ælün Várenth";
    purple_blist_alias_buddy(purple_find_buddy(account, purpleUserName(0).c_str()), alias.c_str());
    prpl.discardEvents();
    pluginInfo().alias_buddy(connection, purpleUserName(0).c_str(), alias.c_str());

    tgl.verifyRequest(addContact(userIds[0], make_object<importedContact>(
        "", "Ælün", "Várenth", nullptr
    ), true));
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
    tgl.verifyRequestsV(
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
            applicationApiId,
            applicationApiHash,
            "",
            "",
            "",
            ""
        )
    );

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
    tgl.verifyRequestsV(
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
            applicationApiId,
            applicationApiHash,
            "",
            "",
            "",
            ""
        )
    );

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
    tgl.verifyRequestsV(
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
            applicationApiId,
            applicationApiHash,
            "",
            "",
            "",
            ""
        )
    );

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
    tgl.verifyRequestsV(
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
            applicationApiId,
            applicationApiHash,
            "",
            "",
            "",
            ""
        )
    );
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
    tgl.verifyRequestsV(
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
            applicationApiId,
            applicationApiHash,
            "",
            "",
            "",
            ""
        )
    );
}

TEST_F(LoginTest, IncomingGroupChatMessageAtLoginWhileChatListStillNull)
{
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

    std::vector<object_ptr<BaseObject>> postUpdateRequests;
    postUpdateRequests.push_back(Mock_ViewMessages(groupChatId, std::vector<int64_t>(1, messageId), true));
    postUpdateRequests.push_back(make_object<getBasicGroupFullInfo>(groupId));

    login(
        std::move(extraUpdates),
        makeUsers({}), make_object<error>(404, "Not Found"),
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
