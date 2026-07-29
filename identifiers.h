#ifndef _IDENTIFIERS_H
#define _IDENTIFIERS_H

#include <stdint.h>
#include <string>
#include <limits>
#include <stdlib.h>
#include <td/telegram/td_api.h>

template<typename IntType>
class Identifier {
protected:
    using IdType = IntType;
    explicit Identifier(IntType value) : m_value(value) {}
    static IntType convertFromString(const char *s);
public:
    bool    valid() const { return (m_value != 0); }
    IntType value() const { return m_value; }
private:
    IntType m_value;
    
    template<typename Object>
    friend void setId(Object &object, Identifier<IntType> id);
};

template <typename IntType>
IntType Identifier<IntType>::convertFromString(const char *s)
{
    long long x;
    static_assert(sizeof(x) >= sizeof(IntType), "long long must hold identifier type");
	errno = 0;
    x = strtoll(s, NULL, 10);
    if (errno || (x < std::numeric_limits<IntType>::min()) || (x > std::numeric_limits<IntType>::max()))
        return 0;

    return x;
}

template<typename Object, typename IntType>
void setId(Object &object, Identifier<IntType> id)
{
    object.id_ = id.value;
}

#define DEFINE_ID_CLASS(classname, inttype) \
class classname: public Identifier<inttype> { \
private: \
    explicit classname(IdType id) : Identifier(id) {} \
public: \
    classname() : classname(classname::invalid) {} \
    static classname fromString(const char *s) { return classname(convertFromString(s)); } \
    bool operator==(const classname &other) const { return (value() == other.value()); } \
    bool operator!=(const classname &other) const { return (value() != other.value()); } \
    bool operator<(const classname &other) const  { return (value() < other.value()); } \
    static const classname invalid;

DEFINE_ID_CLASS(UserId, int64_t)
    friend UserId getId(const td::td_api::user &user);
    friend UserId getUserId(const td::td_api::chatTypePrivate &privType);
    friend UserId getUserId(const td::td_api::chatMember &member);
    friend UserId getUserId(const td::td_api::call &call);
    friend UserId getSenderUserId(const td::td_api::message &message);
    friend UserId getSenderUserId(const td::td_api::messageOriginUser &forwardOrigin);
    friend UserId getUserId(const td::td_api::secretChat &secretChat);
    friend UserId getUserId(const td::td_api::updateUserStatus &update);
    friend UserId getUserId(const td::td_api::updateChatAction &update);
    friend UserId getUserId(const td::td_api::importedContacts &contacts, unsigned index);
    friend UserId getUserId(const td::td_api::users &users, unsigned index);
    friend UserId getUserId(const td::td_api::object_ptr<td::td_api::MessageSender>& sender);
};

DEFINE_ID_CLASS(ChatId, int64_t)
    friend ChatId getId(const td::td_api::chat &chat);
    friend ChatId getChatId(const td::td_api::updateChatPosition &update);
    friend ChatId getChatId(const td::td_api::updateChatTitle &update);
    friend ChatId getChatId(const td::td_api::messageOriginChannel &forwardOrigin);
    friend ChatId getChatId(const td::td_api::message &message);
    friend ChatId getChatId(const td::td_api::updateChatAction &update);
    friend ChatId getChatId(const td::td_api::updateChatLastMessage &update);
};

class ForumTopicId: public Identifier<int32_t> {
private:
    explicit ForumTopicId(IdType id) : Identifier(id) {}
public:
    ForumTopicId() : ForumTopicId(0) {}
    static ForumTopicId fromValue(int32_t id) {
        return ForumTopicId(id > 0 ? id : 0);
    }
    static ForumTopicId general() {
        return ForumTopicId(1);
    }
    bool valid() const { return value() > 0; }
    bool operator==(const ForumTopicId &other) const { return value() == other.value(); }
    bool operator!=(const ForumTopicId &other) const { return value() != other.value(); }
    bool operator<(const ForumTopicId &other) const  { return value() < other.value(); }
    static const ForumTopicId invalid;
};

class ChatTarget {
private:
    ChatTarget(ChatId chatId, ForumTopicId forumTopicId, bool isForumTopic)
        : m_chatId(chatId),
          m_forumTopicId(forumTopicId),
          m_isForumTopic(isForumTopic)
    {}

public:
    ChatTarget()
        : ChatTarget(ChatId::invalid, ForumTopicId::invalid, false)
    {}

    static ChatTarget chat(ChatId chatId) {
        return ChatTarget(chatId, ForumTopicId::invalid, false);
    }

    static ChatTarget forumTopic(ChatId chatId, ForumTopicId forumTopicId) {
        return ChatTarget(chatId, forumTopicId, true);
    }

    bool valid() const {
        return m_chatId.valid() && (!m_isForumTopic || m_forumTopicId.valid());
    }

    ChatId chatId() const { return m_chatId; }
    ForumTopicId forumTopicId() const { return m_forumTopicId; }
    bool isForumTopic() const { return m_isForumTopic; }

    bool operator==(const ChatTarget &other) const {
        return m_chatId == other.m_chatId &&
               m_forumTopicId == other.m_forumTopicId &&
               m_isForumTopic == other.m_isForumTopic;
    }

    bool operator!=(const ChatTarget &other) const {
        return !(*this == other);
    }

    bool operator<(const ChatTarget &other) const {
        if (m_chatId != other.m_chatId)
            return m_chatId < other.m_chatId;
        if (m_isForumTopic != other.m_isForumTopic)
            return m_isForumTopic < other.m_isForumTopic;
        return m_forumTopicId < other.m_forumTopicId;
    }

private:
    ChatId m_chatId;
    ForumTopicId m_forumTopicId;
    bool m_isForumTopic;
};

DEFINE_ID_CLASS(BasicGroupId, int64_t)
    friend BasicGroupId getId(const td::td_api::basicGroup &group);
    friend BasicGroupId getBasicGroupId(const td::td_api::updateBasicGroupFullInfo &update);
    friend BasicGroupId getBasicGroupId(const td::td_api::chatTypeBasicGroup &update);
};

DEFINE_ID_CLASS(SupergroupId, int64_t)
    friend SupergroupId getId(const td::td_api::supergroup &group);
    friend SupergroupId getSupergroupId(const td::td_api::updateSupergroupFullInfo &update);
    friend SupergroupId getSupergroupId(const td::td_api::chatTypeSupergroup &update);
};

DEFINE_ID_CLASS(SecretChatId, int32_t)
    friend SecretChatId getId(const td::td_api::secretChat &secretChat);
    friend SecretChatId getSecretChatId(const td::td_api::chatTypeSecret &update);
};

DEFINE_ID_CLASS(MessageId, int64_t)
    friend MessageId getId(const td::td_api::message &message);
    friend MessageId getReplyMessageId(const td::td_api::message &message);
};

#undef DEFINE_ID_CLASS

UserId       getId(const td::td_api::user &user);
ChatId       getId(const td::td_api::chat &chat);
BasicGroupId getId(const td::td_api::basicGroup &group);
SupergroupId getId(const td::td_api::supergroup &group);
SecretChatId getId(const td::td_api::secretChat &secretChat);
MessageId    getId(const td::td_api::message &message);

UserId       getUserId(const td::td_api::chatTypePrivate &privType);
UserId       getUserId(const td::td_api::chatMember &member);
UserId       getUserId(const td::td_api::call &call);
UserId       getSenderUserId(const td::td_api::message &message);
UserId       getSenderUserId(const td::td_api::messageOriginUser &forwardOrigin);
UserId       getUserId(const td::td_api::secretChat &secretChat);
UserId       getUserId(const td::td_api::updateUserStatus &update);
UserId       getUserId(const td::td_api::updateChatAction &update);
UserId       getUserId(const td::td_api::importedContacts &contacts, unsigned index);
UserId       getUserId(const td::td_api::users &users, unsigned index);
UserId 	     getUserId(const td::td_api::object_ptr<td::td_api::MessageSender>& sender);

ChatId       getChatId(const td::td_api::updateChatPosition &update);
ChatId       getChatId(const td::td_api::updateChatTitle &update);
ChatId       getChatId(const td::td_api::messageOriginChannel &forwardOrigin);
ChatId       getChatId(const td::td_api::message &message);
ChatId       getChatId(const td::td_api::updateChatAction &update);
ChatId       getChatId(const td::td_api::updateChatLastMessage &update);

BasicGroupId getBasicGroupId(const td::td_api::updateBasicGroupFullInfo &update);
BasicGroupId getBasicGroupId(const td::td_api::chatTypeBasicGroup &chatType);

SupergroupId getSupergroupId(const td::td_api::updateSupergroupFullInfo &update);
SupergroupId getSupergroupId(const td::td_api::chatTypeSupergroup &chatType);

SecretChatId getSecretChatId(const td::td_api::chatTypeSecret &update);

MessageId    getReplyMessageId(const td::td_api::message &message);

namespace std {
    static inline std::string to_string(UserId id) { return to_string(id.value()); }
}

#endif
