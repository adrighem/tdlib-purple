#ifndef TD_AUTH_CONTROLLER_H
#define TD_AUTH_CONTROLLER_H

#include <td/telegram/td_api.hpp>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

enum class TdAuthMode : std::uint8_t {
    PhoneNumber,
    QrCode,
    QrCodeWithPhoneFallback,
};

// Immutable, Purple-neutral snapshot used to initialize one TDLib
// authorization session.
class TdAuthConfiguration {
public:
    TdAuthConfiguration(
        std::int32_t apiId,
        std::string apiHash,
        std::string databaseDirectory,
        bool useSecretChats,
        TdAuthMode mode);

    std::int32_t apiId() const noexcept;
    const std::string &apiHash() const noexcept;
    const std::string &databaseDirectory() const noexcept;
    bool useSecretChats() const noexcept;
    TdAuthMode mode() const noexcept;
    bool valid() const noexcept;

private:
    std::int32_t m_apiId;
    std::string m_apiHash;
    std::string m_databaseDirectory;
    bool m_useSecretChats;
    TdAuthMode m_mode;
};

enum class TdAuthState : std::uint8_t {
    None,
    WaitTdlibParameters,
    WaitPhoneNumber,
    WaitPremiumPurchase,
    WaitEmailAddress,
    WaitEmailCode,
    WaitCode,
    WaitOtherDeviceConfirmation,
    WaitRegistration,
    WaitPassword,
    Ready,
    LoggingOut,
    Closing,
    Closed,
    Unknown,
};

enum class TdAuthOperation : std::uint8_t {
    None,
    SetTdlibParameters,
    RequestQrCode,
    SetPhoneNumber,
    SetEmailAddress,
    CheckEmailCode,
    CheckCode,
    RegisterUser,
    CheckPassword,
};

enum class TdAuthPromptType : std::uint8_t {
    None,
    PhoneNumber,
    PremiumPurchase,
    EmailAddress,
    EmailCode,
    Code,
    QrCode,
    Registration,
    Password,
};

class TdAuthPromptId {
public:
    TdAuthPromptId() noexcept = default;
    explicit TdAuthPromptId(std::uint64_t value) noexcept;

    std::uint64_t value() const noexcept;
    bool valid() const noexcept;

private:
    std::uint64_t m_value = 0;
};

bool operator==(TdAuthPromptId left, TdAuthPromptId right) noexcept;
bool operator!=(TdAuthPromptId left, TdAuthPromptId right) noexcept;

enum class TdAuthSubmissionResult : std::uint8_t {
    Accepted,
    StalePrompt,
    WrongPromptType,
    RequestInFlight,
    InvalidInput,
    Stopped,
};

enum class TdAuthCodeType : std::uint8_t {
    None,
    TelegramMessage,
    Sms,
    SmsWord,
    SmsPhrase,
    PhoneCall,
    FlashCall,
    MissedCall,
    Fragment,
    FirebaseAndroid,
    FirebaseIos,
    Unknown,
};

struct TdAuthCodeDelivery {
    TdAuthCodeType type = TdAuthCodeType::None;
    std::int32_t length = 0;
    // Only the legacy flash-call pattern is copied. Fragment URLs, Firebase
    // receipts, phone prefixes, and other tokens never cross this boundary.
    std::string pattern;
};

struct TdAuthCodeChallenge {
    TdAuthCodeDelivery code;
    TdAuthCodeDelivery nextCode;
    std::int32_t timeout = 0;
};

struct TdAuthEmailAddressChallenge {
    bool allowAppleId = false;
    bool allowGoogleId = false;
};

enum class TdAuthEmailResetState : std::uint8_t {
    None,
    Available,
    Pending,
    Unknown,
};

struct TdAuthEmailCodeChallenge {
    bool allowAppleId = false;
    bool allowGoogleId = false;
    std::string emailAddressPattern;
    std::int32_t length = 0;
    TdAuthEmailResetState resetState = TdAuthEmailResetState::None;
    std::int32_t resetSeconds = 0;
};

struct TdAuthRegistrationChallenge {
    std::int32_t minimumAge = 0;
    bool showPopup = false;
};

struct TdAuthPasswordChallenge {
    std::string hint;
    bool hasRecoveryEmailAddress = false;
    bool hasPassportData = false;
    std::string recoveryEmailAddressPattern;
};

enum class TdAuthPromptCloseReason : std::uint8_t {
    Submitted,
    StateChanged,
    Ready,
    TerminalState,
    Cancelled,
    Failed,
    PresentationFailed,
    Shutdown,
};

enum class TdAuthPresentationFailure : std::uint8_t {
    Unsupported,
    Failed,
};

enum class TdAuthFailureType : std::uint8_t {
    InvalidConfiguration,
    RequestRejected,
    TransportUnavailable,
    MalformedResponse,
    MalformedState,
    UnsupportedState,
    PresentationUnavailable,
    PresentationFailed,
    TerminalState,
    InternalError,
};

struct TdAuthFailure {
    TdAuthFailureType type = TdAuthFailureType::MalformedState;
    TdAuthState state = TdAuthState::None;
    TdAuthOperation operation = TdAuthOperation::None;
    std::int32_t rawStateId = 0;
    std::int32_t errorCode = 0;
    std::string errorMessage;
};

struct TdAuthRequestFailure {
    TdAuthState state = TdAuthState::None;
    TdAuthOperation operation = TdAuthOperation::None;
    std::int32_t errorCode = 0;
    std::string errorMessage;
};

// All callbacks are synchronous and run on the controller's owner context.
// The observer must outlive the controller and any callback it initiates.
// Callbacks may shut down or destroy the controller. Sensitive values such as
// QR links and password hints must not be logged or persisted. The raw QR link
// is valid only for the duration of the callback.
class TdAuthObserver {
public:
    virtual ~TdAuthObserver() {}

    virtual void onPhoneNumberRequired(TdAuthPromptId prompt) = 0;
    virtual void onPremiumPurchaseRequired(TdAuthPromptId prompt) = 0;
    virtual void onEmailAddressRequired(
        TdAuthPromptId prompt,
        const TdAuthEmailAddressChallenge &challenge) = 0;
    virtual void onEmailCodeRequired(
        TdAuthPromptId prompt,
        const TdAuthEmailCodeChallenge &challenge) = 0;
    virtual void onCodeRequired(
        TdAuthPromptId prompt,
        const TdAuthCodeChallenge &challenge) = 0;
    virtual void onQrLinkChanged(
        TdAuthPromptId prompt,
        const std::string &link) = 0;
    virtual void onRegistrationRequired(
        TdAuthPromptId prompt,
        const TdAuthRegistrationChallenge &challenge) = 0;
    virtual void onPasswordRequired(
        TdAuthPromptId prompt,
        const TdAuthPasswordChallenge &challenge) = 0;
    virtual void onPromptClosed(
        TdAuthPromptId prompt,
        TdAuthPromptType type,
        TdAuthPromptCloseReason reason) = 0;

    // A recoverable input rejection is followed by a fresh prompt. Raw TDLib
    // error text and the submitted value are deliberately unavailable.
    virtual void onRequestFailed(
        const TdAuthRequestFailure &failure) = 0;

    // Unless shutdown() explicitly abandons an active controller, exactly one
    // of ready, cancelled, or failed is emitted to a functioning observer.
    virtual void onAuthorizationReady() = 0;
    virtual void onAuthorizationCancelled() = 0;
    virtual void onAuthorizationFailed(
        const TdAuthFailure &failure) = 0;

    // Lifecycle states remain observable after successful authorization.
    virtual void onLoggingOut() = 0;
    virtual void onClosing() = 0;
    virtual void onClosed() = 0;
};

class TdAuthControllerState;

// Purple-neutral TDLib authorization state machine. All public methods are
// serialized on the transport's captured owner context. State updates are
// authoritative: responses for an older state epoch are ignored.
class TdAuthController {
public:
    using ObjectPtr = td::td_api::object_ptr<td::td_api::Object>;
    using FunctionPtr = td::td_api::object_ptr<td::td_api::Function>;
    using ResponseCallback =
        std::function<void(std::uint64_t, ObjectPtr)>;
    using SendCallback =
        std::function<std::uint64_t(FunctionPtr, ResponseCallback)>;

    TdAuthController(
        TdAuthConfiguration configuration,
        SendCallback sendCallback,
        TdAuthObserver &observer);
    ~TdAuthController();

    TdAuthController(const TdAuthController &) = delete;
    TdAuthController &operator=(const TdAuthController &) = delete;

    void onAuthorizationState(
        const td::td_api::AuthorizationState *state);

    TdAuthSubmissionResult submitPhoneNumber(
        TdAuthPromptId prompt,
        const std::string &phoneNumber);
    TdAuthSubmissionResult submitEmailAddress(
        TdAuthPromptId prompt,
        const std::string &emailAddress);
    TdAuthSubmissionResult submitEmailCode(
        TdAuthPromptId prompt,
        const std::string &code);
    TdAuthSubmissionResult submitCode(
        TdAuthPromptId prompt,
        const std::string &code);
    TdAuthSubmissionResult submitPassword(
        TdAuthPromptId prompt,
        const std::string &password);
    TdAuthSubmissionResult submitRegistration(
        TdAuthPromptId prompt,
        const std::string &firstName,
        const std::string &lastName);

    TdAuthSubmissionResult cancelPrompt(TdAuthPromptId prompt);
    TdAuthSubmissionResult failPrompt(
        TdAuthPromptId prompt,
        TdAuthPresentationFailure failure);
    bool cancel();
    // Invalidates callbacks without completing authorization. Intended for
    // frontend teardown after it has already chosen the visible outcome.
    void shutdown() noexcept;

private:
    std::shared_ptr<TdAuthControllerState> m_state;
};

#endif
