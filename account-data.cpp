#include "account-data.h"
#include "client-utils.h"
#include "config.h"
#include "format.h"
#include <purple.h>
#include <algorithm>
#include <cstdio>
#include <ctype.h>
#include <limits>

// Unknown forum updates fail closed, so evicting old routes cannot misroute them.
static constexpr size_t MaxMessageRoutesPerChat = 4096;

SendMessageRequest::~SendMessageRequest()
{
    if (!tempFile.empty())
        std::remove(tempFile.c_str());
}

TdAccountData::~TdAccountData()
{
    for (const PendingSendInfo &pending : m_pendingSends) {
        if (!pending.tempFile.empty())
            std::remove(pending.tempFile.c_str());
    }
}

static bool isCanonicalPhoneNumber(const char *s)
{
    if (*s == '\0')
        return false;

    for (const char *c = s; *c; c++)
        if (!isdigit(static_cast<unsigned char>(*c)))
            return false;

    return true;
}

bool isPhoneNumber(const char *s)
{
    if (*s == '+') s++;
    return isCanonicalPhoneNumber(s);
}

const char *getCanonicalPhoneNumber(const char *s)
{
    if (*s == '+')
        return s+1;
    else
        return s;
}

UserId purpleBuddyNameToUserId(const char *s)
{
    if (strncmp(s, "id", 2))
        return UserId::invalid;
    return UserId::fromString(s+2);
}

SecretChatId purpleBuddyNameToSecretChatId(const char *s)
{
    if (strncmp(s, "secret", 2))
        return SecretChatId::invalid;
    return SecretChatId::fromString(s+6);
}

static bool isPhoneEqual(const std::string &n1, const std::string &n2)
{
    const char *s1 = n1.c_str();
    const char *s2 = n2.c_str();
    if (*s1 == '+') s1++;
    if (*s2 == '+') s2++;
    return !strcmp(s1, s2);
}

bool isPrivateChat(const td::td_api::chat &chat)
{
    return getUserIdByPrivateChat(chat).valid();
}

UserId getUserIdByPrivateChat(const td::td_api::chat &chat)
{
    if (chat.type_ && (chat.type_->get_id() == td::td_api::chatTypePrivate::ID)) {
        const td::td_api::chatTypePrivate &privType = static_cast<const td::td_api::chatTypePrivate &>(*chat.type_);
        return getUserId(privType);
    }
    return UserId::invalid;
}

bool isChatInContactList(const td::td_api::chat &chat, const td::td_api::user *privateChatUser)
{
    return !chat.positions_.empty() || (privateChatUser && privateChatUser->is_contact_);
}

BasicGroupId getBasicGroupId(const td::td_api::chat &chat)
{
    if (chat.type_ && (chat.type_->get_id() == td::td_api::chatTypeBasicGroup::ID))
        return getBasicGroupId(static_cast<const td::td_api::chatTypeBasicGroup &>(*chat.type_));

    return BasicGroupId::invalid;
}

SupergroupId getSupergroupId(const td::td_api::chat &chat)
{
    if (chat.type_ && (chat.type_->get_id() == td::td_api::chatTypeSupergroup::ID))
        return getSupergroupId(static_cast<const td::td_api::chatTypeSupergroup &>(*chat.type_));

    return SupergroupId::invalid;
}

SecretChatId getSecretChatId(const td::td_api::chat &chat)
{
    if (chat.type_ && (chat.type_->get_id() == td::td_api::chatTypeSecret::ID))
        return getSecretChatId(static_cast<const td::td_api::chatTypeSecret&>(*chat.type_));

    return SecretChatId::invalid;
}

bool isGroupMember(const td::td_api::object_ptr<td::td_api::ChatMemberStatus> &status)
{
    if (!status)
        return false;
    else if ((status->get_id() == td::td_api::chatMemberStatusLeft::ID) ||
             (status->get_id() == td::td_api::chatMemberStatusBanned::ID))
        return false;
    else if (status->get_id() == td::td_api::chatMemberStatusRestricted::ID)
        return static_cast<const td::td_api::chatMemberStatusRestricted &>(*status).is_member_;
    else if (status->get_id() == td::td_api::chatMemberStatusCreator::ID)
        return static_cast<const td::td_api::chatMemberStatusCreator &>(*status).is_member_;
    else if (status->get_id() == td::td_api::chatMemberStatusAdministrator::ID)
        return true;
    else if (status->get_id() == td::td_api::chatMemberStatusMember::ID)
        return true;

    return false;
}

bool isSameUser(const td::td_api::MessageSender &member1, const td::td_api::MessageSender &member2)
{
    if ((member1.get_id() == td::td_api::messageSenderUser::ID) &&
        (member2.get_id() == td::td_api::messageSenderUser::ID))
    {
        const td::td_api::messageSenderUser &user1 = static_cast<const td::td_api::messageSenderUser &>(member1);
        const td::td_api::messageSenderUser &user2 = static_cast<const td::td_api::messageSenderUser &>(member2);
        return (user1.user_id_ == user2.user_id_);
    }

    return false;
}

static std::string makeDisplayName(const td::td_api::user &user)
{
    std::string result = makeBasicDisplayName(user);

    // If some sneaky user sets their name equal to someone else's libpurple username, or to our
    // phone number which is libpurple account name, make sure display name is different, because
    // of how it is used for group chat members
    if ((purpleBuddyNameToUserId(result.c_str()).valid()) || isPhoneNumber(result.c_str()))
        result += ' ';

    return result;
}

auto PendingMessageQueue::getChatQueue(ChatId chatId) -> std::vector<ChatQueue>::iterator 
{
    return std::find_if(m_queues.begin(), m_queues.end(), [chatId](const ChatQueue &queue) {
        return (queue.chatId == chatId);
    });
}

PendingMessageQueue::Message &PendingMessageQueue::addMessage(ChatQueue &queue, MessageAction action)
{
    if (action == MessageAction::Append) {
        queue.messages.emplace_back();
        return queue.messages.back();
    } else {
        queue.messages.emplace_front();
        return queue.messages.front();
    }
}

IncomingMessage &PendingMessageQueue::addPendingMessage(IncomingMessage &&message,
    MessageAction action)
{
    if (!message.message) return message;

    ChatId     chatId  = getChatId(*message.message);
    auto       queueIt = getChatQueue(chatId);
    ChatQueue *queue;
    purple_debug_misc(config::pluginId,"MessageQueue: chat %" G_GINT64_FORMAT ": "
                      "adding pending message %" G_GINT64_FORMAT " (not ready)\n",
                      chatId.value(), message.message->id_);

    if (queueIt != m_queues.end())
        queue = &*queueIt;
    else {
        m_queues.emplace_back();
        m_queues.back().chatId = chatId;
        queue = &m_queues.back();
    }

    Message &newEntry = addMessage(*queue, action);
    newEntry.ready = false;
    newEntry.message = std::move(message);
    return newEntry.message;
}

void PendingMessageQueue::extractReadyMessages(
    std::vector<ChatQueue>::iterator pQueue,
    std::vector<IncomingMessage> &readyMessages)
{
    std::list<Message>::iterator pReady;
    for (pReady = pQueue->messages.begin(); pReady != pQueue->messages.end(); ++pReady) {
        if (!pReady->ready) break;
        purple_debug_misc(config::pluginId,"MessageQueue: chat %" G_GINT64_FORMAT ": "
                            "showing message %" G_GINT64_FORMAT "\n",
                            pQueue->chatId.value(), getId(*pReady->message.message).value());
        readyMessages.push_back(std::move(pReady->message));
    }

    pQueue->messages.erase(pQueue->messages.begin(), pReady);
    if (pQueue->messages.empty()) {
        m_queues.erase(pQueue);
        pQueue = m_queues.end();
    }
}

void PendingMessageQueue::setMessageReady(ChatId chatId, MessageId messageId,
                                          std::vector<IncomingMessage> &readyMessages)
{
    readyMessages.clear();

    auto pQueue = getChatQueue(chatId);
    if (pQueue == m_queues.end()) return;

    purple_debug_misc(config::pluginId,"MessageQueue: chat %" G_GINT64_FORMAT ": "
                      "message %" G_GINT64_FORMAT " now ready\n",
                      chatId.value(), messageId.value());

    auto it = std::find_if(pQueue->messages.begin(), pQueue->messages.end(), [messageId](const Message &m) {
        return (getId(*m.message.message) == messageId);
    });
    if (it == pQueue->messages.end()) return;

    it->ready = true;
    if (pQueue->ready && (it == pQueue->messages.begin()))
        extractReadyMessages(pQueue, readyMessages);
}

IncomingMessage PendingMessageQueue::addReadyMessage(IncomingMessage &&message,
    MessageAction action)
{
    if (!message.message) return IncomingMessage();

    ChatId chatId  = getChatId(*message.message);
    auto   queueIt = getChatQueue(chatId);
    if (queueIt == m_queues.end())
        return std::move(message);

    purple_debug_misc(config::pluginId,"MessageQueue: chat %" G_GINT64_FORMAT ": "
                      "adding pending message %" G_GINT64_FORMAT " (ready)\n",
                      chatId.value(), message.message->id_);

    Message &newEntry = addMessage(*queueIt, action);
    newEntry.ready = true;
    newEntry.message = std::move(message);

    return IncomingMessage();
}

IncomingMessage *PendingMessageQueue::findPendingMessage(ChatId chatId, MessageId messageId)
{
    auto queueIt = getChatQueue(chatId);
    if (queueIt == m_queues.end()) return nullptr;
    ChatQueue *queue = &*queueIt;

    auto it = std::find_if(queue->messages.begin(), queue->messages.end(), [messageId](const Message &m) {
        return (getId(*m.message.message) == messageId);
    });
    return (it != queue->messages.end()) ? &it->message : nullptr;
}

void PendingMessageQueue::flush(std::vector<IncomingMessage> &messages)
{
    messages.clear();
    for (ChatQueue &queue: m_queues)
        for (Message &message: queue.messages)
            messages.push_back(std::move(message.message));
    m_queues.clear();
}

void PendingMessageQueue::setChatNotReady(ChatId chatId)
{
    auto pQueue = getChatQueue(chatId);
    if (pQueue != m_queues.end())
        pQueue->ready = false;
    else {
        m_queues.emplace_back();
        m_queues.back().chatId = chatId;
        m_queues.back().ready = false;
    }
}

void PendingMessageQueue::setChatReady(ChatId chatId, std::vector<IncomingMessage>& readyMessages)
{
    readyMessages.clear();
    auto pQueue = getChatQueue(chatId);
    if (pQueue == m_queues.end()) return;

    pQueue->ready = true;
    extractReadyMessages(pQueue, readyMessages);
}

bool PendingMessageQueue::isChatReady(ChatId chatId)
{
    auto pQueue = getChatQueue(chatId);
    if (pQueue != m_queues.end())
        return pQueue->ready;
    else
        return true;
}

void TdAccountData::updateUser(TdUserPtr userPtr)
{
    const td::td_api::user *user = userPtr.get();
    if (user) {
        UserId   userId = getId(*user);
        auto     it     = m_userInfo.find(userId);

        if (it == m_userInfo.end()) {
            auto ret = m_userInfo.emplace(std::make_pair(userId, UserInfo()));
            it = ret.first;
        }

        UserInfo &entry = it->second;
        entry.user = std::move(userPtr);
        entry.displayName = makeDisplayName(*user);
        for (unsigned n = 0; n != UINT32_MAX; n++) {
            std::string displayName = entry.displayName;
            if (n != 0) {
                displayName += " #";
                displayName += std::to_string(n);
            }

            std::vector<const td::td_api::user *> existingUsers;
            getUsersByDisplayName(displayName.c_str(), existingUsers);
            if (std::none_of(existingUsers.begin(), existingUsers.end(),
                                [user](const td::td_api::user *otherUser) {
                                    return (otherUser != user);
                                }))
            {
                entry.displayName = std::move(displayName);
                break;
            }
        }
    }
}

void TdAccountData::setUserStatus(UserId userId, td::td_api::object_ptr<td::td_api::UserStatus> status)
{
    auto it = m_userInfo.find(userId);
    if (it != m_userInfo.end())
        it->second.user->status_ = std::move(status);
}

void TdAccountData::updateSmallProfilePhoto(UserId userId, td::td_api::object_ptr<td::td_api::file> photo)
{
    auto it = m_userInfo.find(userId);
    if (it != m_userInfo.end()) {
        td::td_api::user &user = *it->second.user;
        if (user.profile_photo_)
            user.profile_photo_->small_ = std::move(photo);
    }
}

void TdAccountData::updateBasicGroup(TdGroupPtr group)
{
    if (group)
        m_groups[getId(*group)].group = std::move(group);
}

void TdAccountData::setBasicGroupInfoRequested(BasicGroupId groupId)
{
    auto it = m_groups.find(groupId);
    if (it != m_groups.end())
        it->second.fullInfoRequested = true;
}

bool TdAccountData::isBasicGroupInfoRequested(BasicGroupId groupId)
{
    auto it = m_groups.find(groupId);
    if (it != m_groups.end())
        return it->second.fullInfoRequested;
    return false;
}

void TdAccountData::updateBasicGroupInfo(BasicGroupId groupId, TdGroupInfoPtr groupInfo)
{
    if (groupInfo)
        m_groups[groupId].fullInfo = std::move(groupInfo);
}

void TdAccountData::updateSupergroup(TdSupergroupPtr group)
{
    if (!group)
        return;

    SupergroupInfo &info = m_supergroups[getId(*group)];
    info.hasEverBeenForum =
        info.hasEverBeenForum || group->is_forum_;
    info.group = std::move(group);
}

void TdAccountData::setSupergroupInfoRequested(SupergroupId groupId)
{
    auto it = m_supergroups.find(groupId);
    if (it != m_supergroups.end())
        it->second.fullInfoRequested = true;
}

bool TdAccountData::isSupergroupInfoRequested(SupergroupId groupId)
{
    auto it = m_supergroups.find(groupId);
    if (it != m_supergroups.end())
        return it->second.fullInfoRequested;
    return false;
}

void TdAccountData::updateSupergroupInfo(SupergroupId groupId, TdSupergroupInfoPtr groupInfo)
{
    if (groupInfo)
        m_supergroups[groupId].fullInfo = std::move(groupInfo);
}

void TdAccountData::updateSupergroupMembers(SupergroupId groupId, TdChatMembersPtr members)
{
    if (members)
        m_supergroups[groupId].members = std::move(members);
}

void TdAccountData::addChat(TdChatPtr chat)
{
    if (!chat)
        return;

    const ChatId chatId = getId(*chat);

    if (chat->type_->get_id() == td::td_api::chatTypePrivate::ID) {
        const td::td_api::chatTypePrivate &privType = static_cast<const td::td_api::chatTypePrivate &>(*chat->type_);
        auto pContact = std::find(m_contactUserIdsNoChat.begin(), m_contactUserIdsNoChat.end(),
                                  getUserId(privType));
        if (pContact != m_contactUserIdsNoChat.end()) {
            purpleDebug("Private chat (id {}) now known for user {}", {
                std::to_string(chat->id_), std::to_string(privType.user_id_)
            });
            m_contactUserIdsNoChat.erase(pContact);
        }
    }

    auto it = m_chatInfo.find(chatId);
    if (it != m_chatInfo.end())
        it->second.chat = std::move(chat);
    else {
        auto entry = m_chatInfo.emplace(chatId, ChatInfo());
        entry.first->second.chat     = std::move(chat);
        entry.first->second.purpleId = allocatePurpleChatId();
    }

    for (auto &entry : m_forumTopics) {
        ForumTopicState &topic = entry.second;
        if (topic.target.chatId() == chatId && !topic.deleted &&
            (topic.saved || topic.active)) {
            allocateForumTopicPurpleId(topic);
        }
    }
}

void TdAccountData::updateChatPosition(ChatId chatId, td::td_api::object_ptr<td::td_api::chatPosition> &&position)
{
    auto it = m_chatInfo.find(chatId);
    if (position && position->list_ && (it != m_chatInfo.end())) {
        auto listId = position->list_->get_id();
        td::td_api::chat &chat = *it->second.chat;
        if (position->order_ == 0) {
            purpleDebug("Removing chat {} from list {}", {std::to_string(chatId.value()), std::to_string(listId)});
            chat.positions_.erase(
                std::remove_if(chat.positions_.begin(), chat.positions_.end(),
                               [listId](const td::td_api::object_ptr<td::td_api::chatPosition> &chatPos) {
                                   return chatPos && (chatPos->list_->get_id() == listId);
                               }),
                chat.positions_.end());
        } else {
            auto pExisting = std::find_if(chat.positions_.begin(), chat.positions_.end(),
                                          [listId](const td::td_api::object_ptr<td::td_api::chatPosition> &chatPos) {
                                              return chatPos && (chatPos->list_->get_id() == listId);
                                          });
            if (pExisting != chat.positions_.end()) {
                purpleDebug("Changing chat {}, list {} order to {}",
                            {std::to_string(chatId.value()), std::to_string(listId), std::to_string(position->order_)});
                *pExisting = std::move(position);
            } else {
                purpleDebug("Adding chat {} to list {}", {std::to_string(chatId.value()), std::to_string(listId)});
                chat.positions_.push_back(std::move(position));
            }
        }
    }
}

void TdAccountData::updateChatTitle(ChatId chatId, const std::string &title)
{
    auto it = m_chatInfo.find(chatId);
    if (it != m_chatInfo.end())
        it->second.chat->title_ = title;
}

void TdAccountData::updateSmallChatPhoto(ChatId chatId, td::td_api::object_ptr<td::td_api::file> photo)
{
    auto it = m_chatInfo.find(chatId);
    if (it != m_chatInfo.end()) {
        td::td_api::chat &chat = *it->second.chat;
        if (chat.photo_)
            chat.photo_->small_ = std::move(photo);
    }
}

void TdAccountData::setContacts(const td::td_api::users &users)
{
    for (unsigned i = 0; i < users.user_ids_.size(); i++) {
        UserId userId = getUserId(users, i);
        if (getPrivateChatByUserId(userId) == nullptr) {
            purpleDebug("Private chat not yet known for user {}", userId.value());
            m_contactUserIdsNoChat.push_back(userId);
        }
    }
}

void TdAccountData::getContactsWithNoChat(std::vector<UserId> &userIds)
{
    userIds = m_contactUserIdsNoChat;
}

const td::td_api::chat *TdAccountData::getChat(ChatId chatId) const
{
    auto pChatInfo = m_chatInfo.find(chatId);
    if (pChatInfo == m_chatInfo.end())
        return nullptr;
    else
        return pChatInfo->second.chat.get();
}

int TdAccountData::getPurpleChatId(ChatId tdChatId) const
{
    auto pChatInfo = m_chatInfo.find(tdChatId);
    if (pChatInfo == m_chatInfo.end())
        return 0;
    else
        return pChatInfo->second.purpleId;
}

int TdAccountData::getPurpleChatId(ChatTarget target) const
{
    if (!target.valid())
        return 0;
    if (!target.isForumTopic() ||
        target.forumTopicId() == ForumTopicId::general()) {
        return getPurpleChatId(target.chatId());
    }

    const ForumTopicState *topic = findForumTopic(target);
    return topic ? topic->purpleId : 0;
}

int32_t TdAccountData::allocatePurpleChatId()
{
    if (m_lastChatPurpleId == std::numeric_limits<int32_t>::max())
        return 0;
    return ++m_lastChatPurpleId;
}

int32_t TdAccountData::allocateForumTopicPurpleId(ForumTopicState &topic)
{
    if (topic.deleted)
        return topic.purpleId;
    if (topic.isGeneral())
        return getPurpleChatId(topic.target.chatId());
    if (topic.purpleId == 0 && getChat(topic.target.chatId()))
        topic.purpleId = allocatePurpleChatId();
    return topic.purpleId;
}

bool TdAccountData::isForumChat(ChatId chatId) const
{
    const td::td_api::chat *chat = getChat(chatId);
    if (!chat || !chat->type_ ||
        chat->type_->get_id() !=
            td::td_api::chatTypeSupergroup::ID) {
        return false;
    }

    const td::td_api::chatTypeSupergroup &supergroupType =
        static_cast<const td::td_api::chatTypeSupergroup &>(
            *chat->type_);
    if (supergroupType.is_channel_)
        return false;

    const SupergroupId groupId = getSupergroupId(*chat);
    const td::td_api::supergroup *group = getSupergroup(groupId);
    return group && group->is_forum_;
}

ChatTarget TdAccountData::getChatTargetByPurpleId(int32_t purpleChatId) const
{
    if (purpleChatId <= 0)
        return ChatTarget();

    auto topic = std::find_if(
        m_forumTopics.begin(), m_forumTopics.end(),
        [purpleChatId](const std::map<ChatTarget, ForumTopicState>::value_type &entry) {
            return !entry.second.isGeneral() &&
                   entry.second.purpleId == purpleChatId;
        });
    if (topic != m_forumTopics.end())
        return topic->first;

    auto chat = std::find_if(
        m_chatInfo.begin(), m_chatInfo.end(),
        [purpleChatId](const ChatMap::value_type &entry) {
            return entry.second.purpleId == purpleChatId;
        });
    if (chat != m_chatInfo.end()) {
        if (isForumChat(chat->first)) {
            return ChatTarget::forumTopic(
                chat->first, ForumTopicId::general());
        }
        return ChatTarget::chat(chat->first);
    }

    return ChatTarget();
}

const td::td_api::chat *TdAccountData::getChatByPurpleId(int32_t purpleChatId) const
{
    const ChatTarget target = getChatTargetByPurpleId(purpleChatId);
    return target.valid() ? getChat(target.chatId()) : nullptr;
}

static bool isPrivateChat(const td::td_api::chat &chat, UserId userId)
{
    if (chat.type_->get_id() == td::td_api::chatTypePrivate::ID) {
        const td::td_api::chatTypePrivate &privType = static_cast<const td::td_api::chatTypePrivate &>(*chat.type_);
        return (getUserId(privType) == userId);
    }
    return false;
}

const td::td_api::chat *TdAccountData::getPrivateChatByUserId(UserId userId) const
{
    auto pChatInfo = std::find_if(m_chatInfo.begin(), m_chatInfo.end(),
                                  [userId](const ChatMap::value_type &entry) {
                                      return isPrivateChat(*entry.second.chat, userId);
                                  });
    if (pChatInfo == m_chatInfo.end())
        return nullptr;
    else
        return pChatInfo->second.chat.get();
}

const td::td_api::user *TdAccountData::getUser(UserId userId) const
{
    auto pUser = m_userInfo.find(userId);
    if (pUser == m_userInfo.end())
        return nullptr;
    else
        return pUser->second.user.get();
}

const td::td_api::user *TdAccountData::getUserByPhone(const char *phoneNumber) const
{
    purple_debug_misc(config::pluginId, "Searching for user by phone: %s\n", phoneNumber);
    auto pUser = std::find_if(m_userInfo.begin(), m_userInfo.end(),
                              [phoneNumber](const UserMap::value_type &entry) {
                                  purple_debug_misc(config::pluginId, "Checking user %" G_GINT64_FORMAT " phone: %s\n", entry.first.value(), entry.second.user->phone_number_.c_str());
                                  return isPhoneEqual(entry.second.user->phone_number_, phoneNumber);
                              });
    if (pUser == m_userInfo.end())
        return nullptr;
    else
        return pUser->second.user.get();
}

const td::td_api::user *TdAccountData::getUserByPrivateChat(const td::td_api::chat &chat)
{
    UserId userId = getUserIdByPrivateChat(chat);
    if (userId.valid())
        return getUser(userId);
    return nullptr;
}

std::string TdAccountData::getDisplayName(const td::td_api::user &user) const
{
    return getDisplayName(getId(user));
}

std::string TdAccountData::getDisplayName(UserId userId) const
{
    auto it = m_userInfo.find(userId);
    if (it != m_userInfo.end())
        return it->second.displayName;
    else
        return std::string();
}

void TdAccountData::getUsersByDisplayName(const char *displayName,
                                          std::vector<const td::td_api::user*> &users)
{
    users.clear();
    if (!displayName || (*displayName == '\0'))
        return;

    for (const UserMap::value_type &entry: m_userInfo)
        if (entry.second.displayName == displayName)
            users.push_back(entry.second.user.get());
}

const td::td_api::basicGroup *TdAccountData::getBasicGroup(BasicGroupId groupId) const
{
    auto it = m_groups.find(groupId);
    if (it != m_groups.end())
        return it->second.group.get();
    else
        return nullptr;
}

const td::td_api::basicGroupFullInfo *TdAccountData::getBasicGroupInfo(BasicGroupId groupId) const
{
    auto it = m_groups.find(groupId);
    if (it != m_groups.end())
        return it->second.fullInfo.get();
    else
        return nullptr;
}

const td::td_api::supergroup *TdAccountData::getSupergroup(SupergroupId groupId) const
{
    auto it = m_supergroups.find(groupId);
    if (it != m_supergroups.end())
        return it->second.group.get();
    else
        return nullptr;
}

const td::td_api::supergroupFullInfo *TdAccountData::getSupergroupInfo(SupergroupId groupId) const
{
    auto it = m_supergroups.find(groupId);
    if (it != m_supergroups.end())
        return it->second.fullInfo.get();
    else
        return nullptr;
}

const td::td_api::chatMembers *TdAccountData::getSupergroupMembers(SupergroupId groupId) const
{
    auto it = m_supergroups.find(groupId);
    if (it != m_supergroups.end())
        return it->second.members.get();
    else
        return nullptr;
}

const td::td_api::chat *TdAccountData::getBasicGroupChatByGroup(BasicGroupId groupId) const
{
    if (!groupId.valid())
        return nullptr;

    auto it = std::find_if(m_chatInfo.begin(), m_chatInfo.end(),
                           [groupId](const ChatMap::value_type &entry) {
                               return (getBasicGroupId(*entry.second.chat) == groupId);
                           });

    if (it != m_chatInfo.end())
        return it->second.chat.get();
    else
        return nullptr;
}

const td::td_api::chat *TdAccountData::getSupergroupChatByGroup(SupergroupId groupId) const
{
    if (!groupId.valid())
        return nullptr;

    auto it = std::find_if(m_chatInfo.begin(), m_chatInfo.end(),
                           [groupId](const ChatMap::value_type &entry) {
                               return (getSupergroupId(*entry.second.chat) == groupId);
                           });

    if (it != m_chatInfo.end())
        return it->second.chat.get();
    else
        return nullptr;
}

bool TdAccountData::isGroupChatWithMembership(const td::td_api::chat &chat) const
{
    BasicGroupId groupId = getBasicGroupId(chat);
    if (groupId.valid()) {
        const td::td_api::basicGroup *group = getBasicGroup(groupId);
        return (group && isGroupMember(group->status_));
    }
    SupergroupId supergroupId = getSupergroupId(chat);
    if (supergroupId.valid()) {
        const td::td_api::supergroup *group = getSupergroup(supergroupId);
        return (group && isGroupMember(group->status_));
    }
    return false;
}

const td::td_api::chat *TdAccountData::getChatBySecretChat(SecretChatId secretChatId)
{
    auto it = std::find_if(m_chatInfo.begin(), m_chatInfo.end(),
                           [secretChatId](const ChatMap::value_type &entry) {
                               return (getSecretChatId(*entry.second.chat) == secretChatId);
                           });

    if (it != m_chatInfo.end())
        return it->second.chat.get();
    else
        return nullptr;
}

void TdAccountData::getChats(std::vector<const td::td_api::chat *> &chats) const
{
    chats.clear();
    for (const ChatMap::value_type &item: m_chatInfo)
        chats.push_back(item.second.chat.get());
}

void TdAccountData::deleteChat(ChatId id)
{
    const uint64_t generation = reserveForumTopicGeneration();
    for (auto &entry : m_forumTopics) {
        ForumTopicState &topic = entry.second;
        if (topic.target.chatId() == id)
            tombstoneForumTopic(topic, generation);
    }
    m_messageRoutes.erase(id);
    m_chatInfo.erase(id);
}

TdAccountData::ForumTopicUpsertResult TdAccountData::upsertForumTopic(
    ChatTarget target, const std::string &name, bool closed, bool hidden,
    uint64_t generation)
{
    if (!target.valid() || !target.isForumTopic())
        return ForumTopicUpsertResult(nullptr, false);
    if (generation > m_forumTopicGeneration)
        m_forumTopicGeneration = generation;

    auto inserted = m_forumTopics.emplace(target, ForumTopicState(target));
    ForumTopicState &topic = inserted.first->second;
    if (!inserted.second && generation <= topic.metadataGeneration) {
        return ForumTopicUpsertResult(&topic, false);
    }

    topic.name = name;
    topic.closed = closed;
    topic.hidden = hidden;
    topic.deleted = false;
    topic.metadataGeneration = generation;
    topic.metadataKnown = true;
    if (topic.saved || topic.active)
        allocateForumTopicPurpleId(topic);
    return ForumTopicUpsertResult(&topic, true);
}

const TdAccountData::ForumTopicState *
TdAccountData::ensureForumTopicPlaceholder(ChatTarget target)
{
    if (!target.valid() || !target.isForumTopic() ||
        target.forumTopicId() == ForumTopicId::general()) {
        return nullptr;
    }

    auto inserted = m_forumTopics.emplace(
        target, ForumTopicState(target));
    return &inserted.first->second;
}

void TdAccountData::invalidateForumTopicMetadata(
    ChatTarget target, uint64_t generation)
{
    ForumTopicState *topic = findForumTopicMutable(target);
    if (!topic || generation < topic->metadataGeneration)
        return;
    if (generation > m_forumTopicGeneration)
        m_forumTopicGeneration = generation;

    topic->metadataKnown = false;
    topic->metadataGeneration = generation;
}

uint64_t TdAccountData::reserveForumTopicGeneration()
{
    if (m_forumTopicGeneration != std::numeric_limits<uint64_t>::max())
        ++m_forumTopicGeneration;
    return m_forumTopicGeneration;
}

bool TdAccountData::tombstoneForumTopic(
    ForumTopicState &topic, uint64_t generation)
{
    if (generation <= topic.metadataGeneration)
        return false;

    const bool changed = !topic.deleted || topic.active;
    topic.deleted = true;
    topic.active = false;
    topic.metadataGeneration = generation;
    return changed;
}

std::vector<ChatTarget> TdAccountData::reconcileForumTopics(
    ChatId chatId, const std::set<ChatTarget> &seenTargets,
    uint64_t generation)
{
    std::vector<ChatTarget> tombstoned;
    if (!chatId.valid())
        return tombstoned;
    if (generation > m_forumTopicGeneration)
        m_forumTopicGeneration = generation;

    for (auto &entry : m_forumTopics) {
        ForumTopicState &topic = entry.second;
        if (topic.target.chatId() != chatId || topic.isGeneral() ||
            seenTargets.find(topic.target) != seenTargets.end()) {
            continue;
        }

        if (tombstoneForumTopic(topic, generation))
            tombstoned.push_back(topic.target);
    }
    return tombstoned;
}

TdAccountData::ForumTopicState *TdAccountData::findForumTopicMutable(
    ChatTarget target)
{
    auto topic = m_forumTopics.find(target);
    return topic == m_forumTopics.end() ? nullptr : &topic->second;
}

const TdAccountData::ForumTopicState *TdAccountData::findForumTopic(
    ChatTarget target) const
{
    auto topic = m_forumTopics.find(target);
    return topic == m_forumTopics.end() ? nullptr : &topic->second;
}

void TdAccountData::getForumTopics(
    ChatId chatId, std::vector<const ForumTopicState *> &topics) const
{
    topics.clear();
    for (const auto &entry : m_forumTopics) {
        if (entry.first.chatId() == chatId)
            topics.push_back(&entry.second);
    }
}

bool TdAccountData::setForumTopicSaved(ChatTarget target, bool saved)
{
    ForumTopicState *topic = findForumTopicMutable(target);
    if (!topic && saved && target.valid() && target.isForumTopic() &&
        target.forumTopicId() != ForumTopicId::general()) {
        auto inserted = m_forumTopics.emplace(target, ForumTopicState(target));
        topic = &inserted.first->second;
    }
    if (!topic)
        return false;
    topic->saved = saved;
    if (saved)
        allocateForumTopicPurpleId(*topic);
    return true;
}

int32_t TdAccountData::activateForumTopic(ChatTarget target)
{
    ForumTopicState *topic = findForumTopicMutable(target);
    if (!topic || topic->deleted)
        return 0;
    topic->active = true;
    return allocateForumTopicPurpleId(*topic);
}

int32_t TdAccountData::activateForumTopicForIncomingMessage(
    ChatTarget target)
{
    if (!target.valid() || !target.isForumTopic())
        return 0;
    if (target.forumTopicId() == ForumTopicId::general())
        return getPurpleChatId(target.chatId());

    auto inserted = m_forumTopics.emplace(
        target, ForumTopicState(target));
    ForumTopicState &topic = inserted.first->second;
    if (topic.deleted) {
        topic.deleted = false;
        topic.metadataKnown = false;
    }
    topic.active = true;
    return allocateForumTopicPurpleId(topic);
}

void TdAccountData::deactivateForumTopic(ChatTarget target)
{
    ForumTopicState *topic = findForumTopicMutable(target);
    if (topic)
        topic->active = false;
}

void TdAccountData::addExpectedChat(ChatTarget target)
{
    if (target.valid())
        m_expectedChats.insert(target);
}

bool TdAccountData::isExpectedChat(ChatTarget target) const
{
    return m_expectedChats.find(target) != m_expectedChats.end();
}

void TdAccountData::removeExpectedChat(ChatTarget target)
{
    m_expectedChats.erase(target);
}

void TdAccountData::getExpectedForumTopics(
    ChatId chatId, std::vector<ChatTarget> &targets) const
{
    targets.clear();
    for (ChatTarget target : m_expectedChats) {
        if (target.isForumTopic() &&
            target.forumTopicId() != ForumTopicId::general() &&
            target.chatId() == chatId) {
            targets.push_back(target);
        }
    }
}

std::unique_ptr<PendingRequest> TdAccountData::getPendingRequestImpl(uint64_t requestId)
{
    auto it = std::find_if(m_requests.begin(), m_requests.end(),
                           [requestId](const std::unique_ptr<PendingRequest> &req) {
                               return (req->requestId == requestId);
                           });

    if (it != m_requests.end()) {
        auto result = std::move(*it);
        m_requests.erase(it);
        return result;
    }

    return nullptr;
}

PendingRequest *TdAccountData::findPendingRequestImpl(uint64_t requestId)
{
    auto it = std::find_if(m_requests.begin(), m_requests.end(),
                           [requestId](const std::unique_ptr<PendingRequest> &req) {
                               return (req->requestId == requestId);
                           });

    if (it != m_requests.end())
        return it->get();

    return nullptr;
}

const ContactRequest *TdAccountData::findContactRequest(UserId userId)
{
    auto it = std::find_if(m_requests.begin(), m_requests.end(),
                           [userId](const std::unique_ptr<PendingRequest> &req) {
                               const ContactRequest *contactReq = dynamic_cast<const ContactRequest *>(req.get());
                               return (contactReq && (contactReq->userId == userId));
                           });

    if (it != m_requests.end())
        return static_cast<const ContactRequest *>(it->get());
    return nullptr;
}

DownloadRequest* TdAccountData::findDownloadRequest(int32_t fileId)
{
    auto it = std::find_if(m_requests.begin(), m_requests.end(),
                           [fileId](const std::unique_ptr<PendingRequest> &req) {
                               DownloadRequest *downloadReq = dynamic_cast<DownloadRequest *>(req.get());
                               return (downloadReq && (downloadReq->fileId == fileId));
                           });

    if (it != m_requests.end())
        return static_cast<DownloadRequest *>(it->get());
    return nullptr;
}

void TdAccountData::extractFileTransferRequests(std::vector<PurpleXfer *> &transfers)
{
    transfers.clear();

    for (unsigned i = 0; i < m_requests.size(); ) {
        UploadRequest *uploadReq = dynamic_cast<UploadRequest *>(m_requests[i].get());
        NewPrivateChatForMessage *newChatReq = dynamic_cast<NewPrivateChatForMessage *>(m_requests[i].get());
        PurpleXfer *xfer = NULL;
        if (uploadReq)
            xfer = uploadReq->xfer;
        else if (newChatReq)
            xfer = newChatReq->fileUpload;

        if (xfer) {
            transfers.push_back(xfer);
            m_requests.erase(m_requests.begin() + i);
        } else
            i++;
    }
}

void TdAccountData::addPendingSend(
    ChatTarget target, MessageId messageId,
    std::string tempFile)
{
    if (!target.valid() || !messageId.valid()) {
        if (!tempFile.empty())
            std::remove(tempFile.c_str());
        return;
    }

    auto existing = std::find_if(
        m_pendingSends.begin(), m_pendingSends.end(),
        [target, messageId](const PendingSendInfo &pending) {
            return pending.target == target &&
                   pending.messageId == messageId;
        });
    if (existing != m_pendingSends.end()) {
        if (existing->tempFile.empty()) {
            existing->tempFile = std::move(tempFile);
        } else if (!tempFile.empty() &&
                   existing->tempFile != tempFile) {
            std::remove(tempFile.c_str());
        }
        return;
    }

    m_pendingSends.push_back(
        PendingSendInfo{
            target, messageId, std::move(tempFile)});
}

TdAccountData::PendingSendLookupResult
TdAccountData::extractPendingSend(
    MessageId messageId, PendingSendInfo &pending)
{
    pending = PendingSendInfo();
    auto match = m_pendingSends.end();
    for (auto it = m_pendingSends.begin();
         it != m_pendingSends.end(); ++it) {
        if (it->messageId != messageId)
            continue;
        if (match != m_pendingSends.end())
            return PendingSendLookupResult::Ambiguous;
        match = it;
    }

    if (match == m_pendingSends.end())
        return PendingSendLookupResult::NotFound;

    pending = std::move(*match);
    m_pendingSends.erase(match);
    return PendingSendLookupResult::Found;
}

bool TdAccountData::extractPendingSend(
    ChatId chatId, MessageId messageId,
    PendingSendInfo &pending)
{
    pending = PendingSendInfo();
    auto match = std::find_if(
        m_pendingSends.begin(), m_pendingSends.end(),
        [chatId, messageId](const PendingSendInfo &item) {
            return item.target.chatId() == chatId &&
                   item.messageId == messageId;
        });
    if (match == m_pendingSends.end())
        return false;

    pending = std::move(*match);
    m_pendingSends.erase(match);
    return true;
}

void TdAccountData::addFileTransfer(int32_t fileId, PurpleXfer *xfer, ChatId chatId)
{
    if (std::find_if(m_fileTransfers.begin(), m_fileTransfers.end(),
                    [fileId](const FileTransferInfo &upload) {
                        return (upload.fileId == fileId);
                    }) == m_fileTransfers.end()) {
        m_fileTransfers.emplace_back();
        m_fileTransfers.back().fileId = fileId;
        m_fileTransfers.back().xfer = xfer;
        m_fileTransfers.back().chatId = chatId;
    }
}

bool TdAccountData::getFileTransfer(int32_t fileId, PurpleXfer *&xfer, ChatId &chatId)
{
    auto it = std::find_if(m_fileTransfers.begin(), m_fileTransfers.end(),
                           [fileId](const FileTransferInfo &upload) { return (upload.fileId == fileId); });
    if (it != m_fileTransfers.end()) {
        xfer = it->xfer;
        chatId = it->chatId;
        return true;
    }

    return false;
}

bool TdAccountData::getFileIdForTransfer(PurpleXfer *xfer, int &fileId)
{
    auto it = std::find_if(m_fileTransfers.begin(), m_fileTransfers.end(),
                           [xfer](const FileTransferInfo &upload) { return (upload.xfer == xfer); });
    if (it != m_fileTransfers.end()) {
        fileId = it->fileId;
        return true;
    } else
        return false;
}

void TdAccountData::removeFileTransfer(int32_t fileId)
{
    auto it = std::find_if(m_fileTransfers.begin(), m_fileTransfers.end(),
                           [fileId](const FileTransferInfo &upload) { return (upload.fileId == fileId); });
    if (it != m_fileTransfers.end())
        m_fileTransfers.erase(it);
}

void TdAccountData::removeAllFileTransfers(std::vector<PurpleXfer *>& transfers)
{
    transfers.resize(m_fileTransfers.size());
    for (size_t i = 0; i < m_fileTransfers.size(); i++)
        transfers[i] = m_fileTransfers[i].xfer;
    m_fileTransfers.clear();
}

void TdAccountData::addSecretChat(td::td_api::object_ptr<td::td_api::secretChat> secretChat)
{
    if (secretChat)
        m_secretChats[getId(*secretChat)] = std::move(secretChat);
}

const td::td_api::secretChat *TdAccountData::getSecretChat(SecretChatId id)
{
    auto it = m_secretChats.find(id);
    return (it != m_secretChats.end()) ? it->second.get() : nullptr;
}

void TdAccountData::deleteSecretChat(SecretChatId id)
{
    m_secretChats.erase(id);
}

std::vector<std::pair<BasicGroupId, const td::td_api::basicGroupFullInfo *>>
TdAccountData::getBasicGroupsWithMember(UserId userId)
{
    std::vector<std::pair<BasicGroupId, const td::td_api::basicGroupFullInfo *>> result;

    for (const auto &item: m_groups)
        if (item.second.fullInfo) {
            auto &members = item.second.fullInfo->members_;
            if (std::any_of(members.begin(), members.end(),
                            [userId](const td::td_api::object_ptr<td::td_api::chatMember> &member) {
                                return (member && (getUserId(*member) == userId));
                            }))
            {
                result.push_back(std::make_pair(getId(*item.second.group), item.second.fullInfo.get()));
            }
        }

    return result;
}

bool TdAccountData::hasActiveCall()
{
    return (m_callData != nullptr);
}

void TdAccountData::setActiveCall(int32_t callId)
{
    if (!m_callData) {
        m_callData = std::make_unique<tgvoip::VoIPController>();
        m_callId = callId;
    }
}

tgvoip::VoIPController *TdAccountData::getCallData()
{
    return m_callData.get();
}

void TdAccountData::removeActiveCall()
{
    m_callData.reset();
    m_callId = 0;
}

void TdAccountData::addPendingReadReceipt(
    ChatTarget target, MessageId messageId)
{
    if (target.valid() && messageId.valid())
        m_pendingReadReceipts[target].push_back(messageId);
}

void TdAccountData::extractPendingReadReceipts(
    ChatTarget target, std::vector<MessageId>& messageIds)
{
    auto receipts = m_pendingReadReceipts.find(target);
    if (receipts == m_pendingReadReceipts.end()) {
        messageIds.clear();
        return;
    }

    messageIds = std::move(receipts->second);
    m_pendingReadReceipts.erase(receipts);
}

void TdAccountData::rememberMessageTarget(
    ChatTarget target, MessageId messageId)
{
    if (!target.valid() || !messageId.valid())
        return;

    MessageRouteMap &messages =
        m_messageRoutes[target.chatId()];
    auto inserted = messages.emplace(
        messageId, MessageRouteInfo());
    MessageRouteInfo &record =
        inserted.first->second;
    if (record.target.valid() &&
        record.target != target) {
        record = MessageRouteInfo();
    }
    record.target = target;
    if (inserted.second)
        pruneMessageRoutes(target.chatId());
}

void TdAccountData::replaceMessageId(
    ChatId oldChatId, MessageId oldMessageId,
    ChatId newChatId, MessageId newMessageId)
{
    if (!oldChatId.valid() || !newChatId.valid() ||
        !oldMessageId.valid() ||
        !newMessageId.valid())
        return;
    if (oldChatId == newChatId &&
        oldMessageId == newMessageId)
        return;

    auto chatMessages = m_messageRoutes.find(oldChatId);
    if (chatMessages == m_messageRoutes.end())
        return;

    MessageRouteMap &messages = chatMessages->second;
    auto oldRecord = messages.find(oldMessageId);
    if (oldRecord == messages.end())
        return;

    MessageRouteInfo oldInfo =
        std::move(oldRecord->second);
    messages.erase(oldRecord);
    if (messages.empty())
        m_messageRoutes.erase(chatMessages);

    MessageRouteMap &newMessages =
        m_messageRoutes[newChatId];
    auto newRecord = newMessages.find(newMessageId);
    if (newRecord == newMessages.end()) {
        newMessages[newMessageId] =
            std::move(oldInfo);
        pruneMessageRoutes(newChatId);
    } else if (
        newRecord->second.conversationType ==
            PURPLE_CONV_TYPE_UNKNOWN &&
        (!newRecord->second.target.valid() ||
         newRecord->second.target ==
             oldInfo.target)) {
        newRecord->second = std::move(oldInfo);
    }
}

void TdAccountData::rememberDisplayedMessage(
    ChatTarget target, MessageId messageId,
    PurpleConversation *conv, const std::string &sender,
    time_t timestamp, PurpleMessageFlags flags)
{
    if (!target.valid() || !messageId.valid() || !conv)
        return;

    MessageRouteMap &messages =
        m_messageRoutes[target.chatId()];
    auto inserted = messages.emplace(
        messageId, MessageRouteInfo());
    MessageRouteInfo &record =
        inserted.first->second;
    record.target = target;
    record.conversationType =
        purple_conversation_get_type(conv);
    const char *conversationName =
        purple_conversation_get_name(conv);
    record.conversationName =
        conversationName ? conversationName : "";
    record.sender = sender;
    record.timestamp = timestamp;
    record.flags = flags;
    if (inserted.second)
        pruneMessageRoutes(target.chatId());
}

void TdAccountData::pruneMessageRoutes(ChatId chatId)
{
    auto chatMessages = m_messageRoutes.find(chatId);
    if (chatMessages == m_messageRoutes.end())
        return;

    MessageRouteMap &messages = chatMessages->second;
    while (messages.size() > MaxMessageRoutesPerChat) {
        auto oldestServerMessage =
            messages.upper_bound(MessageId());
        if (oldestServerMessage == messages.end())
            oldestServerMessage = messages.begin();
        messages.erase(oldestServerMessage);
    }
}

bool TdAccountData::isForumSensitiveChat(
    ChatId chatId) const
{
    if (isForumChat(chatId))
        return true;

    const td::td_api::chat *chat = getChat(chatId);
    if (chat) {
        const SupergroupId groupId =
            getSupergroupId(*chat);
        auto group = m_supergroups.find(groupId);
        if (group != m_supergroups.end() &&
            group->second.hasEverBeenForum) {
            return true;
        }
    }

    for (const auto &entry : m_forumTopics) {
        if (entry.first.chatId() == chatId &&
            entry.first.isForumTopic()) {
            return true;
        }
    }
    return false;
}

bool TdAccountData::shouldUseLegacyMessageUpdateFallback(
    ChatId chatId, MessageId messageId) const
{
    auto chatMessages = m_messageRoutes.find(chatId);
    if (chatMessages == m_messageRoutes.end())
        return !isForumSensitiveChat(chatId);
    auto record = chatMessages->second.find(messageId);
    if (record == chatMessages->second.end())
        return !isForumSensitiveChat(chatId);

    const ChatTarget target = record->second.target;
    if (!target.valid())
        return !isForumSensitiveChat(chatId);

    return !target.isForumTopic() ||
           target.forumTopicId() ==
               ForumTopicId::general();
}

TdAccountData::DisplayedMessageLookupResult
TdAccountData::findDisplayedMessageConversation(
    ChatId chatId, MessageId messageId,
    DisplayedMessageConversation &conversation) const
{
    auto chatMessages = m_messageRoutes.find(chatId);
    if (chatMessages == m_messageRoutes.end()) {
        conversation = DisplayedMessageConversation();
        return DisplayedMessageLookupResult::UnknownMessage;
    }
    auto record = chatMessages->second.find(messageId);
    if (record == chatMessages->second.end()) {
        conversation = DisplayedMessageConversation();
        return DisplayedMessageLookupResult::UnknownMessage;
    }

    const MessageRouteInfo &displayed = record->second;
    conversation.type = displayed.conversationType;
    conversation.name = displayed.conversationName;
    if (conversation.type == PURPLE_CONV_TYPE_UNKNOWN ||
        conversation.name.empty()) {
        return DisplayedMessageLookupResult::
            KnownConversationUnavailable;
    }
    if (displayed.target.isForumTopic() &&
        displayed.target.forumTopicId() !=
            ForumTopicId::general()) {
        const ForumTopicState *topic =
            findForumTopic(displayed.target);
        if (!topic || topic->deleted || !topic->active) {
            return DisplayedMessageLookupResult::
                KnownConversationUnavailable;
        }
    }

    PurpleConversation *conv = purple_find_conversation_with_account(
        conversation.type, conversation.name.c_str(),
        purpleAccount);
    if (!conv) {
        return DisplayedMessageLookupResult::
            KnownConversationUnavailable;
    }
    if (displayed.conversationType ==
        PURPLE_CONV_TYPE_CHAT) {
        PurpleConvChat *chat =
            purple_conversation_get_chat_data(conv);
        if (!chat || purple_conv_chat_has_left(chat)) {
            return DisplayedMessageLookupResult::
                KnownConversationUnavailable;
        }
    }

    return DisplayedMessageLookupResult::Available;
}

TdAccountData::DisplayedMessageUpdateResult
TdAccountData::showUpdatedMessage(
    ChatId chatId, MessageId messageId,
    const std::string &newText)
{
    auto chatMessages = m_messageRoutes.find(chatId);
    if (chatMessages == m_messageRoutes.end()) {
        return DisplayedMessageUpdateResult::UnknownMessage;
    }
    auto record = chatMessages->second.find(messageId);
    if (record == chatMessages->second.end()) {
        return DisplayedMessageUpdateResult::UnknownMessage;
    }

    DisplayedMessageConversation conversation;
    const DisplayedMessageLookupResult lookup =
        findDisplayedMessageConversation(
            chatId, messageId, conversation);
    if (lookup ==
        DisplayedMessageLookupResult::
            KnownConversationUnavailable) {
        return DisplayedMessageUpdateResult::
            KnownConversationUnavailable;
    }
    if (lookup ==
        DisplayedMessageLookupResult::UnknownMessage) {
        return DisplayedMessageUpdateResult::UnknownMessage;
    }

    const MessageRouteInfo displayed = record->second;
    PurpleConversation *conv =
        purple_find_conversation_with_account(
            conversation.type,
            conversation.name.c_str(),
            purpleAccount);
    if (!conv) {
        return DisplayedMessageUpdateResult::
            KnownConversationUnavailable;
    }
    if (displayed.conversationType == PURPLE_CONV_TYPE_CHAT) {
        PurpleConvChat *chat =
            purple_conversation_get_chat_data(conv);
        if (!chat || purple_conv_chat_has_left(chat)) {
            return DisplayedMessageUpdateResult::
                KnownConversationUnavailable;
        }
    }

    const std::string sender = displayed.sender.empty()
        ? _("Updated")
        : formatMessage(_("Updated {}"), displayed.sender);
    purple_conversation_write(conv, sender.c_str(), newText.c_str(),
                              displayed.flags, displayed.timestamp);
    return DisplayedMessageUpdateResult::Written;
}
