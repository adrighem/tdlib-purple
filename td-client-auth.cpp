#include "td-client.h"
#include "purple-info.h"
#include "config.h"
#include "format.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void PurpleTdClient::processAuthorizationState(td::td_api::AuthorizationState &authState)
{
    switch (authState.get_id()) {
    case td::td_api::authorizationStateWaitEmailAddress::ID:
        purple_debug_misc(config::pluginId, "Authorization email requested\n");
        requestAuthEmail();
        break;

    case td::td_api::authorizationStateWaitEmailCode::ID:
        purple_debug_misc(config::pluginId, "Authorization email confirmation code requested\n");
        requestAuthEmailCode();
        break;

    case td::td_api::authorizationStateWaitTdlibParameters::ID:
        purple_debug_misc(config::pluginId, "Authorization state update: TDLib parameters requested\n");
        m_transceiver.sendQuery(td::td_api::make_object<td::td_api::disableProxy>(), nullptr);
        if (addProxy()) {
            m_transceiver.sendQuery(td::td_api::make_object<td::td_api::getProxies>(),
                                    &PurpleTdClient::getProxiesResponse);
            sendTdlibParameters();
        }
        break;

    case td::td_api::authorizationStateWaitPhoneNumber::ID:
        purple_debug_misc(config::pluginId, "Authorization state update: phone number requested\n");
        sendPhoneNumber();
        break;

    case td::td_api::authorizationStateWaitCode::ID: {
        auto &codeState = static_cast<td::td_api::authorizationStateWaitCode &>(authState);
        purple_debug_misc(config::pluginId, "Authorization state update: authentication code requested\n");
        requestAuthCode(codeState.code_info_.get());
        break;
    }

    case td::td_api::authorizationStateWaitRegistration::ID: {
        purple_debug_misc(config::pluginId, "Authorization state update: new user registration\n");
        registerUser();
        break;
    }

    case td::td_api::authorizationStateWaitPassword::ID: {
        purple_debug_misc(config::pluginId, "Authorization state update: password requested\n");
        auto &pwInfo = static_cast<const td::td_api::authorizationStateWaitPassword &>(authState);
        requestPassword(pwInfo);
        break;
    }

    case td::td_api::authorizationStateReady::ID:
        purple_debug_misc(config::pluginId, "Authorization state update: ready\n");
        onLoggedIn();
        break;
    }
}

bool PurpleTdClient::addProxy()
{
    PurpleProxyInfo *purpleProxy = purple_proxy_get_setup(m_account);
    PurpleProxyType  proxyType   = purpleProxy ? purple_proxy_info_get_type(purpleProxy) : PURPLE_PROXY_NONE;
    const char *     username    = purpleProxy ? purple_proxy_info_get_username(purpleProxy) : "";
    const char *     password    = purpleProxy ? purple_proxy_info_get_password(purpleProxy) : "";
    const char *     host        = purpleProxy ? purple_proxy_info_get_host(purpleProxy) : "";
    int              port        = purpleProxy ? purple_proxy_info_get_port(purpleProxy) : 0;
    if (username == NULL) username = "";
    if (password == NULL) password = "";
    if (host == NULL) host = "";
    std::string errorMessage;

    td::td_api::object_ptr<td::td_api::ProxyType> tdProxyType;
    switch (proxyType) {
    case PURPLE_PROXY_NONE:
        tdProxyType = nullptr;
        break;
    case PURPLE_PROXY_SOCKS5:
        tdProxyType = td::td_api::make_object<td::td_api::proxyTypeSocks5>(username, password);
        break;
    case PURPLE_PROXY_HTTP:
        tdProxyType = td::td_api::make_object<td::td_api::proxyTypeHttp>(username, password, true);
        break;
    default:
        // TRANSLATOR: Buddy-window error message, argument will be some kind of proxy-identifier.
        errorMessage = formatMessage(_("Proxy type {} is not supported"), proxyTypeToString(proxyType));
        break;
    }

    if (!errorMessage.empty()) {
        purple_connection_error(purple_account_get_connection(m_account), errorMessage.c_str());
        return false;
    } else if (tdProxyType) {
        auto addProxy = td::td_api::make_object<td::td_api::addProxy>();
        addProxy->proxy_ = td::td_api::make_object<td::td_api::proxy>(host, port, std::move(tdProxyType));
        addProxy->enable_ = true;
        m_transceiver.sendQuery(std::move(addProxy), &PurpleTdClient::addProxyResponse);
        m_isProxyAdded = true;
    }

    return true;
}

void PurpleTdClient::addProxyResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && (object->get_id() == td::td_api::addedProxy::ID)) {
        m_addedProxy = td::move_tl_object_as<td::td_api::addedProxy>(object);
        if (m_proxies)
            removeOldProxies();
    } else {
        // TRANSLATOR: Buddy-window error message
        std::string message = formatMessage(_("Could not set proxy: {}"), getDisplayedError(object));
        purple_connection_error(purple_account_get_connection(m_account), message.c_str());
    }
}

void PurpleTdClient::getProxiesResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && (object->get_id() == td::td_api::addedProxies::ID)) {
        m_proxies = td::move_tl_object_as<td::td_api::addedProxies>(object);
        if (!m_isProxyAdded || m_addedProxy)
            removeOldProxies();
    } else {
        // TRANSLATOR: Buddy-window error message
        std::string message = formatMessage(_("Could not get proxies: {}"), getDisplayedError(object));
        purple_connection_error(purple_account_get_connection(m_account), message.c_str());
    }
}

void PurpleTdClient::removeOldProxies()
{
    for (const td::td_api::object_ptr<td::td_api::addedProxy> &proxy: m_proxies->proxies_)
        if (proxy && (!m_addedProxy || (proxy->id_ != m_addedProxy->id_)))
            m_transceiver.sendQuery(td::td_api::make_object<td::td_api::removeProxy>(proxy->id_), nullptr);
}

std::string PurpleTdClient::getBaseDatabasePath()
{
    return std::string(purple_user_dir()) + G_DIR_SEPARATOR_S + config::configSubdir;
}

static void stuff(td::td_api::setTdlibParameters &parameters)
{
    std::string s(config::stuff);
    for (size_t i = 0; i < s.length(); i++)
        s[i] -= 16;
    size_t i = s.find('i');
    if (i == std::string::npos)
        return;
    s[i] = ' ';
    sscanf(s.c_str(), "%" G_GINT32_FORMAT, &parameters.api_id_);
    parameters.api_hash_ = s.c_str()+i+1;
}

void PurpleTdClient::sendTdlibParameters()
{
    auto parameters = td::td_api::make_object<td::td_api::setTdlibParameters>();
    const char *username = purple_account_get_username(m_account);
    const char *api_id = purple_account_get_string(m_account, AccountOptions::ApiId, "");
    const char *api_hash = purple_account_get_string(m_account, AccountOptions::ApiHash, "");

    parameters->database_directory_ = getBaseDatabasePath() + G_DIR_SEPARATOR_S + username;
    purple_debug_misc(config::pluginId, "Account %s using database directory %s\n",
                      username, parameters->database_directory_.c_str());
    parameters->use_chat_info_database_ = true;
    parameters->use_message_database_ = true;
    parameters->use_secret_chats_ = (purple_account_get_bool(m_account, AccountOptions::EnableSecretChats,
                                                             AccountOptions::EnableSecretChatsDefault) != FALSE);
    parameters->api_id_ = atoi((api_id == nullptr || strlen(api_id) == 0) ? config::api_id : api_id);
    parameters->api_hash_ = (api_hash == nullptr || strlen(api_hash) == 0) ? config::api_hash : api_hash;
    if (*config::stuff)
        stuff(*parameters);
    parameters->system_language_code_ = "en";
    parameters->device_model_ = "Desktop";
    parameters->system_version_ = "Unknown";
    parameters->application_version_ = "1.0";

    m_transceiver.sendQuery(std::move(parameters),
                            &PurpleTdClient::authResponse);
}

void PurpleTdClient::sendPhoneNumber()
{
    const char *number = purple_account_get_username(m_account);
    m_transceiver.sendQuery(td::td_api::make_object<td::td_api::setAuthenticationPhoneNumber>(number, nullptr),
                            &PurpleTdClient::authResponse);
}

static std::string getAuthCodeDesc(const td::td_api::AuthenticationCodeType &codeType)
{
    switch (codeType.get_id()) {
    case td::td_api::authenticationCodeTypeTelegramMessage::ID:
        // TRANSLATOR: Authentication dialog, secondary content. Appears after a colon (':'). Argument is a number.
        return formatMessage(_("Telegram message (length: {})"),
                             static_cast<const td::td_api::authenticationCodeTypeTelegramMessage &>(codeType).length_);
    case td::td_api::authenticationCodeTypeSms::ID:
        // TRANSLATOR: Authentication dialog, secondary content. Appears after a colon (':'). Argument is a number.
        return formatMessage(_("SMS (length: {})"),
                             static_cast<const td::td_api::authenticationCodeTypeSms &>(codeType).length_);
    case td::td_api::authenticationCodeTypeCall::ID:
        // TRANSLATOR: Authentication dialog, secondary content. Appears after a colon (':'). Argument is a number.
        return formatMessage(_("Phone call (length: {})"),
                             static_cast<const td::td_api::authenticationCodeTypeCall &>(codeType).length_);
    case td::td_api::authenticationCodeTypeFlashCall::ID:
        // TRANSLATOR: Authentication dialog, secondary content. Official name "flash call". Appears after a colon (':'). Argument is some text-string-ish.
        return formatMessage(_("Poor man's phone call (pattern: {})"),
                             static_cast<const td::td_api::authenticationCodeTypeFlashCall &>(codeType).pattern_);
    default:
        // Shouldn't happen, so don't translate.
        return "Pigeon post";
    }
}

void PurpleTdClient::requestAuthCode(const td::td_api::authenticationCodeInfo *codeInfo)
{
    // TRANSLATOR: Authentication dialog, primary content. Will be followed by instructions and an input box.
    std::string message = _("Enter authentication code") + std::string("\n");

    if (codeInfo) {
        if (codeInfo->type_) {
            // TRANSLATOR: Authentication dialog, secondary content. Argument will be a term.
            message += formatMessage(_("Code sent via: {}"), getAuthCodeDesc(*codeInfo->type_)) + "\n";
        }
        if (codeInfo->next_type_) {
            // TRANSLATOR: Authentication dialog, secondary content. Argument will be a term.
            message += formatMessage(_("Next code will be: {}"), getAuthCodeDesc(*codeInfo->next_type_)) + "\n";
        }
    }

    purple_request_input (purple_account_get_connection(m_account),
                               // TRANSLATOR: Authentication dialog, title.
                               _("Login code"),
                               message.c_str(),
                               NULL, // secondary message
                               NULL, // default value
                               FALSE, // multiline input
                               FALSE, // masked input
                               NULL,
                               // TRANSLATOR: Authentication dialog, alternative is "_Cancel". The underscore marks accelerator keys, they must be different!
                               _("_OK"), G_CALLBACK(requestCodeEntered),
                               // TRANSLATOR: Authentication dialog, alternative is "_OK". The underscore marks accelerator keys, they must be different!
                               _("_Cancel"), G_CALLBACK(requestCodeCancelled),
                               m_account,
                               NULL, // buddy
                               NULL, // conversation
                               this);
}

void PurpleTdClient::requestAuthEmail()
{
    std::string message = _("Enter authentication email") + std::string("\n");

    purple_request_input (purple_account_get_connection(m_account),
                               // TRANSLATOR: Authentication dialog, title.
                               _("Authentication email"),
                               message.c_str(),
                               NULL, // secondary message
                               NULL, // default value
                               FALSE, // multiline input
                               FALSE, // masked input
                               NULL,
                               // TRANSLATOR: Authentication dialog, alternative is "_Cancel". The underscore marks accelerator keys, they must be different!
                               _("_OK"), G_CALLBACK(requestAuthEmailEntered),
                               // TRANSLATOR: Authentication dialog, alternative is "_OK". The underscore marks accelerator keys, they must be different!
                               _("_Cancel"), G_CALLBACK(requestAuthEmailCancelled),
                               m_account,
                               NULL, // buddy
                               NULL, // conversation
                               this);
}

void PurpleTdClient::requestAuthEmailEntered(PurpleTdClient *self, const gchar *email)
{
    purple_debug_misc(config::pluginId, "Authentication email entered: '%s'\n", email);
    auto authEmail = td::td_api::make_object<td::td_api::setAuthenticationEmailAddress>(email);

    self->m_transceiver.sendQuery(std::move(authEmail), &PurpleTdClient::authResponse);
}

void PurpleTdClient::requestAuthEmailCancelled(PurpleTdClient *self)
{
    purple_connection_error(purple_account_get_connection(self->m_account),
                            // TRANSLATOR: Connection failure, error message (title; empty content)
                            _("Authentication email required"));
}

void PurpleTdClient::requestAuthEmailCode()
{
    std::string message = _("Enter code sent to authentication email") + std::string("\n");

    purple_request_input (purple_account_get_connection(m_account),
                               // TRANSLATOR: Authentication dialog, title.
                               _("Code from authentication email"),
                               message.c_str(),
                               NULL, // secondary message
                               NULL, // default value
                               FALSE, // multiline input
                               FALSE, // masked input
                               NULL,
                               // TRANSLATOR: Authentication dialog, alternative is "_Cancel". The underscore marks accelerator keys, they must be different!
                               _("_OK"), G_CALLBACK(requestAuthEmailCodeEntered),
                               // TRANSLATOR: Authentication dialog, alternative is "_OK". The underscore marks accelerator keys, they must be different!
                               _("_Cancel"), G_CALLBACK(requestAuthEmailCodeCancelled),
                               m_account,
                               NULL, // buddy
                               NULL, // conversation
                               this);
}

void PurpleTdClient::requestAuthEmailCodeEntered(PurpleTdClient *self, const gchar *code)
{
    purple_debug_misc(config::pluginId, "Authentication email code entered: '%s'\n", code);
    auto authEmailCode = td::td_api::make_object<td::td_api::checkAuthenticationEmailCode>(
                                               td::td_api::make_object<td::td_api::emailAddressAuthenticationCode>(code));

    self->m_transceiver.sendQuery(std::move(authEmailCode), &PurpleTdClient::authResponse);
}

void PurpleTdClient::requestAuthEmailCodeCancelled(PurpleTdClient *self)
{
    purple_connection_error(purple_account_get_connection(self->m_account),
                            // TRANSLATOR: Connection failure, error message (title; empty content)
                            _("Authentication email required"));
}

void PurpleTdClient::requestCodeEntered(PurpleTdClient *self, const gchar *code)
{
    purple_debug_misc(config::pluginId, "Authentication code entered: '%s'\n", code);
    auto checkCode = td::td_api::make_object<td::td_api::checkAuthenticationCode>();
    if (code)
        checkCode->code_ = code;
    self->m_transceiver.sendQuery(std::move(checkCode), &PurpleTdClient::authResponse);
}

void PurpleTdClient::requestCodeCancelled(PurpleTdClient *self)
{
    purple_connection_error(purple_account_get_connection(self->m_account),
                            // TRANSLATOR: Connection failure, error message (title; empty content)
                            _("Authentication code required"));
}

void PurpleTdClient::passwordEntered(PurpleTdClient *self, const gchar *password)
{
    purple_debug_misc(config::pluginId, "Password code entered\n");
    auto checkPassword = td::td_api::make_object<td::td_api::checkAuthenticationPassword>();
    if (password)
        checkPassword->password_ = password;
    self->m_transceiver.sendQuery(std::move(checkPassword), &PurpleTdClient::authResponse);
}

void PurpleTdClient::passwordCancelled(PurpleTdClient *self)
{
    // TRANSLATOR: Connection failure, error message title (title; empty content)
    purple_connection_error(purple_account_get_connection(self->m_account), _("Password required"));
}

void PurpleTdClient::requestPassword(const td::td_api::authorizationStateWaitPassword &pwInfo)
{
    std::string hints;
    if (!pwInfo.password_hint_.empty()) {
        // TRANSLATOR: 2FA dialog, secondary content, appears in new line. Argument is an arbitrary string from Telegram.
        hints = formatMessage(_("Hint: {}"), pwInfo.password_hint_);
    }
    if (!pwInfo.recovery_email_address_pattern_.empty()) {
        if (!hints.empty())
            hints += '\n';
        // TRANSLATOR: 2FA dialog, secondary content, appears in new line. Argument is an e-mail address.
        hints += formatMessage(_("Recovery e-mail may have been sent to {}"), pwInfo.recovery_email_address_pattern_);
    }
    if (!purple_request_input (purple_account_get_connection(m_account),
                               // TRANSLATOR: 2FA dialog, title
                               _("Password"),
                               // TRANSLATOR: 2FA dialog, primary content
                               _("Enter password for two-factor authentication"),
                               hints.empty() ? NULL : hints.c_str(),
                               NULL, // default value
                               FALSE, // multiline input
                               FALSE, // masked input
                               NULL,
                               // TRANSLATOR: 2FA dialog, alternative is "_Cancel". The underscore marks accelerator keys, they must be different!
                               _("_OK"), G_CALLBACK(passwordEntered),
                               // TRANSLATOR: 2FA dialog, alternative is "_OK". The underscore marks accelerator keys, they must be different!
                               _("_Cancel"), G_CALLBACK(passwordCancelled),
                               m_account,
                               NULL, // buddy
                               NULL, // conversation
                               this))
    {
        // Only happens with like empathy, not worth translating
        purple_connection_error(purple_account_get_connection(m_account),
            "Authentication code is required but this libpurple doesn't support input requests");
    }
}

void PurpleTdClient::registerUser()
{
    std::string firstName, lastName;
    getNamesFromAlias(purple_account_get_alias(m_account), firstName, lastName);

    if (firstName.empty() && lastName.empty()) {
        if (!purple_request_input (purple_account_get_connection(m_account),
                                // TRANSLATOR: Registration dialog, title
                                _("Registration"),
                                // TRANSLATOR: Registration dialog, content
                                _("New account is being created. Please enter your display name."),
                                NULL,
                                NULL, // default value
                                FALSE, // multiline input
                                FALSE, // masked input
                                NULL,
                                // TRANSLATOR: Registration dialog, alternative is "_Cancel". The underscore marks accelerator keys, they must be different!
                                _("_OK"), G_CALLBACK(displayNameEntered),
                                // TRANSLATOR: Registration dialog, alternative is "_OK". The underscore marks accelerator keys, they must be different!
                                _("_Cancel"), G_CALLBACK(displayNameCancelled),
                                m_account,
                                NULL, // buddy
                                NULL, // conversation
                                this))
        {
            // Same as when requesting authentication code - not worth translating
            purple_connection_error(purple_account_get_connection(m_account),
                "Registration is required but this libpurple doesn't support input requests");
        }
    } else
        m_transceiver.sendQuery(td::td_api::make_object<td::td_api::registerUser>(firstName, lastName, false),
                                &PurpleTdClient::authResponse);
}

void PurpleTdClient::displayNameEntered(PurpleTdClient *self, const gchar *name)
{
    std::string firstName, lastName;
    getNamesFromAlias(name, firstName, lastName);
    if (firstName.empty() && lastName.empty())
        purple_connection_error(purple_account_get_connection(self->m_account),
                                // TRANSLATOR: Connection error message after failed registration.
                                _("Display name is required for registration"));
    else
        self->m_transceiver.sendQuery(td::td_api::make_object<td::td_api::registerUser>(firstName, lastName, false),
                                      &PurpleTdClient::authResponse);
}

void PurpleTdClient::displayNameCancelled(PurpleTdClient *self)
{
    purple_connection_error(purple_account_get_connection(self->m_account),
                            // TRANSLATOR: Connection error message after failed registration.
                            _("Display name is required for registration"));
}

void PurpleTdClient::authResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && (object->get_id() == td::td_api::ok::ID))
        purple_debug_misc(config::pluginId, "Authentication success on query %lu\n", (unsigned long)requestId);
    else
        notifyAuthError(object);
}

void PurpleTdClient::notifyAuthError(const td::td_api::object_ptr<td::td_api::Object> &response)
{
    std::string message;

    message = _("Authentication error: {}");
    message = formatMessage(message.c_str(), getDisplayedError(response));

    purple_connection_error(purple_account_get_connection(m_account), message.c_str());
}
