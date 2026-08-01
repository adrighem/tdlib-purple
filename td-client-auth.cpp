#include "td-client.h"
#include "module-activity.h"
#include "purple-info.h"
#include "config.h"
#include "format.h"

#include <utility>

namespace {

struct ReconnectContext {
    std::string accountName;
    std::string protocolId;
};

static gboolean restartOnboardingIdle(gpointer data)
{
    ReconnectContext *context = static_cast<ReconnectContext *>(data);

    PurpleAccount *account = purple_accounts_find(
        context->accountName.c_str(), context->protocolId.c_str());
    if (account) {
        purple_account_remove_setting(account, AccountOptions::ApiId);
        purple_account_remove_setting(account, AccountOptions::ApiHash);

        purple_account_disconnect(account);
        purple_account_connect(account);
    }

    return FALSE;
}

} // namespace

void PurpleTdClient::processAuthorizationState(
    td::td_api::AuthorizationState &authState)
{
    if (!m_authController)
        return;

    if (authState.get_id() ==
            td::td_api::authorizationStateWaitTdlibParameters::ID &&
        !m_authParameterSetupStarted) {
        m_authParameterSetupStarted = true;
        purple_debug_misc(
            config::pluginId,
            "Authorization state update: TDLib parameters requested\n");
        m_transceiver.sendQuery(
            td::td_api::make_object<td::td_api::disableProxy>(), nullptr);
        if (!addProxy()) {
            m_authController->shutdown();
            return;
        }
        m_transceiver.sendQuery(
            td::td_api::make_object<td::td_api::getProxies>(),
            &PurpleTdClient::getProxiesResponse);
    }

    m_authController->onAuthorizationState(&authState);
}

bool PurpleTdClient::addProxy()
{
    PurpleProxyInfo *purpleProxy = purple_proxy_get_setup(m_account);
    PurpleProxyType proxyType = purpleProxy
        ? purple_proxy_info_get_type(purpleProxy)
        : PURPLE_PROXY_NONE;
    const char *username = purpleProxy
        ? purple_proxy_info_get_username(purpleProxy)
        : "";
    const char *password = purpleProxy
        ? purple_proxy_info_get_password(purpleProxy)
        : "";
    const char *host = purpleProxy
        ? purple_proxy_info_get_host(purpleProxy)
        : "";
    int port = purpleProxy ? purple_proxy_info_get_port(purpleProxy) : 0;
    if (!username)
        username = "";
    if (!password)
        password = "";
    if (!host)
        host = "";
    std::string errorMessage;

    td::td_api::object_ptr<td::td_api::ProxyType> tdProxyType;
    switch (proxyType) {
    case PURPLE_PROXY_NONE:
        tdProxyType = nullptr;
        break;
    case PURPLE_PROXY_SOCKS5:
        tdProxyType =
            td::td_api::make_object<td::td_api::proxyTypeSocks5>(
                username, password);
        break;
    case PURPLE_PROXY_HTTP:
        tdProxyType =
            td::td_api::make_object<td::td_api::proxyTypeHttp>(
                username, password, true);
        break;
    default:
        errorMessage = formatMessage(
            // TRANSLATOR: Buddy-window error message, argument will be some
            // kind of proxy identifier.
            _("Proxy type {} is not supported"),
            proxyTypeToString(proxyType));
        break;
    }

    if (!errorMessage.empty()) {
        purple_connection_error(
            purple_account_get_connection(m_account),
            errorMessage.c_str());
        return false;
    }
    if (tdProxyType) {
        auto addProxyRequest =
            td::td_api::make_object<td::td_api::addProxy>();
        addProxyRequest->proxy_ =
            td::td_api::make_object<td::td_api::proxy>(
                host, port, std::move(tdProxyType));
        addProxyRequest->enable_ = true;
        m_transceiver.sendQuery(
            std::move(addProxyRequest),
            &PurpleTdClient::addProxyResponse);
        m_isProxyAdded = true;
    }

    return true;
}

void PurpleTdClient::addProxyResponse(
    uint64_t,
    td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && object->get_id() == td::td_api::addedProxy::ID) {
        m_addedProxy =
            td::move_tl_object_as<td::td_api::addedProxy>(object);
        if (m_proxies)
            removeOldProxies();
    } else {
        std::string message = formatMessage(
            // TRANSLATOR: Buddy-window error message
            _("Could not set proxy: {}"), getDisplayedError(object));
        purple_connection_error(
            purple_account_get_connection(m_account), message.c_str());
    }
}

void PurpleTdClient::getProxiesResponse(
    uint64_t,
    td::td_api::object_ptr<td::td_api::Object> object)
{
    if (object && object->get_id() == td::td_api::addedProxies::ID) {
        m_proxies =
            td::move_tl_object_as<td::td_api::addedProxies>(object);
        if (!m_isProxyAdded || m_addedProxy)
            removeOldProxies();
    } else {
        std::string message = formatMessage(
            // TRANSLATOR: Buddy-window error message
            _("Could not get proxies: {}"), getDisplayedError(object));
        purple_connection_error(
            purple_account_get_connection(m_account), message.c_str());
    }
}

void PurpleTdClient::removeOldProxies()
{
    for (const td::td_api::object_ptr<td::td_api::addedProxy> &proxy:
         m_proxies->proxies_) {
        if (proxy && (!m_addedProxy || proxy->id_ != m_addedProxy->id_)) {
            m_transceiver.sendQuery(
                td::td_api::make_object<td::td_api::removeProxy>(
                    proxy->id_),
                nullptr);
        }
    }
}

std::string PurpleTdClient::getBaseDatabasePath()
{
    return getPurple2BaseDatabasePath();
}

static std::string getAuthCodeDesc(const TdAuthCodeDelivery &delivery)
{
    switch (delivery.type) {
    case TdAuthCodeType::TelegramMessage:
        return formatMessage(
            // TRANSLATOR: Authentication dialog, secondary content. Appears
            // after a colon. Argument is a number.
            _("Telegram message (length: {})"), delivery.length);
    case TdAuthCodeType::Sms:
        // TRANSLATOR: Authentication dialog, secondary content. Appears after
        // a colon. Argument is a number.
        return formatMessage(_("SMS (length: {})"), delivery.length);
    case TdAuthCodeType::SmsWord:
        return _("Word sent by SMS");
    case TdAuthCodeType::SmsPhrase:
        return _("Phrase sent by SMS");
    case TdAuthCodeType::PhoneCall:
        return formatMessage(
            // TRANSLATOR: Authentication dialog, secondary content. Appears
            // after a colon. Argument is a number.
            _("Phone call (length: {})"), delivery.length);
    case TdAuthCodeType::FlashCall:
        return formatMessage(
            // TRANSLATOR: Authentication dialog, secondary content. Official
            // name is flash call. Argument is a phone-number pattern.
            _("Flash call (pattern: {})"), delivery.pattern);
    case TdAuthCodeType::MissedCall:
        return formatMessage(
            _("Missed call (length: {})"), delivery.length);
    case TdAuthCodeType::Fragment:
        return formatMessage(
            _("Fragment (length: {})"), delivery.length);
    case TdAuthCodeType::FirebaseAndroid:
    case TdAuthCodeType::FirebaseIos:
        return formatMessage(
            _("Device verification (length: {})"), delivery.length);
    case TdAuthCodeType::None:
    case TdAuthCodeType::Unknown:
        return _("Unknown delivery method");
    }
    return _("Unknown delivery method");
}

void PurpleTdClient::requestAuthCode(
    TdAuthPromptId prompt,
    const TdAuthCodeChallenge &challenge)
{
    // TRANSLATOR: Authentication dialog, primary content. Will be followed by
    // instructions and an input box.
    std::string message = _("Enter authentication code");
    message += '\n';
    if (challenge.code.type != TdAuthCodeType::None) {
        message += formatMessage(
            // TRANSLATOR: Authentication dialog, secondary content. Argument
            // is a delivery method.
            _("Code sent via: {}"), getAuthCodeDesc(challenge.code));
        message += '\n';
    }
    if (challenge.nextCode.type != TdAuthCodeType::None) {
        message += formatMessage(
            // TRANSLATOR: Authentication dialog, secondary content. Argument
            // is a delivery method.
            _("Next code will be: {}"),
            getAuthCodeDesc(challenge.nextCode));
        message += '\n';
    }

    requestAuthInput(
        prompt,
        TdAuthPromptType::Code,
        _("Login code"),
        message.c_str(),
        nullptr,
        false);
}

void PurpleTdClient::requestAuthEmail(TdAuthPromptId prompt)
{
    requestAuthInput(
        prompt,
        TdAuthPromptType::EmailAddress,
        _("Authentication email"),
        _("Enter authentication email"),
        nullptr,
        false);
}

void PurpleTdClient::requestAuthEmailCode(
    TdAuthPromptId prompt,
    const TdAuthEmailCodeChallenge &challenge)
{
    std::string details;
    if (!challenge.emailAddressPattern.empty()) {
        details = formatMessage(
            // TRANSLATOR: Authentication dialog, secondary content. Argument
            // is a masked email-address pattern supplied by Telegram.
            _("Code sent to: {}"), challenge.emailAddressPattern);
    }
    if (challenge.length > 0) {
        if (!details.empty())
            details += '\n';
        details += formatMessage(
            _("Code length: {}"), challenge.length);
    }

    requestAuthInput(
        prompt,
        TdAuthPromptType::EmailCode,
        _("Code from authentication email"),
        _("Enter code sent to authentication email"),
        details.empty() ? nullptr : details.c_str(),
        false);
}

void PurpleTdClient::requestPassword(
    TdAuthPromptId prompt,
    const TdAuthPasswordChallenge &challenge)
{
    std::string hints;
    if (!challenge.hint.empty()) {
        // TRANSLATOR: 2FA dialog, secondary content. Argument is arbitrary
        // text supplied by Telegram.
        hints = formatMessage(_("Hint: {}"), challenge.hint);
    }
    if (!challenge.recoveryEmailAddressPattern.empty()) {
        if (!hints.empty())
            hints += '\n';
        hints += formatMessage(
            // TRANSLATOR: 2FA dialog, secondary content. Argument is a masked
            // email-address pattern supplied by Telegram.
            _("Recovery e-mail may have been sent to {}"),
            challenge.recoveryEmailAddressPattern);
    }

    requestAuthInput(
        prompt,
        TdAuthPromptType::Password,
        _("Password"),
        _("Enter password for two-factor authentication"),
        hints.empty() ? nullptr : hints.c_str(),
        true);
}

void PurpleTdClient::registerUser(TdAuthPromptId prompt)
{
    std::string firstName;
    std::string lastName;
    getNamesFromAlias(
        purple_account_get_alias(m_account), firstName, lastName);

    if (!firstName.empty() && !m_registrationAliasRejected) {
        m_authController->submitRegistration(
            prompt, firstName, lastName);
        return;
    }

    requestAuthInput(
        prompt,
        TdAuthPromptType::Registration,
        _("Registration"),
        _("New account is being created. Please enter your display name."),
        nullptr,
        false);
}

void PurpleTdClient::requestAuthInput(
    TdAuthPromptId prompt,
    TdAuthPromptType type,
    const char *title,
    const char *primary,
    const char *secondary,
    bool masked)
{
    PurpleConnection *connection =
        purple_account_get_connection(m_account);
    AuthPromptContext *context = createAuthPromptContext(prompt, type);
    m_activeAuthPrompt = prompt;
    void *request = purple_request_input(
        connection,
        title,
        primary,
        secondary,
        nullptr,
        FALSE,
        masked ? TRUE : FALSE,
        nullptr,
        // TRANSLATOR: Authentication dialog button. The underscore marks an
        // accelerator and must differ from the Cancel button.
        _("_OK"),
        G_CALLBACK(authPromptEntered),
        // TRANSLATOR: Authentication dialog button. The underscore marks an
        // accelerator and must differ from the OK button.
        _("_Cancel"),
        G_CALLBACK(authPromptCancelled),
        m_account,
        nullptr,
        nullptr,
        context);
    if (!request) {
        if (m_activeAuthPrompt == prompt)
            m_activeAuthPrompt = TdAuthPromptId();
        m_authController->failPrompt(
            prompt, TdAuthPresentationFailure::Unsupported);
    }
}

PurpleTdClient::AuthPromptContext *
PurpleTdClient::createAuthPromptContext(
    TdAuthPromptId prompt,
    TdAuthPromptType type)
{
    std::unique_ptr<AuthPromptContext> context(new AuthPromptContext{
        this,
        m_lifetime,
        prompt,
        type,
    });
    AuthPromptContext *result = context.get();
    m_authPromptContexts.push_back(std::move(context));
    return result;
}

void PurpleTdClient::closeAuthPrompt(TdAuthPromptId prompt)
{
    if (!m_activeAuthPrompt.valid())
        return;
    if (prompt.valid() && prompt != m_activeAuthPrompt)
        return;
    m_activeAuthPrompt = TdAuthPromptId();
    PurpleConnection *connection =
        purple_account_get_connection(m_account);
    if (connection)
        purple_request_close_with_handle(connection);
}

void PurpleTdClient::authPromptEntered(
    AuthPromptContext *prompt,
    const gchar *value)
{
    if (!prompt)
        return;
    std::shared_ptr<LifetimeState> lifetime = prompt->lifetime.lock();
    if (!lifetime || !lifetime->alive || !prompt->client)
        return;

    PurpleTdClient *self = prompt->client;
    if (!self->m_authController)
        return;
    if (self->m_activeAuthPrompt != prompt->prompt)
        return;
    self->m_activeAuthPrompt = TdAuthPromptId();
    const std::string input = value ? value : "";
    TdAuthSubmissionResult result = TdAuthSubmissionResult::Stopped;
    switch (prompt->type) {
    case TdAuthPromptType::EmailAddress:
        result = self->m_authController->submitEmailAddress(
            prompt->prompt, input);
        break;
    case TdAuthPromptType::EmailCode:
        result = self->m_authController->submitEmailCode(
            prompt->prompt, input);
        break;
    case TdAuthPromptType::Code:
        result = self->m_authController->submitCode(
            prompt->prompt, input);
        break;
    case TdAuthPromptType::Password:
        result = self->m_authController->submitPassword(
            prompt->prompt, input);
        break;
    case TdAuthPromptType::Registration: {
        std::string firstName;
        std::string lastName;
        getNamesFromAlias(input.c_str(), firstName, lastName);
        result = self->m_authController->submitRegistration(
            prompt->prompt, firstName, lastName);
        break;
    }
    default:
        return;
    }
    (void)result;
}

void PurpleTdClient::authPromptCancelled(AuthPromptContext *prompt)
{
    if (!prompt)
        return;
    std::shared_ptr<LifetimeState> lifetime = prompt->lifetime.lock();
    if (!lifetime || !lifetime->alive || !prompt->client)
        return;
    PurpleTdClient *self = prompt->client;
    if (!self->m_authController)
        return;
    if (self->m_activeAuthPrompt != prompt->prompt)
        return;
    self->m_activeAuthPrompt = TdAuthPromptId();
    self->m_authController->cancelPrompt(prompt->prompt);
}

void PurpleTdClient::onPhoneNumberRequired(TdAuthPromptId prompt)
{
    m_lastAuthPromptType = TdAuthPromptType::PhoneNumber;
    purple_debug_misc(
        config::pluginId,
        "Authorization state update: phone number requested\n");
    const char *number = purple_account_get_username(m_account);
    m_authController->submitPhoneNumber(
        prompt, number ? number : "");
}

void PurpleTdClient::onPremiumPurchaseRequired(TdAuthPromptId prompt)
{
    m_lastAuthPromptType = TdAuthPromptType::PremiumPurchase;
    m_authController->failPrompt(
        prompt, TdAuthPresentationFailure::Unsupported);
}

void PurpleTdClient::onEmailAddressRequired(
    TdAuthPromptId prompt,
    const TdAuthEmailAddressChallenge &)
{
    m_lastAuthPromptType = TdAuthPromptType::EmailAddress;
    purple_debug_misc(
        config::pluginId, "Authorization email requested\n");
    requestAuthEmail(prompt);
}

void PurpleTdClient::onEmailCodeRequired(
    TdAuthPromptId prompt,
    const TdAuthEmailCodeChallenge &challenge)
{
    m_lastAuthPromptType = TdAuthPromptType::EmailCode;
    purple_debug_misc(
        config::pluginId,
        "Authorization email confirmation code requested\n");
    requestAuthEmailCode(prompt, challenge);
}

void PurpleTdClient::onCodeRequired(
    TdAuthPromptId prompt,
    const TdAuthCodeChallenge &challenge)
{
    m_lastAuthPromptType = TdAuthPromptType::Code;
    purple_debug_misc(
        config::pluginId,
        "Authorization state update: authentication code requested\n");
    requestAuthCode(prompt, challenge);
}

void PurpleTdClient::onQrLinkChanged(
    TdAuthPromptId prompt,
    const std::string &link)
{
    (void)link;
    m_lastAuthPromptType = TdAuthPromptType::QrCode;
    m_authController->failPrompt(
        prompt, TdAuthPresentationFailure::Unsupported);
}

void PurpleTdClient::onRegistrationRequired(
    TdAuthPromptId prompt,
    const TdAuthRegistrationChallenge &)
{
    m_lastAuthPromptType = TdAuthPromptType::Registration;
    purple_debug_misc(
        config::pluginId,
        "Authorization state update: new user registration\n");
    registerUser(prompt);
}

void PurpleTdClient::onPasswordRequired(
    TdAuthPromptId prompt,
    const TdAuthPasswordChallenge &challenge)
{
    m_lastAuthPromptType = TdAuthPromptType::Password;
    purple_debug_misc(
        config::pluginId,
        "Authorization state update: password requested\n");
    requestPassword(prompt, challenge);
}

void PurpleTdClient::onPromptClosed(
    TdAuthPromptId prompt,
    TdAuthPromptType,
    TdAuthPromptCloseReason)
{
    closeAuthPrompt(prompt);
}

void PurpleTdClient::onRequestFailed(
    const TdAuthRequestFailure &failure)
{
    if (failure.operation == TdAuthOperation::SetPhoneNumber) {
        // Purple 2 takes its fixed phone number from the account identity, so
        // redisplaying the same automatic value would only create a loop.
        m_authController->shutdown();
        purple_connection_error(
            purple_account_get_connection(m_account),
            failure.errorCode == 0
                ? _("A phone number is required for Purple 2 authentication")
                : _("Telegram rejected the configured phone number"));
        return;
    }

    if (failure.operation == TdAuthOperation::RegisterUser)
        m_registrationAliasRejected = true;

    const char *details = nullptr;
    if (failure.errorCode == 0) {
        details = (
            // TRANSLATOR: Authentication retry notification. No submitted
            // value is included.
            _("Required authentication input was empty. Please try again."));
    } else {
        details = (
            // TRANSLATOR: Authentication retry notification. No
            // server-provided text or submitted value is included.
            _("Telegram rejected the authentication response. Please try "
              "again."));
    }
    purple_notify_error(
        m_account,
        // TRANSLATOR: Authentication retry notification title.
        _("Authentication error"),
        details,
        nullptr);
}

void PurpleTdClient::onAuthorizationReady()
{
    purple_debug_misc(
        config::pluginId,
        "Authorization state update: ready\n");
    onLoggedIn();
}

void PurpleTdClient::onAuthorizationCancelled()
{
    const char *message = _("Authentication was cancelled");
    switch (m_lastAuthPromptType) {
    case TdAuthPromptType::EmailAddress:
        message = _("Authentication email required");
        break;
    case TdAuthPromptType::EmailCode:
        message = _("Authentication email code required");
        break;
    case TdAuthPromptType::Code:
        message = _("Authentication code required");
        break;
    case TdAuthPromptType::Password:
        message = _("Password required");
        break;
    case TdAuthPromptType::Registration:
        message = _("Display name is required for registration");
        break;
    case TdAuthPromptType::QrCode:
        message = _("QR authentication was cancelled");
        break;
    default:
        break;
    }
    purple_connection_error(
        purple_account_get_connection(m_account), message);
}

void PurpleTdClient::onAuthorizationFailed(const TdAuthFailure &failure)
{
    if (failure.type == TdAuthFailureType::RequestRejected &&
        failure.errorCode == 400 &&
        (failure.errorMessage.find("api_id") != std::string::npos ||
         failure.errorMessage.find("API_ID") != std::string::npos))
    {
        purple_debug_warning(
            config::pluginId,
            "API ID/Hash rejected, restarting onboarding flow\n");
        auto context = new ReconnectContext{
            purple_account_get_username(m_account),
            purple_account_get_protocol_id(m_account)
        };
        moduleActivityAddIdle(
            restartOnboardingIdle,
            context,
            [](gpointer data) { delete static_cast<ReconnectContext *>(data); });
        return;
    }

    const char *message = _("Telegram authentication failed");
    switch (failure.type) {
    case TdAuthFailureType::InvalidConfiguration:
        message = _(
            "Telegram application credentials are missing or invalid");
        break;
    case TdAuthFailureType::RequestRejected:
        message = _("Telegram rejected the authentication request");
        break;
    case TdAuthFailureType::TransportUnavailable:
        message = _("Telegram authentication transport stopped");
        break;
    case TdAuthFailureType::MalformedResponse:
        message = _("Telegram returned an invalid authentication response");
        break;
    case TdAuthFailureType::MalformedState:
        message = _("Telegram returned an invalid authentication state");
        break;
    case TdAuthFailureType::UnsupportedState:
        message = _("Telegram requested an unsupported authentication step");
        break;
    case TdAuthFailureType::PresentationUnavailable:
        if (m_lastAuthPromptType == TdAuthPromptType::QrCode) {
            message = _("QR authentication is not supported by Purple 2");
        } else if (
            m_lastAuthPromptType == TdAuthPromptType::PremiumPurchase) {
            message = _(
                "Telegram Premium authentication is not supported");
        } else {
            message = _(
                "This libpurple client cannot show the required authentication input");
        }
        break;
    case TdAuthFailureType::PresentationFailed:
        message = _("Required authentication input was empty");
        break;
    case TdAuthFailureType::TerminalState:
        message = _("Telegram authorization ended before login completed");
        break;
    case TdAuthFailureType::InternalError:
        message = _("Telegram authentication could not be completed");
        break;
    }
    purple_connection_error(
        purple_account_get_connection(m_account), message);
}

void PurpleTdClient::reportAuthorizationEnded()
{
    if (m_authLifecycleErrorReported)
        return;
    m_authLifecycleErrorReported = true;
    purple_connection_error(
        purple_account_get_connection(m_account),
        _("Telegram authorization ended"));
}

void PurpleTdClient::onLoggingOut()
{
    reportAuthorizationEnded();
}

void PurpleTdClient::onClosing()
{
    reportAuthorizationEnded();
}

void PurpleTdClient::onClosed()
{
    reportAuthorizationEnded();
}

void PurpleTdClient::notifyAuthError(
    const td::td_api::object_ptr<td::td_api::Object> &response)
{
    (void)response;
    // Server error text can echo submitted values. Keep this post-auth login
    // bootstrap failure stable and value-free too.
    purple_connection_error(
        purple_account_get_connection(m_account),
        _("Telegram login could not be completed"));
}
