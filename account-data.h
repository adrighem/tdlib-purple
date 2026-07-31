#ifndef _ACCOUNT_DATA_H
#define _ACCOUNT_DATA_H

#include "buildopt.h"
#include "identifiers.h"
#include "transceiver.h"
#include <td/telegram/td_api.h>

#include <map>
#include <mutex>
#include <set>
#include <list>
#include <memory>
#include <vector>
#include <purple.h>

#ifndef NoVoip
#include <VoIPController.h>
#else

namespace tgvoip {
    struct VoIPController {};
}
#endif

bool        isPhoneNumber(const char *s);
const char *getCanonicalPhoneNumber(const char *s);
UserId      purpleBuddyNameToUserId(const char *s);
SecretChatId purpleBuddyNameToSecretChatId(const char *s);
bool        isPrivateChat(const td::td_api::chat &chat);
UserId      getUserIdByPrivateChat(const td::td_api::chat &chat);
bool        isChatInContactList(const td::td_api::chat &chat, const td::td_api::user *privateChatUser);
BasicGroupId getBasicGroupId(const td::td_api::chat &chat);
SupergroupId getSupergroupId(const td::td_api::chat &chat);
SecretChatId getSecretChatId(const td::td_api::chat &chat);
bool        isActiveBasicGroup(const td::td_api::basicGroup &group);
bool        isGroupMember(const td::td_api::object_ptr<td::td_api::ChatMemberStatus> &status);
bool        isSameUser(const td::td_api::MessageSender &member1, const td::td_api::MessageSender &member2);

enum {
    CHAT_HISTORY_REQUEST_LIMIT  = 50,
    CHAT_HISTORY_RETRIEVE_LIMIT = 100
};

class PendingRequest {
public:
    uint64_t requestId;

    PendingRequest(uint64_t requestId) : requestId(requestId) {}
    virtual ~PendingRequest() {}
};

class GroupInfoRequest: public PendingRequest {
public:
    BasicGroupId groupId;

    GroupInfoRequest(uint64_t requestId, BasicGroupId groupId)
    : PendingRequest(requestId), groupId(groupId) {}
};

class SupergroupInfoRequest: public PendingRequest {
public:
    SupergroupId groupId;

    SupergroupInfoRequest(uint64_t requestId, SupergroupId groupId)
    : PendingRequest(requestId), groupId(groupId) {}
};

class SupergroupMembersRequest: public PendingRequest {
public:
    SupergroupId groupId;
    uint64_t     membersRevision;

    SupergroupMembersRequest(
        uint64_t requestId, SupergroupId groupId,
        uint64_t membersRevision)
    : PendingRequest(requestId), groupId(groupId),
      membersRevision(membersRevision) {}
};

class GroupMembersRequestCont: public PendingRequest {
public:
    SupergroupId groupId;
    uint64_t     membersRevision;
    td::td_api::object_ptr<td::td_api::chatMembers> members;

    GroupMembersRequestCont(
        uint64_t requestId, SupergroupId groupId,
        uint64_t membersRevision,
        td::td_api::chatMembers *members)
    : PendingRequest(requestId), groupId(groupId),
      membersRevision(membersRevision),
      members(std::move(members)) {}
};

class ContactRequest: public PendingRequest {
public:
    std::string phoneNumber;
    std::string alias;
    std::string groupName;
    UserId      userId;

    ContactRequest(uint64_t requestId, const std::string &phoneNumber, const std::string &alias,
                   const std::string &groupName, UserId userId)
    : PendingRequest(requestId), phoneNumber(phoneNumber), alias(alias), groupName(groupName),
      userId(userId) {}
};

class GroupJoinRequest: public PendingRequest {
public:
    enum class Type {
        InviteLink,
        Username,
    };
    std::string joinString;
    Type        type;
    ChatId      chatId;

    GroupJoinRequest(uint64_t requestId, const std::string &joinString, Type type,
                     ChatId chatId = ChatId::invalid)
    : PendingRequest(requestId), joinString(joinString), type(type), chatId(chatId) {}
};

class SendMessageRequest: public PendingRequest {
public:
    ChatTarget  target;
    std::string tempFile;

    SendMessageRequest(
        uint64_t requestId, ChatTarget target,
        const char *tempFile)
    : PendingRequest(requestId),
      target(target),
      tempFile(tempFile ? tempFile : "")
    {}

    ~SendMessageRequest() override;
};

class UploadRequest: public PendingRequest {
public:
    PurpleXfer *xfer;
    ChatTarget  target;

    UploadRequest(
        uint64_t requestId, PurpleXfer *xfer, ChatTarget target)
    : PendingRequest(requestId), xfer(xfer), target(target) {}
};

struct TgMessageInfo {
    enum class Type {
        Photo,
        Sticker,
        Other
    };
    ChatTarget  target;
    MessageId   id;
    Type        type = Type::Other;
    std::string incomingGroupchatSender;
    time_t      timestamp = 0;
    bool        outgoing = false;
    bool        sentLocally = false; // For outgoing messages, whether sent by this very client
    bool        forumTopicDisplayAccepted = false;
    bool        readReceiptEligible = false;
    bool        isLiveUpdate = false;
    MessageId   repliedMessageId;
    td::td_api::object_ptr<td::td_api::message> repliedMessage;
    std::string forwardedFrom;

    void assign(const TgMessageInfo &other)
    {
        target = other.target;
        id = other.id;
        type = other.type;
        incomingGroupchatSender = other.incomingGroupchatSender;
        timestamp = other.timestamp;
        outgoing = other.outgoing;
        sentLocally = other.sentLocally;
        forumTopicDisplayAccepted =
            other.forumTopicDisplayAccepted;
        readReceiptEligible = other.readReceiptEligible;
        isLiveUpdate = other.isLiveUpdate;
        repliedMessageId = other.repliedMessageId;
        repliedMessage = nullptr;
        forwardedFrom = other.forwardedFrom;
    }
};

class PendingMessageState {
public:
    enum class Disposition {
        Queued,
        Released,
        Cancelled,
    };

    ChatId chatId() const { return m_chatId; }
    MessageId messageId() const { return m_messageId; }
    uint64_t contentRevision() const {
        return m_contentRevision;
    }
    Disposition disposition() const { return m_disposition; }

private:
    friend class PendingMessageQueue;

    PendingMessageState(ChatId chatId, MessageId messageId)
    : m_chatId(chatId), m_messageId(messageId)
    {}

    ChatId      m_chatId;
    MessageId   m_messageId;
    uint64_t    m_contentRevision = 1;
    Disposition m_disposition = Disposition::Queued;
};

struct PendingMessageHandle {
    std::shared_ptr<PendingMessageState> state;

    bool valid() const { return static_cast<bool>(state); }
    ChatId chatId() const {
        return state ? state->chatId() : ChatId::invalid;
    }
    MessageId messageId() const {
        return state ? state->messageId() : MessageId::invalid;
    }
    PendingMessageState::Disposition disposition() const {
        return state
            ? state->disposition()
            : PendingMessageState::Disposition::Cancelled;
    }
};

struct PendingContentHandle {
    PendingMessageHandle message;
    uint64_t revision = 0;

    bool valid() const {
        return message.valid() && revision != 0;
    }
    bool current() const {
        return valid() &&
               revision == message.state->contentRevision();
    }
    bool currentAndNotCancelled() const {
        return current() &&
               message.disposition() !=
                   PendingMessageState::Disposition::Cancelled;
    }
};

class PurpleTdClient;

enum class ReceiveTransferKind {
    None,
    InlineProgress,
    Standard,
};

// Used for matching completed downloads to chats they belong to, and for starting PurpleXfer for
// time-consuming downloads
class DownloadRequest: public PendingRequest {
public:
    ReceiveTransferKind transferKind;
    ChatId         chatId;

    // For inline downloads this is a copy of original TgMessageInfo from IncomingMessage.
    TgMessageInfo  message;

    int32_t        fileId;
    int32_t        fileSize;
    int32_t        downloadedSize;
    std::string    fileDescription;
    int            tempFd = -1;
    std::string    tempFileName;
    td::td_api::object_ptr<td::td_api::file> thumbnail;
    PendingContentHandle pendingContent;

    // Could not pass object_ptr through variadic funciton :(
    DownloadRequest(uint64_t requestId,
                    ReceiveTransferKind transferKind,
                    ChatId chatId, TgMessageInfo &message,
                    int32_t fileId, int32_t fileSize, const std::string &fileDescription,
                    td::td_api::file *thumbnail,
                    PendingContentHandle pendingContent)
    : PendingRequest(requestId), transferKind(transferKind),
      chatId(chatId), fileId(fileId),
      fileSize(fileSize), downloadedSize(0), fileDescription(fileDescription),
      thumbnail(thumbnail),
      pendingContent(std::move(pendingContent))
    {
        // If download is started while the message is in PendingMessageQueue, repliedMessage will
        // be on IncomingMessage, and one here in TgMessageInfo will be NULL. In this case,
        // repliedMessage will be moved onto DownloadRequest if message leaves PendingMessageQueue
        // before download is complete (meaning it took more than 1 second to download).
        this->message.assign(message);
        if (message.repliedMessage)
            this->message.repliedMessage = std::move(message.repliedMessage);
    }

    ~DownloadRequest() override;
};

class AvatarDownloadRequest: public PendingRequest {
public:
    UserId userId;
    ChatId chatId;

    AvatarDownloadRequest(uint64_t requestId, const td::td_api::user *user)
    : PendingRequest(requestId), userId(getId(*user)), chatId(ChatId::invalid) {}
    AvatarDownloadRequest(uint64_t requestId, const td::td_api::chat *chat)
    : PendingRequest(requestId), userId(UserId::invalid), chatId(getId(*chat)) {}
};

class ProfilePhotoRequest: public PendingRequest {
public:
    std::string tempFile;

    ProfilePhotoRequest(uint64_t requestId, const char *tempFile)
    : PendingRequest(requestId), tempFile(tempFile ? tempFile : "") {}
};

class NewPrivateChatForMessage: public PendingRequest {
public:
    std::string  username;
    std::string  message;
    PurpleXfer  *fileUpload;

    NewPrivateChatForMessage(uint64_t requestId, const char *username, const char *message)
    : PendingRequest(requestId), username(username), message(message ? message : nullptr),
      fileUpload(nullptr) {}

    NewPrivateChatForMessage(uint64_t requestId, const char *username, PurpleXfer *upload)
    : PendingRequest(requestId), username(username), fileUpload(upload) {}
};

class ChatActionRequest: public PendingRequest {
public:
    enum class Type: uint8_t {
        Kick,
        Invite,
        GenerateInviteLink
    };
    Type   type;
    ChatId chatId;
    ChatActionRequest(uint64_t requestId, Type type, ChatId chatId)
    : PendingRequest(requestId), type(type), chatId(chatId) {}
};

struct UnreadReactionInfo {
    std::string sender;
    std::string text;
};

class UnreadReactionsRequest: public PendingRequest {
public:
    ChatId chatId;
    MessageId messageId;
    std::vector<UnreadReactionInfo> reactions;

    UnreadReactionsRequest(uint64_t requestId, ChatId chatId, MessageId messageId,
                           std::vector<UnreadReactionInfo> reactions)
    : PendingRequest(requestId), chatId(chatId), messageId(messageId),
      reactions(std::move(reactions)) {}
};

struct IncomingMessage {
    td::td_api::object_ptr<td::td_api::message> message;
    td::td_api::object_ptr<td::td_api::message> repliedMessage;
    td::td_api::object_ptr<td::td_api::file>    thumbnail;
    std::string inlineDownloadedFilePath;

    // This doesn't have to be a separate struct, it exists for historical reasons.
    // Could be refactored.
    TgMessageInfo messageInfo;

    int32_t  selectedPhotoSizeId;
    unsigned inlineFileSizeLimit;
    bool     standardDownloadConfigured;
    bool     repliedMessageFetchDoneOrFailed;
    bool     inlineDownloadComplete;
    bool     inlineDownloadTimeout;
    bool     animatedStickerConverted;
    bool     animatedStickerConvertSuccess;
    int      animatedStickerImageId;
    // Set only for the synchronous best-effort display performed while the
    // client is being destroyed. Such a display must not start new work.
    bool     forcedSyncDisplay = false;
    PendingMessageHandle pendingMessage;
    // Immutable content incarnation captured when this message is queued or
    // replaced. Do not reconstruct it from pendingMessage at display time:
    // a reentrant edit can advance the shared state's revision while an old
    // released IncomingMessage is still waiting in a display batch.
    PendingContentHandle pendingContent;
};

struct PreparedMessageContent {
    td::td_api::object_ptr<td::td_api::MessageContent> content;
    td::td_api::object_ptr<td::td_api::file> thumbnail;
    TgMessageInfo::Type type = TgMessageInfo::Type::Other;
    int32_t selectedPhotoSizeId = 0;
};

class PendingMessageQueue {
public:
    enum class MessageAction {
        Append,
        Prepend
    };
    static constexpr MessageAction Append = MessageAction::Append;
    static constexpr MessageAction Prepend = MessageAction::Prepend;
    using TdMessagePtr = td::td_api::object_ptr<td::td_api::message>;

    struct RemoveResult {
        size_t removedCount = 0;
        std::vector<IncomingMessage> readyMessages;
    };

    PendingMessageHandle addPendingMessage(
                                     IncomingMessage &&message,
                                     MessageAction action);
    void             setMessageReady(const PendingMessageHandle &handle,
                                     std::vector<IncomingMessage> &readyMessages);
    IncomingMessage  addReadyMessage(IncomingMessage &&message, MessageAction action);
    IncomingMessage *findPendingMessage(
                                     const PendingMessageHandle &handle);
    IncomingMessage *findPendingMessage(
                                     const PendingContentHandle &handle);
    PendingContentHandle currentContentHandle(
                                     const PendingMessageHandle &handle) const;
    PendingContentHandle replaceMessageContent(
                                     ChatId chatId, MessageId messageId,
                                     PreparedMessageContent &&content);
    RemoveResult     removeMessages(
                                     ChatId chatId,
                                     const std::vector<MessageId> &messageIds);
    void             flush(std::vector<IncomingMessage> &messages);
    void             setChatNotReady(ChatId chatId);
    void             setChatReady(ChatId chatId, std::vector<IncomingMessage> &readyMessages);
    bool             isChatReady(ChatId chatId);
private:
    using MessageKey = std::pair<ChatId, MessageId>;

    struct Message {
        IncomingMessage message;
        bool            ready;
    };
    struct ChatQueue {
        ChatId             chatId;
        bool               ready = true;
        std::list<Message> messages;
    };
    std::vector<ChatQueue> m_queues;
    std::map<MessageKey, std::weak_ptr<PendingMessageState>>
        m_releasedMessages;

    std::vector<ChatQueue>::iterator getChatQueue(ChatId chatId);
    Message &addMessage(ChatQueue &queue, MessageAction action);
    PendingMessageHandle assignFreshHandle(IncomingMessage &message);
    Message *findMessage(const PendingMessageHandle &handle);
    void markReleased(IncomingMessage &message);
    void rememberReleased(
        const PendingMessageHandle &handle);
    void invalidateReleasedContent(
        ChatId chatId, MessageId messageId);
    void cancelReleased(
        ChatId chatId, MessageId messageId);
    void cancelReleased(
        ChatId chatId,
        const std::vector<MessageId> &messageIds);
    void cancelAllReleased();
    void pruneReleased();
    void extractReadyMessages(std::vector<ChatQueue>::iterator pQueue,
                              std::vector<IncomingMessage> &readyMessages);
};

class TdAccountData {
public:
    using TdUserPtr           = td::td_api::object_ptr<td::td_api::user>;
    using TdChatPtr           = td::td_api::object_ptr<td::td_api::chat>;
    using TdGroupPtr          = td::td_api::object_ptr<td::td_api::basicGroup>;
    using TdGroupInfoPtr      = td::td_api::object_ptr<td::td_api::basicGroupFullInfo>;
    using TdSupergroupPtr     = td::td_api::object_ptr<td::td_api::supergroup>;
    using TdSupergroupInfoPtr = td::td_api::object_ptr<td::td_api::supergroupFullInfo>;
    using TdChatMembersPtr    = td::td_api::object_ptr<td::td_api::chatMembers>;
    using TdChatMemberPtr     = td::td_api::object_ptr<td::td_api::chatMember>;
    using SecretChatPtr       = td::td_api::object_ptr<td::td_api::secretChat>;

    struct ForumTopicState {
        ChatTarget target;
        std::string name;
        int32_t purpleId = 0;
        bool closed = false;
        bool hidden = false;
        bool deleted = false;
        bool saved = false;
        bool active = false;
        uint64_t metadataGeneration = 0;
        uint64_t lastLiveMessageGeneration = 0;
        bool metadataKnown = false;
        MessageId creationMessageId;

        explicit ForumTopicState(ChatTarget target)
            : target(target)
        {}

        bool isGeneral() const {
            return target.isForumTopic() &&
                   target.forumTopicId() == ForumTopicId::general();
        }
    };

    struct ForumTopicUpsertResult {
        const ForumTopicState *state;
        bool applied;

        ForumTopicUpsertResult(const ForumTopicState *state, bool applied)
            : state(state), applied(applied)
        {}
    };

    struct DisplayedMessageConversation {
        PurpleConversationType type = PURPLE_CONV_TYPE_UNKNOWN;
        std::string name;

        bool operator==(
            const DisplayedMessageConversation &other) const
        {
            return type == other.type && name == other.name;
        }
    };

    enum class DisplayedMessageLookupResult {
        Available,
        KnownConversationUnavailable,
        UnknownMessage,
    };

    enum class DisplayedMessageUpdateResult {
        Written,
        KnownConversationUnavailable,
        UnknownMessage,
    };

    enum class PendingSendLookupResult {
        NotFound,
        Found,
        Ambiguous,
    };

    struct PendingSendInfo {
        ChatTarget  target;
        MessageId   messageId;
        std::string tempFile;
    };

    struct {
        unsigned maxCaptionLength = 0;
        unsigned maxMessageLength = 0;
    } options;

    PurpleAccount *const  purpleAccount;
    TdTransceiver        &transceiver;
    TdAccountData(PurpleAccount *purpleAccount, TdTransceiver &transceiver)
    : purpleAccount(purpleAccount), transceiver(transceiver) {}
    ~TdAccountData();

    void updateUser(TdUserPtr user);
    void setUserStatus(UserId UserId, td::td_api::object_ptr<td::td_api::UserStatus> status);
    void updateSmallProfilePhoto(UserId userId, td::td_api::object_ptr<td::td_api::file> photo);
    void updateBasicGroup(TdGroupPtr group);
    void setBasicGroupInfoRequested(BasicGroupId groupId);
    bool isBasicGroupInfoRequested(BasicGroupId groupId);
    void updateBasicGroupInfo(BasicGroupId groupId, TdGroupInfoPtr groupInfo);
    void updateSupergroup(TdSupergroupPtr group);
    void setSupergroupInfoRequested(SupergroupId groupId);
    bool isSupergroupInfoRequested(SupergroupId groupId);
    void updateSupergroupInfo(SupergroupId groupId, TdSupergroupInfoPtr groupInfo);
    uint64_t getSupergroupMembersRevision(
        SupergroupId groupId) const;
    void reconcileSupergroupMembers(
        SupergroupId groupId, TdChatMembersPtr members,
        uint64_t snapshotRevision);
    const td::td_api::chatMember *updateSupergroupMember(
        SupergroupId groupId, TdChatMemberPtr member);
    void removeSupergroupMember(
        SupergroupId groupId, UserId userId);
    bool beginSupergroupProjection(
        SupergroupId groupId);
    uint64_t prepareSupergroupProjectionAttempt(
        SupergroupId groupId);
    bool isSupergroupProjectionAttemptCurrent(
        SupergroupId groupId, uint64_t epoch) const;
    void endSupergroupProjection(
        SupergroupId groupId);

    void addChat(TdChatPtr chat); // Updates existing chat if any
    void updateChatPosition(ChatId chatId, td::td_api::object_ptr<td::td_api::chatPosition> &&position);
    void updateChatTitle(ChatId chatId, const std::string &title);
    void updateSmallChatPhoto(ChatId chatId, td::td_api::object_ptr<td::td_api::file> photo);
    void setContacts(const td::td_api::users &users);
    void getContactsWithNoChat(std::vector<UserId> &userIds);
    void getChats(std::vector<const td::td_api::chat *> &chats) const;
    void deleteChat(ChatId id);
    void addExpectedChat(ChatTarget target);
    bool isExpectedChat(ChatTarget target) const;
    void removeExpectedChat(ChatTarget target);
    void getExpectedForumTopics(
        ChatId chatId, std::vector<ChatTarget> &targets) const;
    void addExpectedChat(ChatId id) {
        addExpectedChat(ChatTarget::chat(id));
    }
    bool isExpectedChat(ChatId id) const {
        return isExpectedChat(ChatTarget::chat(id));
    }
    void removeExpectedChat(ChatId id) {
        removeExpectedChat(ChatTarget::chat(id));
    }

    const td::td_api::chat       *getChat(ChatId chatId) const;
    int                           getPurpleChatId(ChatId tdChatId) const;
    int                           getPurpleChatId(ChatTarget target) const;
    ChatTarget                    getChatTargetByPurpleId(int32_t purpleChatId) const;
    const td::td_api::chat       *getChatByPurpleId(int32_t purpleChatId) const;
    const td::td_api::chat       *getPrivateChatByUserId(UserId userId) const;
    const td::td_api::user       *getUser(UserId userId) const;
    const td::td_api::user       *getUserByPhone(const char *phoneNumber) const;
    const td::td_api::user       *getUserByPrivateChat(const td::td_api::chat &chat);
    std::string                   getDisplayName(const td::td_api::user &user) const;
    std::string                   getDisplayName(UserId userId) const;
    void                          getUsersByDisplayName(const char *displayName,
                                                        std::vector<const td::td_api::user*> &users);

    const td::td_api::basicGroup *getBasicGroup(BasicGroupId groupId) const;
    const td::td_api::basicGroupFullInfo *getBasicGroupInfo(BasicGroupId groupId) const;
    const td::td_api::supergroup *getSupergroup(SupergroupId groupId) const;
    const td::td_api::supergroupFullInfo *getSupergroupInfo(SupergroupId groupId) const;
    const td::td_api::chatMembers*getSupergroupMembers(SupergroupId groupId) const;
    const td::td_api::chat       *getBasicGroupChatByGroup(BasicGroupId groupId) const;
    const td::td_api::chat       *getSupergroupChatByGroup(SupergroupId groupId) const;
    bool                          isGroupChatWithMembership(const td::td_api::chat &chat) const;

    const td::td_api::chat       *getChatBySecretChat(SecretChatId secretChatId);

    ForumTopicUpsertResult         upsertForumTopic(ChatTarget target,
                                                    const std::string &name,
                                                    bool closed,
                                                    bool hidden,
                                                    uint64_t generation);
    const ForumTopicState         *ensureForumTopicPlaceholder(
                                      ChatTarget target);
    void                          invalidateForumTopicMetadata(
                                      ChatTarget target,
                                      uint64_t generation);
    uint64_t                      reserveForumTopicGeneration();
    std::vector<ChatTarget>       reconcileForumTopics(
                                      ChatId chatId,
                                      const std::set<ChatTarget> &seenTargets,
                                      uint64_t generation);
    const ForumTopicState         *findForumTopic(ChatTarget target) const;
    void                          getForumTopics(
                                      ChatId chatId,
                                      std::vector<const ForumTopicState *> &topics) const;
    bool                          setForumTopicSaved(ChatTarget target, bool saved);
    int32_t                       activateForumTopic(ChatTarget target);
    int32_t                       prepareForumTopicForIncomingMessage(
                                      ChatTarget target);
    void                          deactivateForumTopic(ChatTarget target);

    template<typename ReqType, typename... ArgsType>
    void addPendingRequest(ArgsType... args)
    {
        m_requests.push_back(std::make_unique<ReqType>(args...));
    }
    template<typename ReqType>
    void addPendingRequest(uint64_t requestId, std::unique_ptr<ReqType> &&request)
    {
        m_requests.push_back(std::move(request));
        m_requests.back()->requestId = requestId;
    }
    template<typename ReqType>
    std::unique_ptr<ReqType> getPendingRequest(uint64_t requestId)
    {
        return std::unique_ptr<ReqType>(dynamic_cast<ReqType *>(getPendingRequestImpl(requestId).release()));
    }
    template<typename ReqType>
    ReqType *findPendingRequest(uint64_t requestId)
    {
        return dynamic_cast<ReqType *>(findPendingRequestImpl(requestId));
    }

    const ContactRequest *     findContactRequest(UserId userId);
    void                       addPendingSend(
                                      ChatTarget target,
                                      MessageId messageId,
                                      std::string tempFile);
    // On success, ownership of pending.tempFile transfers to the caller.
    PendingSendLookupResult    extractPendingSend(
                                      MessageId messageId,
                                      PendingSendInfo &pending);
    bool                       extractPendingSend(
                                      ChatId chatId,
                                      MessageId messageId,
                                      PendingSendInfo &pending);
    std::vector<uint64_t>      findDownloadRequestIds(
                                      int32_t fileId) const;
    void                       extractFileTransferRequests(std::vector<PurpleXfer *> &transfers);

    struct FileTransferInfo {
        int32_t     fileId = 0;
        ChatTarget  target;
        PurpleXfer *xfer = nullptr;
        ReceiveTransferKind receiveKind =
            ReceiveTransferKind::None;
        uint64_t requestId = 0;
    };

    void                       addFileTransfer(
                                      int32_t fileId,
                                      PurpleXfer *xfer,
                                      ChatTarget target,
                                      ReceiveTransferKind receiveKind,
                                      uint64_t requestId);
    bool                       associateFileTransferRequest(
                                      PurpleXfer *xfer,
                                      ReceiveTransferKind receiveKind,
                                      uint64_t requestId);
    bool                       getFileTransferInfo(
                                      PurpleXfer *xfer,
                                      FileTransferInfo &transfer) const;
    bool                       getFileTransferForRequest(
                                      uint64_t requestId,
                                      ReceiveTransferKind receiveKind,
                                      FileTransferInfo &transfer) const;
    bool                       extractFileTransferForRequest(
                                      uint64_t requestId,
                                      ReceiveTransferKind receiveKind,
                                      FileTransferInfo &transfer);
    bool                       getFileTransfer(
                                      int32_t fileId,
                                      PurpleXferType type,
                                      PurpleXfer *&xfer,
                                      ChatTarget &target);
    std::vector<FileTransferInfo> getFileTransfers(
                                      int32_t fileId,
                                      PurpleXferType type) const;
    std::vector<FileTransferInfo> extractFileTransfers(
                                      int32_t fileId,
                                      PurpleXferType type);
    bool                       hasFileTransfer(
                                      int32_t fileId,
                                      PurpleXferType type) const;
    bool                       getFileIdForTransfer(PurpleXfer *xfer, int &fileId);
    void                       removeFileTransfer(
                                      int32_t fileId,
                                      PurpleXfer *xfer);
    void                       removeAllFileTransfers(std::vector<PurpleXfer *> &transfers);
    bool                       hasPendingUploadRequests() const;

    void                       addSecretChat(td::td_api::object_ptr<td::td_api::secretChat> secretChat);
    const td::td_api::secretChat *getSecretChat(SecretChatId id);
    void                       deleteSecretChat(SecretChatId id);

    auto                       getBasicGroupsWithMember(UserId userId) ->
                               std::vector<std::pair<BasicGroupId, const td::td_api::basicGroupFullInfo *>>;
    std::vector<SupergroupId>  getSupergroupsWithMember(
                               UserId userId) const;

    bool                       hasActiveCall();
    void                       setActiveCall(int32_t id);
    int32_t                    getActiveCallId() const { return m_callId; }
    tgvoip::VoIPController    *getCallData();
    void                       removeActiveCall();

    PendingMessageQueue        pendingMessages;

    struct ReadReceiptConversation {
        PurpleConversationType type =
            PURPLE_CONV_TYPE_UNKNOWN;
        std::string            name;
    };

    void                       beginReadReceiptBatch();
    bool                       deferReadReceiptFlush(
                                      PurpleConversationType type,
                                      const std::string &name);
    bool                       finishReadReceiptBatch(
                                      std::vector<ReadReceiptConversation>
                                          &conversations);
    void                       extractPendingReadReceipts(
                                      ChatTarget target,
                                      std::vector<MessageId> &messageIds);
    void                       discardPendingReadReceipts(
                                      ChatId chatId,
                                      const std::vector<MessageId>
                                          &messageIds);
    void                       rememberMessageTarget(
                                      ChatTarget target,
                                      MessageId messageId);
    void                       replaceMessageId(
                                      ChatId oldChatId,
                                      MessageId oldMessageId,
                                      ChatId newChatId,
                                      MessageId newMessageId,
                                      ChatTarget newTarget);
    void                       rememberDisplayedMessage(
                                      ChatTarget target,
                                      MessageId messageId,
                                      PurpleConversation *conv,
                                      const std::string &sender,
                                      time_t timestamp,
                                      PurpleMessageFlags flags,
                                      bool queueReadReceipt);
    bool                       isForumSensitiveChat(
                                      ChatId chatId) const;
    bool                       shouldUseLegacyMessageUpdateFallback(
                                      ChatId chatId,
                                      MessageId messageId) const;
    DisplayedMessageLookupResult findDisplayedMessageConversation(
                                      ChatId chatId,
                                      MessageId messageId,
                                      DisplayedMessageConversation &conversation) const;
    DisplayedMessageUpdateResult showUpdatedMessage(
                                      ChatId chatId,
                                      MessageId messageId,
                                      const std::string &newText);
private:
    TdAccountData(const TdAccountData &other) = delete;
    TdAccountData &operator=(const TdAccountData &other) = delete;

    struct UserInfo {
        TdUserPtr   user;
        std::string displayName;
    };

    struct ChatInfo {
        int32_t   purpleId;
        TdChatPtr chat;

        ChatInfo() : purpleId(0), chat() {}
    };

    struct GroupInfo {
        TdGroupPtr     group;
        TdGroupInfoPtr fullInfo;
        bool           fullInfoRequested = false;
    };

    struct SupergroupInfo {
        TdSupergroupPtr     group;
        TdSupergroupInfoPtr fullInfo;
        TdChatMembersPtr    members;
        bool                fullInfoRequested = false;
        bool                hasEverBeenForum = false;
        // Per-user revisions retain removals as tombstones so an in-flight
        // member snapshot can be rebased onto newer live updates.
        uint64_t            membersRevision = 0;
        std::map<UserId, uint64_t> memberRevisions;
        uint64_t            projectionEpoch = 0;
        bool                projectionActive = false;
        bool                projectionPending = false;
    };

    struct MessageRouteInfo {
        ChatTarget             target;
        PurpleConversationType conversationType =
            PURPLE_CONV_TYPE_UNKNOWN;
        std::string            conversationName;
        std::string            sender;
        time_t                 timestamp = 0;
        PurpleMessageFlags     flags =
            static_cast<PurpleMessageFlags>(0);
        bool                   readReceiptQueued = false;
    };

    using ChatMap = std::map<ChatId, ChatInfo>;
    using UserMap = std::map<UserId, UserInfo>;
    using MessageRouteMap =
        std::map<MessageId, MessageRouteInfo>;
    UserMap                            m_userInfo;
    ChatMap                            m_chatInfo;
    std::map<BasicGroupId, GroupInfo>  m_groups;
    std::map<SupergroupId, SupergroupInfo>  m_supergroups;
    std::map<SecretChatId, SecretChatPtr>   m_secretChats;
    std::map<ChatTarget, ForumTopicState>    m_forumTopics;
    uint64_t                           m_forumTopicGeneration = 0;
    int32_t                            m_lastChatPurpleId = 0;

    // List of contacts for which private chat is not known yet.
    std::vector<UserId>                m_contactUserIdsNoChat;

    // Used to remember stuff during asynchronous communication when adding contact
    std::vector<ContactRequest>        m_addContactRequests;

    // Chats we want to libpurple-join when we get an updateNewChat about them
    std::set<ChatTarget>               m_expectedChats;

    std::vector<std::unique_ptr<PendingRequest>> m_requests;

    // Pending sends retain their original room independently of any fields
    // TDLib changes in the final message. Inline-image paths share this
    // ownership record so they are removed on completion or teardown.
    std::vector<PendingSendInfo>       m_pendingSends;

    // Currently active file transfers for which PurpleXfer is used
    std::vector<FileTransferInfo>      m_fileTransfers;

    // Voice call data
    std::unique_ptr<tgvoip::VoIPController> m_callData;
    int32_t                                 m_callId;

    std::unique_ptr<PendingRequest> getPendingRequestImpl(uint64_t requestId);
    PendingRequest *                findPendingRequestImpl(uint64_t requestId);
    ForumTopicState *               findForumTopicMutable(ChatTarget target);
    bool                            tombstoneForumTopic(ForumTopicState &topic,
                                                        uint64_t generation);
    int32_t                         allocatePurpleChatId();
    int32_t                         allocateForumTopicPurpleId(ForumTopicState &topic);
    bool                            isForumChat(ChatId chatId) const;
    bool                            hasPendingReadReceipt(
                                        ChatTarget target,
                                        MessageId messageId) const;
    void                            pruneMessageRoutes(
                                        ChatId chatId,
                                        MessageId protectedMessageId =
                                            MessageId());

    // Successfully displayed messages awaiting receipt delivery, grouped per exact room.
    std::map<ChatTarget, std::vector<MessageId>> m_pendingReadReceipts;
    unsigned m_readReceiptBatchDepth = 0;
    std::vector<ReadReceiptConversation>
        m_deferredReadReceiptConversations;

    std::map<ChatId, MessageRouteMap>
        m_messageRoutes;
};

#endif
