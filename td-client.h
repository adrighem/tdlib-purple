#ifndef _TD_CLIENT_H
#define _TD_CLIENT_H

#include "account-data.h"
#include "client-utils.h"
#include "forum-topics.h"
#include "telegram-application-credentials.h"
#include <td/telegram/Log.h>
#include <purple.h>
#include <map>
#include <memory>
#include <set>

enum class BasicGroupMembership: uint8_t {
    Invalid,
    Creator,
    NonCreator,
};

class PurpleTdClient {
public:
    PurpleTdClient(
        PurpleAccount *acct,
        ITransceiverBackend *testBackend,
        const TdlibPurpleApplicationCredentials &applicationCredentials);
    ~PurpleTdClient();

    static void disableTdlibLogging();
    static void setTdlibFatalErrorCallback(td::Log::FatalErrorCallbackPtr callback);
    static std::string getBaseDatabasePath();
    int  sendMessage(const char *buddyName, const char *message);
    void sendTyping(const char *buddyName, bool isTyping);
    void sendReadReceipts(PurpleConversation *conversation);
    void setOnlineStatus(bool online);
    void setBuddyIcon(PurpleStoredImage *img);

    void addContact(const std::string &purpleName, const std::string &alias, const std::string &groupName);
    void renameContact(const char *buddyName, const char *newAlias);
    void removeContactAndPrivateChat(const std::string &buddyName);
    void getUsers(const char *username, std::vector<const td::td_api::user *> &users);

    bool joinChat(const char *chatName);
    void joinChatByInviteLink(const char *inviteLink);
    void joinChatByGroupName(const char *joinString, const char *groupName);
    void createGroup(const char *name, int type, const std::vector<std::string> &basicGroupMembers);
    BasicGroupMembership getBasicGroupMembership(const char *purpleChatName);
    void leaveGroup(const std::string &purpleChatName, bool deleteSupergroup);
    void closeConversation(const char *conversationName);
    void ensureForumTopicMetadata(ChatTarget target);
    bool satisfyForumTopicJoinIfOpen(ChatTarget target);
    int  sendGroupMessage(int purpleChatId, const char *message);
    void setGroupDescription(int purpleChatId, const char *description);
    void kickUserFromChat(PurpleConversation *conv, const char *name);
    void addUserToChat(int purpleChatId, const char *name);
    void showInviteLink(const std::string &purpleChatName);
    void getGroupChatList(PurpleRoomlist *roomlist);

    void setTwoFactorAuth(const char *oldPassword, const char *newPassword, const char *hint,
                        const char *email);

    ChatTarget resolveFileChatTarget(int purpleChatId) const;
    void sendFileToChat(PurpleXfer *xfer, const char *purpleName,
                        PurpleConversationType type, ChatTarget target);
    void cancelUpload(PurpleXfer *xfer);
    bool canSendFileToUser(const char *purpleName);
    bool canSendFileToChat(int purpleChatId);

    bool startVoiceCall(const char *buddyName);
    bool terminateCall(PurpleConversation *conv);

    void createSecretChat(const char *buddyName);
private:
    using TdObjectPtr   = td::td_api::object_ptr<td::td_api::Object>;
    using ResponseCb    = void (PurpleTdClient::*)(uint64_t requestId, TdObjectPtr object);
    enum class ForumTopicJoinIntent : uint8_t {
        UserRequest,
        PersistentRejoin,
    };
    struct PendingForumTopicJoin {
        ForumTopicJoinIntent intent;
        uint64_t serial;
        uint64_t liveMessageGenerationAtStart;

        PendingForumTopicJoin(
            ForumTopicJoinIntent intent, uint64_t serial,
            uint64_t liveMessageGenerationAtStart)
            : intent(intent),
              serial(serial),
              liveMessageGenerationAtStart(
                  liveMessageGenerationAtStart)
        {}
    };
    struct LifetimeState {
        bool alive = true;
    };

    void       processUpdate(td::td_api::Object &object);
    void       processAuthorizationState(td::td_api::AuthorizationState &authState);

    // Login sequence start
    bool       addProxy();
    void       addProxyResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       getProxiesResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       removeOldProxies();
    void       sendTdlibParameters();
    void       sendPhoneNumber();
    void       requestAuthEmail();
    void       requestAuthEmailCode();
    void       requestAuthCode(const td::td_api::authenticationCodeInfo *authCodeInfo);
    void       requestPassword(const td::td_api::authorizationStateWaitPassword &pwInfo);
    void       registerUser();
    static void requestCodeEntered(PurpleTdClient *self, const gchar *code);
    static void requestCodeCancelled(PurpleTdClient *self);
    static void requestAuthEmailEntered(PurpleTdClient *self, const gchar *email);
    static void requestAuthEmailCancelled(PurpleTdClient *self);
    static void requestAuthEmailCodeEntered(PurpleTdClient *self, const gchar *code);
    static void requestAuthEmailCodeCancelled(PurpleTdClient *self);
    static void passwordEntered(PurpleTdClient *self, const gchar *code);
    static void passwordCancelled(PurpleTdClient *self);
    static void displayNameEntered(PurpleTdClient *self, const gchar *name);
    static void displayNameCancelled(PurpleTdClient *self);
    void       authResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       notifyAuthError(const td::td_api::object_ptr<td::td_api::Object> &response);
    void       setPurpleConnectionInProgress();
    void       onLoggedIn();
    void       getContactsResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       getChatsResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       requestMissingPrivateChats();
    void       loginCreatePrivateChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    // List of chats is requested after connection is ready, and when response is received,
    // then we report to libpurple that we are connected
    void       onChatListReady();
    // Login sequence end

    void       onIncomingMessage(td::td_api::object_ptr<td::td_api::message> message);
    void       unreadReactionsMessageResponse(uint64_t requestId,
                                              td::td_api::object_ptr<td::td_api::Object> object);
    void       updateChatLastMessage(td::td_api::updateChatLastMessage &lastMessage);
    void       updateVisibleChatMemberList(td::td_api::updateChatMember &update);
    void       setProfilePhotoResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);

    void       updateUserStatus(UserId userId, td::td_api::object_ptr<td::td_api::UserStatus> status);
    void       updateUser(td::td_api::object_ptr<td::td_api::user> user);
    void       downloadProfilePhoto(const td::td_api::user &user);
    void       avatarDownloadResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       updateGroup(td::td_api::object_ptr<td::td_api::basicGroup> group);
    void       updateSupergroup(td::td_api::object_ptr<td::td_api::supergroup> group);
    bool       resolveDeferredGroupChat(ChatId chatId);
    void       resolveDeferredGroupChats();
    void       markForumRoomListsReadyIfPossible();
    void       updateChat(const td::td_api::chat *chat);
    void       updateUserInfo(const td::td_api::user &user, const td::td_api::chat *privateChat);
    void       downloadChatPhoto(const td::td_api::chat &chat);
    void       requestBasicGroupFullInfo(BasicGroupId groupId);
    void       requestSupergroupFullInfo(SupergroupId groupId);
    void       groupInfoResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       supergroupInfoResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       supergroupMembersResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       supergroupAdministratorsResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       updateGroupFull(BasicGroupId groupId, td::td_api::object_ptr<td::td_api::basicGroupFullInfo> groupInfo);
    void       updateSupergroupFull(SupergroupId groupId, td::td_api::object_ptr<td::td_api::supergroupFullInfo> groupInfo);

    void       addContactById(UserId userId, const std::string &phoneNumber, const std::string &alias,
                              const std::string &groupName);
    void       addChat(td::td_api::object_ptr<td::td_api::chat> chat);
    void       handleUserChatAction(const td::td_api::updateChatAction &updateChatAction);
    void       showUserChatAction(UserId userId, bool isTyping);
    void       addBuddySearchChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       importContactResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       addContactResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       addContactCreatePrivateChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       notifyFailedContact(const std::string &errorMessage);
    bool       showMessageLinkedUpdate(
                   ChatId chatId, MessageId messageId,
                   const std::string &message,
                   PurpleMessageFlags extraFlags =
                       static_cast<PurpleMessageFlags>(0));
    bool       showDeletedMessageUpdate(
                   ChatId chatId,
                   const std::vector<td::td_api::int53> &messageIds);
    bool       showTargetNotification(
                   ChatTarget target,
                   const std::string &message,
                   time_t timestamp = static_cast<time_t>(-1),
                   PurpleMessageFlags extraFlags =
                       static_cast<PurpleMessageFlags>(0));
    ChatTarget replacePendingMessageId(
                   const td::td_api::message &message,
                   MessageId oldMessageId,
                   ChatId oldChatId);
    bool       joinForumTopic(ChatTarget target);
    void       openPreparedForumTopicForPendingJoin(
                   ChatTarget target);
    void       completeForumTopicJoin(
                   const ForumTopicLookupResult &result,
                   uint64_t joinSerial);
    void       failForumTopicJoin(ChatTarget target);
    void       failForumTopicJoins(ChatId chatId);
    void       pruneAbandonedForumTopicJoins(ChatId chatId);
    void       retryExpectedForumTopicJoins(ChatId chatId);
    void       projectForumTopic(ChatTarget target);
    void       projectForumTopics(ChatId chatId);
    void       suspendForumTopics(ChatId chatId);
    void       joinChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       joinGroupSearchChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       deleteSupergroupResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       setGroupDescriptionResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       chatActionResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);

    void       onAnimatedStickerConverted(AccountThread *arg);
    void       sendMessageCreatePrivateChatResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       uploadResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       deferOrCancelUpload(int32_t fileId);
    void       flushDeferredUploadCancels();
    void       sendUploadCancel(int32_t fileId);

    void       sendMessageResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void       removeTempFile(const std::string &path);

    void        setTwoFactorAuthResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);
    void        requestRecoveryEmailConfirmation(const std::string &emailInfo);
    static void verifyRecoveryEmail(PurpleTdClient *self, const char *code);
    void        verifyRecoveryEmailResponse(uint64_t requestId, td::td_api::object_ptr<td::td_api::Object> object);

    PurpleAccount        *m_account;
    TdlibPurpleApplicationCredentials m_applicationCredentials;
    TdTransceiver         m_transceiver;
    TdAccountData         m_data;
    std::shared_ptr<LifetimeState> m_lifetime;
    std::unique_ptr<ForumTopicsAdapter> m_forumTopics;
    int32_t               m_lastAuthState = 0;
    std::vector<UserId>   m_usersForNewPrivateChats;
    std::set<ChatId>      m_deferredGroupChats;
    std::set<int32_t>     m_deferredUploadCancels;
    std::map<ChatTarget, PendingForumTopicJoin> m_pendingForumTopicJoins;
    uint64_t              m_lastForumTopicJoinSerial = 0;
    bool                  m_chatListReady = false;
    bool                  m_isProxyAdded = false;
    td::td_api::object_ptr<td::td_api::addedProxy>   m_addedProxy;
    td::td_api::object_ptr<td::td_api::addedProxies> m_proxies;

    struct ChatGap {
        ChatId    chatId;
        MessageId lastMessage;
    };
    std::vector<ChatGap> m_chatGaps;
};

#endif
