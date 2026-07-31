#include "td-auth-controller.h"

#include <glib.h>

#include <algorithm>
#include <cctype>
#include <utility>
#include <vector>

namespace {

using namespace td::td_api;

bool isHexadecimal(char value)
{
    const unsigned char character =
        static_cast<unsigned char>(value);
    return (character >= '0' && character <= '9') ||
        (character >= 'a' && character <= 'f') ||
        (character >= 'A' && character <= 'F');
}

bool sameDelivery(
    const TdAuthCodeDelivery &left,
    const TdAuthCodeDelivery &right)
{
    return left.type == right.type &&
        left.length == right.length &&
        left.pattern == right.pattern;
}

TdAuthCodeDelivery copyCodeDelivery(
    const AuthenticationCodeType *type)
{
    TdAuthCodeDelivery result;
    if (!type)
        return result;

    switch (type->get_id()) {
    case authenticationCodeTypeTelegramMessage::ID:
        result.type = TdAuthCodeType::TelegramMessage;
        result.length =
            static_cast<const authenticationCodeTypeTelegramMessage *>(
                type)->length_;
        break;
    case authenticationCodeTypeSms::ID:
        result.type = TdAuthCodeType::Sms;
        result.length =
            static_cast<const authenticationCodeTypeSms *>(type)->length_;
        break;
    case authenticationCodeTypeSmsWord::ID:
        result.type = TdAuthCodeType::SmsWord;
        break;
    case authenticationCodeTypeSmsPhrase::ID:
        result.type = TdAuthCodeType::SmsPhrase;
        break;
    case authenticationCodeTypeCall::ID:
        result.type = TdAuthCodeType::PhoneCall;
        result.length =
            static_cast<const authenticationCodeTypeCall *>(type)->length_;
        break;
    case authenticationCodeTypeFlashCall::ID:
        result.type = TdAuthCodeType::FlashCall;
        result.pattern =
            static_cast<const authenticationCodeTypeFlashCall *>(
                type)->pattern_;
        break;
    case authenticationCodeTypeMissedCall::ID:
        result.type = TdAuthCodeType::MissedCall;
        result.length =
            static_cast<const authenticationCodeTypeMissedCall *>(
                type)->length_;
        break;
    case authenticationCodeTypeFragment::ID:
        result.type = TdAuthCodeType::Fragment;
        result.length =
            static_cast<const authenticationCodeTypeFragment *>(
                type)->length_;
        break;
    case authenticationCodeTypeFirebaseAndroid::ID:
        result.type = TdAuthCodeType::FirebaseAndroid;
        result.length =
            static_cast<const authenticationCodeTypeFirebaseAndroid *>(
                type)->length_;
        break;
    case authenticationCodeTypeFirebaseIos::ID:
        result.type = TdAuthCodeType::FirebaseIos;
        result.length =
            static_cast<const authenticationCodeTypeFirebaseIos *>(
                type)->length_;
        break;
    default:
        result.type = TdAuthCodeType::Unknown;
        break;
    }
    return result;
}

TdAuthEmailCodeChallenge copyEmailCodeChallenge(
    const authorizationStateWaitEmailCode &state)
{
    TdAuthEmailCodeChallenge result;
    result.allowAppleId = state.allow_apple_id_;
    result.allowGoogleId = state.allow_google_id_;
    if (state.code_info_) {
        result.emailAddressPattern =
            state.code_info_->email_address_pattern_;
        result.length = state.code_info_->length_;
    }
    if (!state.email_address_reset_state_)
        return result;

    switch (state.email_address_reset_state_->get_id()) {
    case emailAddressResetStateAvailable::ID:
        result.resetState = TdAuthEmailResetState::Available;
        result.resetSeconds =
            static_cast<const emailAddressResetStateAvailable *>(
                state.email_address_reset_state_.get())->wait_period_;
        break;
    case emailAddressResetStatePending::ID:
        result.resetState = TdAuthEmailResetState::Pending;
        result.resetSeconds =
            static_cast<const emailAddressResetStatePending *>(
                state.email_address_reset_state_.get())->reset_in_;
        break;
    default:
        result.resetState = TdAuthEmailResetState::Unknown;
        break;
    }
    return result;
}

struct ChallengeSnapshot {
    TdAuthPromptType type = TdAuthPromptType::None;
    TdAuthEmailAddressChallenge emailAddress;
    TdAuthEmailCodeChallenge emailCode;
    TdAuthCodeChallenge code;
    TdAuthRegistrationChallenge registration;
    TdAuthPasswordChallenge password;
};

bool sameChallenge(
    const ChallengeSnapshot &left,
    const ChallengeSnapshot &right)
{
    if (left.type != right.type)
        return false;

    switch (left.type) {
    case TdAuthPromptType::EmailAddress:
        return left.emailAddress.allowAppleId ==
                   right.emailAddress.allowAppleId &&
            left.emailAddress.allowGoogleId ==
                   right.emailAddress.allowGoogleId;
    case TdAuthPromptType::EmailCode:
        return left.emailCode.allowAppleId ==
                   right.emailCode.allowAppleId &&
            left.emailCode.allowGoogleId ==
                   right.emailCode.allowGoogleId &&
            left.emailCode.emailAddressPattern ==
                   right.emailCode.emailAddressPattern &&
            left.emailCode.length == right.emailCode.length &&
            left.emailCode.resetState == right.emailCode.resetState &&
            left.emailCode.resetSeconds == right.emailCode.resetSeconds;
    case TdAuthPromptType::Code:
        return sameDelivery(left.code.code, right.code.code) &&
            sameDelivery(left.code.nextCode, right.code.nextCode) &&
            left.code.timeout == right.code.timeout;
    case TdAuthPromptType::Registration:
        return left.registration.minimumAge ==
                   right.registration.minimumAge &&
            left.registration.showPopup ==
                   right.registration.showPopup;
    case TdAuthPromptType::Password:
        return left.password.hint == right.password.hint &&
            left.password.hasRecoveryEmailAddress ==
                   right.password.hasRecoveryEmailAddress &&
            left.password.hasPassportData ==
                   right.password.hasPassportData &&
            left.password.recoveryEmailAddressPattern ==
                   right.password.recoveryEmailAddressPattern;
    default:
        return true;
    }
}

bool fingerprintLink(
    const std::string &link,
    std::string &fingerprint)
{
    if (link.empty())
        return false;

    gchar *digest = g_compute_checksum_for_data(
        G_CHECKSUM_SHA256,
        reinterpret_cast<const guchar *>(link.data()),
        link.size());
    if (!digest)
        return false;
    fingerprint = digest;
    g_free(digest);
    return true;
}

} // namespace

TdAuthConfiguration::TdAuthConfiguration(
    std::int32_t apiId,
    std::string apiHash,
    std::string databaseDirectory,
    bool useSecretChats,
    TdAuthMode mode)
    : m_apiId(apiId),
      m_apiHash(std::move(apiHash)),
      m_databaseDirectory(std::move(databaseDirectory)),
      m_useSecretChats(useSecretChats),
      m_mode(mode)
{}

std::int32_t TdAuthConfiguration::apiId() const noexcept
{
    return m_apiId;
}

const std::string &TdAuthConfiguration::apiHash() const noexcept
{
    return m_apiHash;
}

const std::string &TdAuthConfiguration::databaseDirectory() const noexcept
{
    return m_databaseDirectory;
}

bool TdAuthConfiguration::useSecretChats() const noexcept
{
    return m_useSecretChats;
}

TdAuthMode TdAuthConfiguration::mode() const noexcept
{
    return m_mode;
}

bool TdAuthConfiguration::valid() const noexcept
{
    return m_apiId > 0 &&
        !m_databaseDirectory.empty() &&
        m_apiHash.size() == 32 &&
        std::all_of(
            m_apiHash.begin(), m_apiHash.end(), isHexadecimal);
}

TdAuthPromptId::TdAuthPromptId(std::uint64_t value) noexcept
    : m_value(value)
{}

std::uint64_t TdAuthPromptId::value() const noexcept
{
    return m_value;
}

bool TdAuthPromptId::valid() const noexcept
{
    return m_value != 0;
}

bool operator==(TdAuthPromptId left, TdAuthPromptId right) noexcept
{
    return left.value() == right.value();
}

bool operator!=(TdAuthPromptId left, TdAuthPromptId right) noexcept
{
    return !(left == right);
}

class TdAuthControllerState
    : public std::enable_shared_from_this<TdAuthControllerState> {
public:
    using ObjectPtr = TdAuthController::ObjectPtr;
    using FunctionPtr = TdAuthController::FunctionPtr;
    using SendCallback = TdAuthController::SendCallback;

    TdAuthControllerState(
        TdAuthConfiguration configuration,
        SendCallback sendCallback,
        TdAuthObserver &observer)
        : m_configuration(std::move(configuration)),
          m_sendCallback(std::move(sendCallback)),
          m_observer(&observer)
    {}

    void onAuthorizationState(
        const td::td_api::AuthorizationState *state)
    {
        if (m_stopped)
            return;
        if (!state) {
            failAuthorization(
                TdAuthFailureType::MalformedState,
                TdAuthState::None,
                TdAuthOperation::None,
                0,
                0,
                TdAuthPromptCloseReason::Failed);
            return;
        }

        const std::int32_t stateId = state->get_id();
        if (stateId == authorizationStateLoggingOut::ID) {
            handleLifecycle(TdAuthState::LoggingOut);
            return;
        }
        if (stateId == authorizationStateClosing::ID) {
            handleLifecycle(TdAuthState::Closing);
            return;
        }
        if (stateId == authorizationStateClosed::ID) {
            handleLifecycle(TdAuthState::Closed);
            return;
        }

        if (m_completion != Completion::None)
            return;

        switch (stateId) {
        case authorizationStateWaitTdlibParameters::ID:
            handleParameters(stateId);
            break;
        case authorizationStateWaitPhoneNumber::ID:
            handlePhoneNumber(stateId);
            break;
        case authorizationStateWaitPremiumPurchase::ID:
            handlePremiumPurchase(stateId);
            break;
        case authorizationStateWaitEmailAddress::ID:
            handleEmailAddress(
                stateId,
                static_cast<const authorizationStateWaitEmailAddress &>(
                    *state));
            break;
        case authorizationStateWaitEmailCode::ID:
            handleEmailCode(
                stateId,
                static_cast<const authorizationStateWaitEmailCode &>(
                    *state));
            break;
        case authorizationStateWaitCode::ID:
            handleCode(
                stateId,
                static_cast<const authorizationStateWaitCode &>(*state));
            break;
        case authorizationStateWaitOtherDeviceConfirmation::ID:
            handleQrLink(
                stateId,
                static_cast<
                    const authorizationStateWaitOtherDeviceConfirmation &>(
                    *state).link_);
            break;
        case authorizationStateWaitRegistration::ID:
            handleRegistration(
                stateId,
                static_cast<const authorizationStateWaitRegistration &>(
                    *state));
            break;
        case authorizationStateWaitPassword::ID:
            handlePassword(
                stateId,
                static_cast<const authorizationStateWaitPassword &>(
                    *state));
            break;
        case authorizationStateReady::ID:
            completeReady(stateId);
            break;
        default:
            failAuthorization(
                TdAuthFailureType::UnsupportedState,
                TdAuthState::Unknown,
                TdAuthOperation::None,
                stateId,
                0,
                TdAuthPromptCloseReason::Failed);
            break;
        }
    }

    TdAuthSubmissionResult submitPhoneNumber(
        TdAuthPromptId prompt,
        const std::string &phoneNumber)
    {
        if (phoneNumber.empty())
            return rejectInvalidInput(
                prompt,
                TdAuthPromptType::PhoneNumber,
                TdAuthOperation::SetPhoneNumber);
        return submit(
            prompt,
            TdAuthPromptType::PhoneNumber,
            TdAuthOperation::SetPhoneNumber,
            make_object<setAuthenticationPhoneNumber>(
                phoneNumber, nullptr),
            true);
    }

    TdAuthSubmissionResult submitEmailAddress(
        TdAuthPromptId prompt,
        const std::string &emailAddress)
    {
        if (emailAddress.empty())
            return rejectInvalidInput(
                prompt,
                TdAuthPromptType::EmailAddress,
                TdAuthOperation::SetEmailAddress);
        return submit(
            prompt,
            TdAuthPromptType::EmailAddress,
            TdAuthOperation::SetEmailAddress,
            make_object<setAuthenticationEmailAddress>(emailAddress),
            true);
    }

    TdAuthSubmissionResult submitEmailCode(
        TdAuthPromptId prompt,
        const std::string &code)
    {
        if (code.empty())
            return rejectInvalidInput(
                prompt,
                TdAuthPromptType::EmailCode,
                TdAuthOperation::CheckEmailCode);
        return submit(
            prompt,
            TdAuthPromptType::EmailCode,
            TdAuthOperation::CheckEmailCode,
            make_object<checkAuthenticationEmailCode>(
                make_object<emailAddressAuthenticationCode>(code)),
            true);
    }

    TdAuthSubmissionResult submitCode(
        TdAuthPromptId prompt,
        const std::string &code)
    {
        if (code.empty())
            return rejectInvalidInput(
                prompt,
                TdAuthPromptType::Code,
                TdAuthOperation::CheckCode);
        return submit(
            prompt,
            TdAuthPromptType::Code,
            TdAuthOperation::CheckCode,
            make_object<checkAuthenticationCode>(code),
            true);
    }

    TdAuthSubmissionResult submitPassword(
        TdAuthPromptId prompt,
        const std::string &password)
    {
        if (password.empty())
            return rejectInvalidInput(
                prompt,
                TdAuthPromptType::Password,
                TdAuthOperation::CheckPassword);
        return submit(
            prompt,
            TdAuthPromptType::Password,
            TdAuthOperation::CheckPassword,
            make_object<checkAuthenticationPassword>(password),
            true);
    }

    TdAuthSubmissionResult submitRegistration(
        TdAuthPromptId prompt,
        const std::string &firstName,
        const std::string &lastName)
    {
        if (firstName.empty())
            return rejectInvalidInput(
                prompt,
                TdAuthPromptType::Registration,
                TdAuthOperation::RegisterUser);
        return submit(
            prompt,
            TdAuthPromptType::Registration,
            TdAuthOperation::RegisterUser,
            make_object<registerUser>(firstName, lastName, false),
            true);
    }

    TdAuthSubmissionResult cancelPrompt(TdAuthPromptId prompt)
    {
        const TdAuthSubmissionResult validation =
            validatePrompt(prompt, TdAuthPromptType::None);
        if (validation != TdAuthSubmissionResult::Accepted)
            return validation;

        m_completion = Completion::Cancelled;
        ++m_stateEpoch;
        invalidateRequest();
        notifyCompletion(
            TdAuthPromptCloseReason::Cancelled,
            [](TdAuthObserver &observer) {
                observer.onAuthorizationCancelled();
            });
        return TdAuthSubmissionResult::Accepted;
    }

    TdAuthSubmissionResult failPrompt(
        TdAuthPromptId prompt,
        TdAuthPresentationFailure presentationFailure)
    {
        const TdAuthSubmissionResult validation =
            validatePrompt(prompt, TdAuthPromptType::None);
        if (validation != TdAuthSubmissionResult::Accepted)
            return validation;

        const TdAuthState failedState = m_state;
        m_completion = Completion::Failed;
        ++m_stateEpoch;
        invalidateRequest();

        TdAuthFailure failure;
        failure.type =
            presentationFailure == TdAuthPresentationFailure::Unsupported
                ? TdAuthFailureType::PresentationUnavailable
                : TdAuthFailureType::PresentationFailed;
        failure.state = failedState;
        notifyCompletion(
            TdAuthPromptCloseReason::PresentationFailed,
            [&failure](TdAuthObserver &observer) {
                observer.onAuthorizationFailed(failure);
            });
        return TdAuthSubmissionResult::Accepted;
    }

    bool cancel()
    {
        if (m_stopped || m_completion != Completion::None)
            return false;
        m_completion = Completion::Cancelled;
        ++m_stateEpoch;
        invalidateRequest();
        notifyCompletion(
            TdAuthPromptCloseReason::Cancelled,
            [](TdAuthObserver &observer) {
                observer.onAuthorizationCancelled();
            });
        return true;
    }

    void shutdown() noexcept
    {
        if (m_stopped)
            return;
        m_stopped = true;
        ++m_stateEpoch;
        invalidateRequest();
        notifyPromptClosed(TdAuthPromptCloseReason::Shutdown);
        m_observer = nullptr;
        m_sendCallback = SendCallback();
        m_qrFingerprint.clear();
        clearChallenge();
        m_pendingLifecycle.clear();
    }

    void abortUnexpectedException() noexcept
    {
        if (m_stopped)
            return;
        if (m_completion != Completion::None) {
            stopAfterObserverFailure();
            return;
        }
        try {
            failAuthorization(
                TdAuthFailureType::InternalError,
                m_state,
                m_pendingOperation,
                m_rawStateId,
                0,
                TdAuthPromptCloseReason::Failed);
        } catch (...) {
            stopAfterObserverFailure();
        }
    }

private:
    enum class Completion : std::uint8_t {
        None,
        Ready,
        Cancelled,
        Failed,
    };

    struct ClosedPrompt {
        TdAuthPromptId prompt;
        TdAuthPromptType type = TdAuthPromptType::None;
    };

    template <typename Callback>
    bool notifyObserver(Callback callback) noexcept
    {
        TdAuthObserver *observer = m_observer;
        if (!observer)
            return false;
        try {
            callback(*observer);
            return true;
        } catch (...) {
            completeObserverFailure();
            return false;
        }
    }

    void completeObserverFailure() noexcept
    {
        if (!m_stopped && m_completion == Completion::None) {
            try {
                failAuthorization(
                    TdAuthFailureType::InternalError,
                    m_state,
                    m_pendingOperation,
                    m_rawStateId,
                    0,
                    TdAuthPromptCloseReason::Failed);
                return;
            } catch (...) {
                // The guarded failure path below is the final containment
                // boundary if even completion reporting cannot run.
            }
        }
        stopAfterObserverFailure();
    }

    void stopAfterObserverFailure() noexcept
    {
        if (!m_stopped) {
            m_stopped = true;
            ++m_stateEpoch;
            invalidateRequest();
        }
        discardPrompt();
        m_observer = nullptr;
        m_sendCallback = SendCallback();
        m_qrFingerprint.clear();
        clearChallenge();
        m_pendingLifecycle.clear();
    }

    void handleParameters(std::int32_t rawStateId)
    {
        if (m_state == TdAuthState::WaitTdlibParameters)
            return;
        if (!m_configuration.valid()) {
            failAuthorization(
                TdAuthFailureType::InvalidConfiguration,
                TdAuthState::WaitTdlibParameters,
                TdAuthOperation::SetTdlibParameters,
                rawStateId,
                0,
                TdAuthPromptCloseReason::Failed);
            return;
        }

        const std::uint64_t stateEpoch = enterState(
            TdAuthState::WaitTdlibParameters,
            rawStateId,
            TdAuthPromptCloseReason::StateChanged);
        if (!isCurrentState(
                stateEpoch, TdAuthState::WaitTdlibParameters))
            return;

        object_ptr<setTdlibParameters> parameters =
            make_object<setTdlibParameters>();
        parameters->database_directory_ =
            m_configuration.databaseDirectory();
        parameters->use_chat_info_database_ = true;
        parameters->use_message_database_ = true;
        parameters->use_secret_chats_ =
            m_configuration.useSecretChats();
        parameters->api_id_ = m_configuration.apiId();
        parameters->api_hash_ = m_configuration.apiHash();
        parameters->system_language_code_ = "en";
        parameters->device_model_ = "Desktop";
        parameters->system_version_ = "Unknown";
        parameters->application_version_ = "1.0";

        startRequest(
            TdAuthPromptId(),
            TdAuthOperation::SetTdlibParameters,
            std::move(parameters),
            false,
            false);
    }

    void handlePhoneNumber(std::int32_t rawStateId)
    {
        if (m_configuration.mode() == TdAuthMode::PhoneNumber) {
            ChallengeSnapshot challenge;
            challenge.type = TdAuthPromptType::PhoneNumber;
            handleChallengeState(
                TdAuthState::WaitPhoneNumber,
                rawStateId,
                std::move(challenge));
            return;
        }

        if (m_state != TdAuthState::WaitPhoneNumber) {
            const std::uint64_t stateEpoch = enterState(
                TdAuthState::WaitPhoneNumber,
                rawStateId,
                TdAuthPromptCloseReason::StateChanged);
            if (!isCurrentState(
                    stateEpoch, TdAuthState::WaitPhoneNumber)) {
                return;
            }
            if (m_qrRequested) {
                failAuthorization(
                    TdAuthFailureType::UnsupportedState,
                    TdAuthState::WaitPhoneNumber,
                    TdAuthOperation::RequestQrCode,
                    rawStateId,
                    0,
                    TdAuthPromptCloseReason::Failed);
                return;
            }
        }
        if (m_completion != Completion::None || m_stopped || m_qrRequested)
            return;

        // One request is sent per controller generation. TDLib owns QR
        // refresh after this request; a later re-entry fails above rather
        // than silently hanging or starting a second QR flow.
        m_qrRequested = true;
        startRequest(
            TdAuthPromptId(),
            TdAuthOperation::RequestQrCode,
            make_object<requestQrCodeAuthentication>(
                std::vector<td::td_api::int53>()),
            false,
            false);
    }

    void handlePremiumPurchase(std::int32_t rawStateId)
    {
        ChallengeSnapshot challenge;
        challenge.type = TdAuthPromptType::PremiumPurchase;
        handleChallengeState(
            TdAuthState::WaitPremiumPurchase,
            rawStateId,
            std::move(challenge));
    }

    void handleEmailAddress(
        std::int32_t rawStateId,
        const authorizationStateWaitEmailAddress &state)
    {
        ChallengeSnapshot challenge;
        challenge.type = TdAuthPromptType::EmailAddress;
        challenge.emailAddress.allowAppleId = state.allow_apple_id_;
        challenge.emailAddress.allowGoogleId = state.allow_google_id_;
        handleChallengeState(
            TdAuthState::WaitEmailAddress,
            rawStateId,
            std::move(challenge));
    }

    void handleEmailCode(
        std::int32_t rawStateId,
        const authorizationStateWaitEmailCode &state)
    {
        ChallengeSnapshot challenge;
        challenge.type = TdAuthPromptType::EmailCode;
        challenge.emailCode = copyEmailCodeChallenge(state);
        handleChallengeState(
            TdAuthState::WaitEmailCode,
            rawStateId,
            std::move(challenge));
    }

    void handleCode(
        std::int32_t rawStateId,
        const authorizationStateWaitCode &state)
    {
        ChallengeSnapshot challenge;
        challenge.type = TdAuthPromptType::Code;
        if (state.code_info_) {
            challenge.code.code =
                copyCodeDelivery(state.code_info_->type_.get());
            challenge.code.nextCode =
                copyCodeDelivery(state.code_info_->next_type_.get());
            challenge.code.timeout = state.code_info_->timeout_;
        }
        handleChallengeState(
            TdAuthState::WaitCode,
            rawStateId,
            std::move(challenge));
    }

    void handleQrLink(
        std::int32_t rawStateId,
        const std::string &link)
    {
        std::string fingerprint;
        if (!fingerprintLink(link, fingerprint)) {
            failAuthorization(
                TdAuthFailureType::MalformedState,
                TdAuthState::WaitOtherDeviceConfirmation,
                TdAuthOperation::None,
                rawStateId,
                0,
                TdAuthPromptCloseReason::Failed);
            return;
        }

        if (m_state == TdAuthState::WaitOtherDeviceConfirmation &&
            m_openPromptType == TdAuthPromptType::QrCode &&
            m_qrFingerprint == fingerprint &&
            m_qrLinkLength == link.size()) {
            return;
        }

        if (m_state != TdAuthState::WaitOtherDeviceConfirmation ||
            m_openPromptType != TdAuthPromptType::QrCode) {
            const std::uint64_t stateEpoch = enterState(
                TdAuthState::WaitOtherDeviceConfirmation,
                rawStateId,
                TdAuthPromptCloseReason::StateChanged);
            if (!isCurrentState(
                    stateEpoch,
                    TdAuthState::WaitOtherDeviceConfirmation)) {
                return;
            }
            m_openPrompt = nextPrompt();
            m_openPromptType = TdAuthPromptType::QrCode;
        } else {
            ++m_stateEpoch;
            invalidateRequest();
            m_rawStateId = rawStateId;
        }

        m_qrFingerprint = std::move(fingerprint);
        m_qrLinkLength = link.size();
        const TdAuthPromptId prompt = m_openPrompt;
        notifyObserver(
            [&prompt, &link](TdAuthObserver &observer) {
                observer.onQrLinkChanged(prompt, link);
            });
    }

    void handleRegistration(
        std::int32_t rawStateId,
        const authorizationStateWaitRegistration &state)
    {
        ChallengeSnapshot challenge;
        challenge.type = TdAuthPromptType::Registration;
        if (state.terms_of_service_) {
            challenge.registration.minimumAge =
                state.terms_of_service_->min_user_age_;
            challenge.registration.showPopup =
                state.terms_of_service_->show_popup_;
        }
        handleChallengeState(
            TdAuthState::WaitRegistration,
            rawStateId,
            std::move(challenge));
    }

    void handlePassword(
        std::int32_t rawStateId,
        const authorizationStateWaitPassword &state)
    {
        ChallengeSnapshot challenge;
        challenge.type = TdAuthPromptType::Password;
        challenge.password.hint = state.password_hint_;
        challenge.password.hasRecoveryEmailAddress =
            state.has_recovery_email_address_;
        challenge.password.hasPassportData = state.has_passport_data_;
        challenge.password.recoveryEmailAddressPattern =
            state.recovery_email_address_pattern_;
        handleChallengeState(
            TdAuthState::WaitPassword,
            rawStateId,
            std::move(challenge));
    }

    void handleChallengeState(
        TdAuthState state,
        std::int32_t rawStateId,
        ChallengeSnapshot challenge)
    {
        if (m_state == state && m_hasChallenge &&
            sameChallenge(m_challenge, challenge)) {
            if (m_openPrompt.valid() || m_requestPending)
                return;
            issuePrompt();
            return;
        }

        const std::uint64_t stateEpoch = enterState(
            state,
            rawStateId,
            TdAuthPromptCloseReason::StateChanged);
        if (!isCurrentState(stateEpoch, state))
            return;
        m_challenge = std::move(challenge);
        m_hasChallenge = true;
        issuePrompt();
    }

    void issuePrompt()
    {
        if (m_stopped || m_completion != Completion::None ||
            !m_hasChallenge || m_openPrompt.valid()) {
            return;
        }

        m_openPrompt = nextPrompt();
        m_openPromptType = m_challenge.type;
        const TdAuthPromptId prompt = m_openPrompt;
        const ChallengeSnapshot challenge = m_challenge;

        switch (challenge.type) {
        case TdAuthPromptType::PhoneNumber:
            notifyObserver([&prompt](TdAuthObserver &observer) {
                observer.onPhoneNumberRequired(prompt);
            });
            break;
        case TdAuthPromptType::PremiumPurchase:
            notifyObserver([&prompt](TdAuthObserver &observer) {
                observer.onPremiumPurchaseRequired(prompt);
            });
            break;
        case TdAuthPromptType::EmailAddress:
            notifyObserver([&prompt, &challenge](TdAuthObserver &observer) {
                observer.onEmailAddressRequired(
                    prompt, challenge.emailAddress);
            });
            break;
        case TdAuthPromptType::EmailCode:
            notifyObserver([&prompt, &challenge](TdAuthObserver &observer) {
                observer.onEmailCodeRequired(
                    prompt, challenge.emailCode);
            });
            break;
        case TdAuthPromptType::Code:
            notifyObserver([&prompt, &challenge](TdAuthObserver &observer) {
                observer.onCodeRequired(prompt, challenge.code);
            });
            break;
        case TdAuthPromptType::Registration:
            notifyObserver([&prompt, &challenge](TdAuthObserver &observer) {
                observer.onRegistrationRequired(
                    prompt, challenge.registration);
            });
            break;
        case TdAuthPromptType::Password:
            notifyObserver([&prompt, &challenge](TdAuthObserver &observer) {
                observer.onPasswordRequired(
                    prompt, challenge.password);
            });
            break;
        default:
            break;
        }
    }

    TdAuthSubmissionResult submit(
        TdAuthPromptId prompt,
        TdAuthPromptType expectedType,
        TdAuthOperation operation,
        FunctionPtr function,
        bool retryable)
    {
        const TdAuthSubmissionResult validation =
            validatePrompt(prompt, expectedType);
        if (validation != TdAuthSubmissionResult::Accepted)
            return validation;

        startRequest(
            prompt,
            operation,
            std::move(function),
            retryable,
            true);
        return TdAuthSubmissionResult::Accepted;
    }

    TdAuthSubmissionResult rejectInvalidInput(
        TdAuthPromptId prompt,
        TdAuthPromptType expectedType,
        TdAuthOperation operation)
    {
        const TdAuthSubmissionResult validation =
            validatePrompt(prompt, expectedType);
        if (validation != TdAuthSubmissionResult::Accepted)
            return validation;

        const std::uint64_t stateEpoch = m_stateEpoch;
        const TdAuthState state = m_state;
        notifyPromptClosed(TdAuthPromptCloseReason::Submitted);
        if (!isCurrentState(stateEpoch, state))
            return TdAuthSubmissionResult::InvalidInput;

        TdAuthRequestFailure failure;
        failure.state = m_state;
        failure.operation = operation;
        failure.errorCode = 0;
        notifyObserver([&failure](TdAuthObserver &observer) {
            observer.onRequestFailed(failure);
        });
        if (!m_stopped && m_completion == Completion::None &&
            stateEpoch == m_stateEpoch && m_hasChallenge &&
            !m_openPrompt.valid() && !m_requestPending) {
            issuePrompt();
        }
        return TdAuthSubmissionResult::InvalidInput;
    }

    TdAuthSubmissionResult validatePrompt(
        TdAuthPromptId prompt,
        TdAuthPromptType expectedType) const
    {
        if (m_stopped || m_completion != Completion::None)
            return TdAuthSubmissionResult::Stopped;
        if (m_requestPending && prompt == m_pendingPrompt)
            return TdAuthSubmissionResult::RequestInFlight;
        if (!prompt.valid() || prompt != m_openPrompt)
            return TdAuthSubmissionResult::StalePrompt;
        if (expectedType != TdAuthPromptType::None &&
            expectedType != m_openPromptType) {
            return TdAuthSubmissionResult::WrongPromptType;
        }
        return TdAuthSubmissionResult::Accepted;
    }

    void startRequest(
        TdAuthPromptId prompt,
        TdAuthOperation operation,
        FunctionPtr function,
        bool retryable,
        bool closeSubmittedPrompt)
    {
        if (m_stopped || m_completion != Completion::None ||
            m_requestPending || !function) {
            return;
        }

        m_requestPending = true;
        m_pendingOperation = operation;
        m_pendingPrompt = prompt;
        m_pendingRetryable = retryable;
        m_pendingStateEpoch = m_stateEpoch;
        const std::uint64_t requestSerial = ++m_requestSerial;
        m_pendingRequestSerial = requestSerial;

        if (closeSubmittedPrompt)
            notifyPromptClosed(TdAuthPromptCloseReason::Submitted);
        if (!isCurrentRequest(requestSerial))
            return;

        const std::uint64_t stateEpoch = m_pendingStateEpoch;
        std::weak_ptr<TdAuthControllerState> weakState(
            shared_from_this());
        TdAuthController::ResponseCallback response =
            [weakState, stateEpoch, requestSerial, operation, prompt](
                std::uint64_t,
                ObjectPtr object) mutable {
                std::shared_ptr<TdAuthControllerState> state =
                    weakState.lock();
                if (state) {
                    try {
                        state->handleResponse(
                            stateEpoch,
                            requestSerial,
                            operation,
                            prompt,
                            std::move(object));
                    } catch (...) {
                        state->abortUnexpectedException();
                    }
                }
            };

        std::uint64_t requestId = 0;
        try {
            SendCallback sendCallback = m_sendCallback;
            if (sendCallback) {
                requestId = sendCallback(
                    std::move(function), std::move(response));
            }
        } catch (...) {
            requestId = 0;
        }

        if (requestId == 0 && isCurrentRequest(requestSerial)) {
            failCurrentRequest(
                TdAuthFailureType::TransportUnavailable,
                operation,
                0,
                TdAuthPromptCloseReason::Failed);
        }
    }

    void handleResponse(
        std::uint64_t stateEpoch,
        std::uint64_t requestSerial,
        TdAuthOperation operation,
        TdAuthPromptId prompt,
        ObjectPtr response)
    {
        if (m_stopped || m_completion != Completion::None ||
            stateEpoch != m_stateEpoch ||
            !isCurrentRequest(requestSerial) ||
            operation != m_pendingOperation ||
            prompt != m_pendingPrompt) {
            return;
        }

        const bool retryable = m_pendingRetryable;
        invalidateRequest();
        if (response && response->get_id() == ok::ID)
            return;

        if (response && response->get_id() == error::ID && retryable) {
            const std::int32_t errorCode =
                static_cast<const error &>(*response).code_;
            TdAuthRequestFailure failure;
            failure.state = m_state;
            failure.operation = operation;
            failure.errorCode = errorCode;
            notifyObserver([&failure](TdAuthObserver &observer) {
                observer.onRequestFailed(failure);
            });
            if (!m_stopped && m_completion == Completion::None &&
                stateEpoch == m_stateEpoch && m_hasChallenge &&
                !m_openPrompt.valid() && !m_requestPending) {
                issuePrompt();
            }
            return;
        }

        const std::int32_t errorCode =
            response && response->get_id() == error::ID
                ? static_cast<const error &>(*response).code_
                : 0;
        failCurrentRequest(
            response && response->get_id() == error::ID
                ? TdAuthFailureType::RequestRejected
                : TdAuthFailureType::MalformedResponse,
            operation,
            errorCode,
            TdAuthPromptCloseReason::Failed);
    }

    void failCurrentRequest(
        TdAuthFailureType failureType,
        TdAuthOperation operation,
        std::int32_t errorCode,
        TdAuthPromptCloseReason closeReason)
    {
        invalidateRequest();
        failAuthorization(
            failureType,
            m_state,
            operation,
            m_rawStateId,
            errorCode,
            closeReason);
    }

    bool isCurrentRequest(std::uint64_t requestSerial) const
    {
        return m_requestPending &&
            m_pendingRequestSerial == requestSerial;
    }

    void invalidateRequest()
    {
        m_requestPending = false;
        m_pendingOperation = TdAuthOperation::None;
        m_pendingPrompt = TdAuthPromptId();
        m_pendingRetryable = false;
        m_pendingStateEpoch = 0;
        m_pendingRequestSerial = 0;
    }

    std::uint64_t enterState(
        TdAuthState state,
        std::int32_t rawStateId,
        TdAuthPromptCloseReason closeReason)
    {
        const std::uint64_t stateEpoch = ++m_stateEpoch;
        invalidateRequest();
        m_state = state;
        m_rawStateId = rawStateId;
        clearChallenge();
        notifyPromptClosed(closeReason);
        return stateEpoch;
    }

    bool isCurrentState(
        std::uint64_t stateEpoch,
        TdAuthState state) const
    {
        return !m_stopped && m_completion == Completion::None &&
            stateEpoch == m_stateEpoch && m_state == state;
    }

    void completeReady(std::int32_t rawStateId)
    {
        if (m_completion != Completion::None)
            return;
        ++m_stateEpoch;
        invalidateRequest();
        m_state = TdAuthState::Ready;
        m_rawStateId = rawStateId;
        clearChallenge();
        m_completion = Completion::Ready;
        notifyCompletion(
            TdAuthPromptCloseReason::Ready,
            [](TdAuthObserver &observer) {
                observer.onAuthorizationReady();
            });
    }

    void handleLifecycle(TdAuthState state)
    {
        if (m_stopped)
            return;
        if (m_dispatchingCompletion || m_drainingLifecycle) {
            m_pendingLifecycle.push_back(state);
            return;
        }
        dispatchLifecycle(state);
    }

    void dispatchLifecycle(TdAuthState state)
    {
        bool *notified = nullptr;
        switch (state) {
        case TdAuthState::LoggingOut:
            notified = &m_loggingOutNotified;
            break;
        case TdAuthState::Closing:
            notified = &m_closingNotified;
            break;
        case TdAuthState::Closed:
            notified = &m_closedNotified;
            break;
        default:
            return;
        }
        if (*notified)
            return;
        *notified = true;

        ++m_stateEpoch;
        invalidateRequest();
        m_state = state;
        clearChallenge();

        if (m_completion == Completion::None) {
            m_completion = Completion::Failed;
            TdAuthFailure failure;
            failure.type = TdAuthFailureType::TerminalState;
            failure.state = state;
            notifyCompletion(
                TdAuthPromptCloseReason::TerminalState,
                [&failure](TdAuthObserver &observer) {
                    observer.onAuthorizationFailed(failure);
                });
            return;
        }
        if (m_completion != Completion::Ready)
            return;

        switch (state) {
        case TdAuthState::LoggingOut:
            notifyObserver([](TdAuthObserver &observer) {
                observer.onLoggingOut();
            });
            break;
        case TdAuthState::Closing:
            notifyObserver([](TdAuthObserver &observer) {
                observer.onClosing();
            });
            break;
        case TdAuthState::Closed:
            notifyObserver([](TdAuthObserver &observer) {
                observer.onClosed();
            });
            break;
        default:
            break;
        }
    }

    void failAuthorization(
        TdAuthFailureType failureType,
        TdAuthState state,
        TdAuthOperation operation,
        std::int32_t rawStateId,
        std::int32_t errorCode,
        TdAuthPromptCloseReason closeReason)
    {
        if (m_stopped || m_completion != Completion::None)
            return;
        ++m_stateEpoch;
        invalidateRequest();
        m_state = state;
        m_rawStateId = rawStateId;
        clearChallenge();
        m_completion = Completion::Failed;

        TdAuthFailure failure;
        failure.type = failureType;
        failure.state = state;
        failure.operation = operation;
        failure.rawStateId = rawStateId;
        failure.errorCode = errorCode;
        notifyCompletion(
            closeReason,
            [&failure](TdAuthObserver &observer) {
                observer.onAuthorizationFailed(failure);
            });
    }

    ClosedPrompt discardPrompt() noexcept
    {
        ClosedPrompt closed;
        closed.prompt = m_openPrompt;
        closed.type = m_openPromptType;
        m_openPrompt = TdAuthPromptId();
        m_openPromptType = TdAuthPromptType::None;
        m_qrFingerprint.clear();
        m_qrLinkLength = 0;
        return closed;
    }

    void clearChallenge()
    {
        m_challenge = ChallengeSnapshot();
        m_hasChallenge = false;
    }

    void notifyPromptClosed(TdAuthPromptCloseReason reason) noexcept
    {
        const ClosedPrompt closed = discardPrompt();
        if (!closed.prompt.valid())
            return;
        notifyObserver([&closed, reason](TdAuthObserver &observer) {
            observer.onPromptClosed(
                closed.prompt, closed.type, reason);
        });
    }

    template <typename Callback>
    void notifyCompletion(
        TdAuthPromptCloseReason reason,
        Callback callback) noexcept
    {
        const ClosedPrompt closed = discardPrompt();
        TdAuthObserver *observer = m_observer;
        if (!observer)
            return;

        m_dispatchingCompletion = true;
        bool observerFailed = false;
        if (closed.prompt.valid()) {
            try {
                observer->onPromptClosed(
                    closed.prompt, closed.type, reason);
            } catch (...) {
                observerFailed = true;
            }
        }
        try {
            callback(*observer);
        } catch (...) {
            observerFailed = true;
        }
        m_dispatchingCompletion = false;

        if (observerFailed) {
            stopAfterObserverFailure();
            return;
        }
        drainPendingLifecycle();
    }

    void drainPendingLifecycle()
    {
        if (m_stopped || m_dispatchingCompletion || m_drainingLifecycle ||
            m_pendingLifecycle.empty()) {
            return;
        }
        m_drainingLifecycle = true;
        std::size_t next = 0;
        while (!m_stopped && next < m_pendingLifecycle.size()) {
            const TdAuthState state = m_pendingLifecycle[next++];
            dispatchLifecycle(state);
        }
        m_pendingLifecycle.clear();
        m_drainingLifecycle = false;
    }

    TdAuthPromptId nextPrompt()
    {
        ++m_lastPrompt;
        if (m_lastPrompt == 0)
            ++m_lastPrompt;
        return TdAuthPromptId(m_lastPrompt);
    }

    const TdAuthConfiguration m_configuration;
    SendCallback m_sendCallback;
    TdAuthObserver *m_observer;

    Completion m_completion = Completion::None;
    bool m_stopped = false;
    TdAuthState m_state = TdAuthState::None;
    std::int32_t m_rawStateId = 0;
    std::uint64_t m_stateEpoch = 0;
    std::uint64_t m_lastPrompt = 0;

    ChallengeSnapshot m_challenge;
    bool m_hasChallenge = false;
    TdAuthPromptId m_openPrompt;
    TdAuthPromptType m_openPromptType = TdAuthPromptType::None;
    std::string m_qrFingerprint;
    std::size_t m_qrLinkLength = 0;
    bool m_qrRequested = false;

    bool m_requestPending = false;
    TdAuthOperation m_pendingOperation = TdAuthOperation::None;
    TdAuthPromptId m_pendingPrompt;
    bool m_pendingRetryable = false;
    std::uint64_t m_pendingStateEpoch = 0;
    std::uint64_t m_requestSerial = 0;
    std::uint64_t m_pendingRequestSerial = 0;

    bool m_loggingOutNotified = false;
    bool m_closingNotified = false;
    bool m_closedNotified = false;
    bool m_dispatchingCompletion = false;
    bool m_drainingLifecycle = false;
    std::vector<TdAuthState> m_pendingLifecycle;
};

TdAuthController::TdAuthController(
    TdAuthConfiguration configuration,
    SendCallback sendCallback,
    TdAuthObserver &observer)
    : m_state(std::make_shared<TdAuthControllerState>(
          std::move(configuration),
          std::move(sendCallback),
          observer))
{}

TdAuthController::~TdAuthController()
{
    std::shared_ptr<TdAuthControllerState> state =
        std::move(m_state);
    if (state)
        state->shutdown();
}

void TdAuthController::onAuthorizationState(
    const td::td_api::AuthorizationState *state)
{
    std::shared_ptr<TdAuthControllerState> controllerState = m_state;
    if (!controllerState)
        return;
    try {
        controllerState->onAuthorizationState(state);
    } catch (...) {
        controllerState->abortUnexpectedException();
    }
}

TdAuthSubmissionResult TdAuthController::submitPhoneNumber(
    TdAuthPromptId prompt,
    const std::string &phoneNumber)
{
    std::shared_ptr<TdAuthControllerState> state = m_state;
    if (!state)
        return TdAuthSubmissionResult::Stopped;
    try {
        return state->submitPhoneNumber(prompt, phoneNumber);
    } catch (...) {
        state->abortUnexpectedException();
        return TdAuthSubmissionResult::Stopped;
    }
}

TdAuthSubmissionResult TdAuthController::submitEmailAddress(
    TdAuthPromptId prompt,
    const std::string &emailAddress)
{
    std::shared_ptr<TdAuthControllerState> state = m_state;
    if (!state)
        return TdAuthSubmissionResult::Stopped;
    try {
        return state->submitEmailAddress(prompt, emailAddress);
    } catch (...) {
        state->abortUnexpectedException();
        return TdAuthSubmissionResult::Stopped;
    }
}

TdAuthSubmissionResult TdAuthController::submitEmailCode(
    TdAuthPromptId prompt,
    const std::string &code)
{
    std::shared_ptr<TdAuthControllerState> state = m_state;
    if (!state)
        return TdAuthSubmissionResult::Stopped;
    try {
        return state->submitEmailCode(prompt, code);
    } catch (...) {
        state->abortUnexpectedException();
        return TdAuthSubmissionResult::Stopped;
    }
}

TdAuthSubmissionResult TdAuthController::submitCode(
    TdAuthPromptId prompt,
    const std::string &code)
{
    std::shared_ptr<TdAuthControllerState> state = m_state;
    if (!state)
        return TdAuthSubmissionResult::Stopped;
    try {
        return state->submitCode(prompt, code);
    } catch (...) {
        state->abortUnexpectedException();
        return TdAuthSubmissionResult::Stopped;
    }
}

TdAuthSubmissionResult TdAuthController::submitPassword(
    TdAuthPromptId prompt,
    const std::string &password)
{
    std::shared_ptr<TdAuthControllerState> state = m_state;
    if (!state)
        return TdAuthSubmissionResult::Stopped;
    try {
        return state->submitPassword(prompt, password);
    } catch (...) {
        state->abortUnexpectedException();
        return TdAuthSubmissionResult::Stopped;
    }
}

TdAuthSubmissionResult TdAuthController::submitRegistration(
    TdAuthPromptId prompt,
    const std::string &firstName,
    const std::string &lastName)
{
    std::shared_ptr<TdAuthControllerState> state = m_state;
    if (!state)
        return TdAuthSubmissionResult::Stopped;
    try {
        return state->submitRegistration(
            prompt, firstName, lastName);
    } catch (...) {
        state->abortUnexpectedException();
        return TdAuthSubmissionResult::Stopped;
    }
}

TdAuthSubmissionResult TdAuthController::cancelPrompt(
    TdAuthPromptId prompt)
{
    std::shared_ptr<TdAuthControllerState> state = m_state;
    if (!state)
        return TdAuthSubmissionResult::Stopped;
    try {
        return state->cancelPrompt(prompt);
    } catch (...) {
        state->abortUnexpectedException();
        return TdAuthSubmissionResult::Stopped;
    }
}

TdAuthSubmissionResult TdAuthController::failPrompt(
    TdAuthPromptId prompt,
    TdAuthPresentationFailure failure)
{
    std::shared_ptr<TdAuthControllerState> state = m_state;
    if (!state)
        return TdAuthSubmissionResult::Stopped;
    try {
        return state->failPrompt(prompt, failure);
    } catch (...) {
        state->abortUnexpectedException();
        return TdAuthSubmissionResult::Stopped;
    }
}

bool TdAuthController::cancel()
{
    std::shared_ptr<TdAuthControllerState> state = m_state;
    if (!state)
        return false;
    try {
        return state->cancel();
    } catch (...) {
        state->abortUnexpectedException();
        return false;
    }
}

void TdAuthController::shutdown() noexcept
{
    std::shared_ptr<TdAuthControllerState> state = m_state;
    if (state)
        state->shutdown();
}
