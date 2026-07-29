#include "forum-topics.h"

#include "account-data.h"
#include "config.h"
#include "purple-info.h"
#include "transceiver.h"
#include "translate.h"

#include <purple.h>

#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int32_t FORUM_TOPICS_PAGE_SIZE = 100;

ChatId chatIdFromValue(td::td_api::int53 value)
{
    const std::string text = std::to_string(value);
    return ChatId::fromString(text.c_str());
}

struct ForumTopicCursor {
    int32_t date = 0;
    td::td_api::int53 messageId = 0;
    int32_t topicId = 0;

    bool operator<(const ForumTopicCursor &other) const
    {
        if (date != other.date)
            return date < other.date;
        if (messageId != other.messageId)
            return messageId < other.messageId;
        return topicId < other.topicId;
    }
};

class ForumTopicsAdapterCore;

class RoomListSession:
    public std::enable_shared_from_this<RoomListSession> {
public:
    RoomListSession(std::weak_ptr<ForumTopicsAdapterCore> owner,
                    PurpleRoomlist *roomList)
        : m_owner(std::move(owner)),
          m_roomList(roomList)
    {}

    bool active() const { return m_active; }

    PurpleRoomlist *detach()
    {
        if (!m_active)
            return nullptr;

        m_active = false;
        m_waitingForPage = false;
        PurpleRoomlist *roomList = m_roomList;
        m_roomList = nullptr;
        return roomList;
    }

    void begin();
    void projectTopic(const TdAccountData::ForumTopicState &topic);

private:
    void addRoom(ChatTarget target, const std::string &displayName,
                 const std::string &description);
    void requestPage();
    void handlePage(uint64_t requestSerial, uint64_t metadataGeneration,
                    td::td_api::object_ptr<td::td_api::Object> response);
    void advanceForum();

    std::weak_ptr<ForumTopicsAdapterCore> m_owner;
    PurpleRoomlist *m_roomList;
    bool m_active = true;
    bool m_started = false;
    bool m_waitingForPage = false;
    std::vector<ChatId> m_forums;
    std::size_t m_forumIndex = 0;
    ForumTopicCursor m_cursor;
    std::set<ForumTopicCursor> m_requestedCursors;
    std::set<ChatTarget> m_seenTargets;
    std::set<ChatId> m_eligibleForums;
    std::set<ChatTarget> m_projectedTargets;
    bool m_reconciliationSafe = true;
    uint64_t m_listingMetadataGeneration = 0;
    uint64_t m_lastRequestSerial = 0;
    uint64_t m_pendingRequestSerial = 0;
};

class ForumTopicsAdapterCore:
    public std::enable_shared_from_this<ForumTopicsAdapterCore> {
public:
    ForumTopicsAdapterCore(ForumTopicsAdapter *adapter,
                           TdTransceiver &transceiver,
                           TdAccountData &account)
        : m_adapter(adapter),
          m_transceiver(transceiver),
          m_account(account)
    {}

    TdTransceiver &transceiver() { return m_transceiver; }
    TdAccountData &account() { return m_account; }
    bool shuttingDown() const { return m_shuttingDown; }

    void markRoomListsPending();
    void markRoomListsReady();
    void startRoomList(PurpleRoomlist *roomList);
    void cancelRoomList(PurpleRoomlist *roomList);
    void shutdown();
    void processUpdate(const td::td_api::updateForumTopicInfo &update);
    void finish(std::shared_ptr<RoomListSession> session);
    const TdAccountData::ForumTopicState *upsertMetadata(
        const ForumTopicMetadata &metadata, uint64_t generation);
    bool findForumParent(ChatId chatId, const td::td_api::chat *&chat) const;

private:
    std::vector<std::shared_ptr<RoomListSession>> sessionSnapshot() const;
    void configureFields(PurpleRoomlist *roomList);

    ForumTopicsAdapter *m_adapter;
    TdTransceiver &m_transceiver;
    TdAccountData &m_account;
    bool m_roomListsReady = false;
    bool m_shuttingDown = false;
    std::map<PurpleRoomlist *, std::shared_ptr<RoomListSession>> m_sessions;
};

std::string getChatDescription(const td::td_api::chat &chat,
                               const TdAccountData &account)
{
    const BasicGroupId basicGroupId = getBasicGroupId(chat);
    if (basicGroupId.valid()) {
        const td::td_api::basicGroupFullInfo *info =
            account.getBasicGroupInfo(basicGroupId);
        return info ? info->description_ : std::string();
    }

    const SupergroupId supergroupId = getSupergroupId(chat);
    if (supergroupId.valid()) {
        const td::td_api::supergroupFullInfo *info =
            account.getSupergroupInfo(supergroupId);
        return info ? info->description_ : std::string();
    }

    return std::string();
}

std::vector<std::shared_ptr<RoomListSession>>
ForumTopicsAdapterCore::sessionSnapshot() const
{
    std::vector<std::shared_ptr<RoomListSession>> result;
    result.reserve(m_sessions.size());
    for (const auto &entry : m_sessions)
        result.push_back(entry.second);
    return result;
}

void ForumTopicsAdapterCore::configureFields(PurpleRoomlist *roomList)
{
    GList *fields = nullptr;
    PurpleRoomlistField *field = purple_roomlist_field_new(
        PURPLE_ROOMLIST_FIELD_STRING, "", getChatNameComponent(), TRUE);
    fields = g_list_append(fields, field);
    field = purple_roomlist_field_new(
        PURPLE_ROOMLIST_FIELD_STRING, _("Description"), "description", FALSE);
    fields = g_list_append(fields, field);
    purple_roomlist_set_fields(roomList, fields);
}

void ForumTopicsAdapterCore::startRoomList(PurpleRoomlist *roomList)
{
    if (!roomList || m_shuttingDown ||
        m_sessions.find(roomList) != m_sessions.end()) {
        return;
    }

    std::shared_ptr<RoomListSession> session =
        std::make_shared<RoomListSession>(shared_from_this(), roomList);
    purple_roomlist_ref(roomList);
    m_sessions.emplace(roomList, session);
    roomList->proto_data = m_adapter;

    configureFields(roomList);
    if (!session->active())
        return;

    purple_roomlist_set_in_progress(roomList, TRUE);
    if (!session->active())
        return;

    if (m_roomListsReady)
        session->begin();
}

void ForumTopicsAdapterCore::cancelRoomList(PurpleRoomlist *roomList)
{
    auto session = m_sessions.find(roomList);
    if (session != m_sessions.end())
        finish(session->second);
}

void ForumTopicsAdapterCore::markRoomListsPending()
{
    if (!m_shuttingDown)
        m_roomListsReady = false;
}

void ForumTopicsAdapterCore::markRoomListsReady()
{
    if (m_roomListsReady || m_shuttingDown)
        return;

    m_roomListsReady = true;
    for (const std::shared_ptr<RoomListSession> &session : sessionSnapshot())
        session->begin();
}

void ForumTopicsAdapterCore::finish(
    std::shared_ptr<RoomListSession> session)
{
    if (!session || !session->active())
        return;

    PurpleRoomlist *roomList = session->detach();
    auto tracked = m_sessions.find(roomList);
    if (tracked != m_sessions.end() && tracked->second == session)
        m_sessions.erase(tracked);
    if (roomList && roomList->proto_data == m_adapter)
        roomList->proto_data = nullptr;

    if (roomList) {
        purple_roomlist_set_in_progress(roomList, FALSE);
        purple_roomlist_unref(roomList);
    }
}

void ForumTopicsAdapterCore::shutdown()
{
    if (m_shuttingDown)
        return;

    m_shuttingDown = true;
    std::vector<PurpleRoomlist *> roomLists;
    roomLists.reserve(m_sessions.size());
    for (const auto &entry : m_sessions) {
        PurpleRoomlist *roomList = entry.second->detach();
        if (!roomList)
            continue;
        if (roomList->proto_data == m_adapter)
            roomList->proto_data = nullptr;
        roomLists.push_back(roomList);
    }
    m_sessions.clear();

    for (PurpleRoomlist *roomList : roomLists) {
        purple_roomlist_set_in_progress(roomList, FALSE);
        purple_roomlist_unref(roomList);
    }
}

bool ForumTopicsAdapterCore::findForumParent(
    ChatId chatId, const td::td_api::chat *&chat) const
{
    chat = m_account.getChat(chatId);
    if (!chat || !m_account.isGroupChatWithMembership(*chat))
        return false;

    const SupergroupId supergroupId = getSupergroupId(*chat);
    const td::td_api::supergroup *supergroup =
        supergroupId.valid() ? m_account.getSupergroup(supergroupId) : nullptr;
    return supergroup && supergroup->is_forum_;
}

const TdAccountData::ForumTopicState *
ForumTopicsAdapterCore::upsertMetadata(
    const ForumTopicMetadata &metadata, uint64_t generation)
{
    const TdAccountData::ForumTopicUpsertResult result =
        m_account.upsertForumTopic(metadata.target, metadata.name,
                                  metadata.closed, metadata.hidden, generation);
    return result.state;
}

void ForumTopicsAdapterCore::processUpdate(
    const td::td_api::updateForumTopicInfo &update)
{
    if (m_shuttingDown || !update.info_)
        return;

    ForumTopicMetadata metadata;
    if (!adaptForumTopicInfo(*update.info_, metadata))
        return;

    const TdAccountData::ForumTopicState *topic =
        upsertMetadata(
            metadata, m_account.reserveForumTopicGeneration());
    if (!topic || topic->isGeneral())
        return;

    for (const std::shared_ptr<RoomListSession> &session : sessionSnapshot())
        session->projectTopic(*topic);
}

void RoomListSession::addRoom(ChatTarget target,
                              const std::string &displayName,
                              const std::string &description)
{
    // libpurple has no API for updating an added room. Keep each room list as
    // a stable snapshot and let the next list refresh names from Telegram.
    if (!m_active || !m_roomList || !target.valid() ||
        !m_projectedTargets.insert(target).second) {
        return;
    }

    PurpleRoomlistRoom *room = purple_roomlist_room_new(
        PURPLE_ROOMLIST_ROOMTYPE_ROOM, displayName.c_str(), nullptr);
    purple_roomlist_room_add_field(
        m_roomList, room, getPurpleChatName(target).c_str());
    if (!description.empty())
        purple_roomlist_room_add_field(m_roomList, room, description.c_str());
    purple_roomlist_room_add(m_roomList, room);
}

void RoomListSession::projectTopic(
    const TdAccountData::ForumTopicState &topic)
{
    if (!m_active || !m_started || !topic.metadataKnown ||
        topic.deleted || topic.isGeneral() ||
        m_eligibleForums.find(topic.target.chatId()) ==
            m_eligibleForums.end()) {
        return;
    }

    std::shared_ptr<ForumTopicsAdapterCore> owner = m_owner.lock();
    if (!owner || owner->shuttingDown())
        return;

    const td::td_api::chat *parent = nullptr;
    if (!owner->findForumParent(topic.target.chatId(), parent))
        return;

    addRoom(topic.target, parent->title_ + " / " + topic.name, std::string());
}

void RoomListSession::begin()
{
    if (!m_active || m_started)
        return;

    std::shared_ptr<ForumTopicsAdapterCore> owner = m_owner.lock();
    if (!owner || owner->shuttingDown())
        return;
    m_started = true;

    std::vector<const td::td_api::chat *> chats;
    owner->account().getChats(chats);
    for (const td::td_api::chat *chat : chats) {
        if (!m_active)
            return;
        if (!chat || !owner->account().isGroupChatWithMembership(*chat))
            continue;

        const ChatId chatId = getId(*chat);
        const td::td_api::chat *forumParent = nullptr;
        if (!owner->findForumParent(chatId, forumParent)) {
            addRoom(ChatTarget::chat(chatId), chat->title_,
                    getChatDescription(*chat, owner->account()));
            continue;
        }

        addRoom(ChatTarget::forumTopic(chatId, ForumTopicId::general()),
                chat->title_, getChatDescription(*chat, owner->account()));
        if (!m_active)
            return;

        m_forums.push_back(chatId);
        m_eligibleForums.insert(chatId);
    }

    if (m_forums.empty())
        owner->finish(shared_from_this());
    else
        requestPage();
}

void RoomListSession::requestPage()
{
    if (!m_active || m_waitingForPage)
        return;

    std::shared_ptr<ForumTopicsAdapterCore> owner = m_owner.lock();
    if (!owner || owner->shuttingDown() || m_forumIndex >= m_forums.size())
        return;

    const td::td_api::chat *parent = nullptr;
    if (!owner->findForumParent(m_forums[m_forumIndex], parent)) {
        purple_debug_misc(
            config::pluginId,
            "Skipping forum topic discovery because parent chat %"
            G_GINT64_FORMAT " is no longer eligible\n",
            m_forums[m_forumIndex].value());
        advanceForum();
        return;
    }

    if (!m_requestedCursors.insert(m_cursor).second) {
        purple_debug_warning(
            config::pluginId,
            "Stopping forum topic discovery for chat %" G_GINT64_FORMAT
            " after a repeated pagination cursor\n",
            m_forums[m_forumIndex].value());
        advanceForum();
        return;
    }
    if (m_listingMetadataGeneration == 0)
        m_listingMetadataGeneration =
            owner->account().reserveForumTopicGeneration();

    const uint64_t generation = m_listingMetadataGeneration;
    const uint64_t requestSerial = ++m_lastRequestSerial;
    m_pendingRequestSerial = requestSerial;
    m_waitingForPage = true;

    const ChatId chatId = m_forums[m_forumIndex];
    auto request = td::td_api::make_object<td::td_api::getForumTopics>(
        chatId.value(), std::string(), m_cursor.date, m_cursor.messageId,
        m_cursor.topicId, FORUM_TOPICS_PAGE_SIZE);
    const std::weak_ptr<RoomListSession> weakSession = shared_from_this();
    owner->transceiver().sendQuery(
        std::move(request),
        [weakSession, requestSerial, generation](
            uint64_t, td::td_api::object_ptr<td::td_api::Object> response) {
            const std::shared_ptr<RoomListSession> session =
                weakSession.lock();
            if (session) {
                session->handlePage(
                    requestSerial, generation, std::move(response));
            }
        });
}

void RoomListSession::handlePage(
    uint64_t requestSerial, uint64_t metadataGeneration,
    td::td_api::object_ptr<td::td_api::Object> response)
{
    if (!m_active || !m_waitingForPage ||
        requestSerial != m_pendingRequestSerial) {
        return;
    }
    m_waitingForPage = false;

    std::shared_ptr<ForumTopicsAdapterCore> owner = m_owner.lock();
    if (!owner || owner->shuttingDown())
        return;
    const ChatId chatId = m_forums[m_forumIndex];
    if (!response || response->get_id() != td::td_api::forumTopics::ID) {
        if (response && response->get_id() == td::td_api::error::ID) {
            const auto &error =
                static_cast<const td::td_api::error &>(*response);
            purple_debug_warning(
                config::pluginId,
                "Failed to list forum topics for chat %"
                G_GINT64_FORMAT " with error code %d\n",
                chatId.value(), error.code_);
        } else {
            purple_debug_warning(
                config::pluginId,
                "Failed to list forum topics for chat %"
                G_GINT64_FORMAT " with response type %d\n",
                chatId.value(), response ? response->get_id() : 0);
        }
        advanceForum();
        return;
    }

    const td::td_api::chat *parent = nullptr;
    if (!owner->findForumParent(chatId, parent)) {
        purple_debug_misc(
            config::pluginId,
            "Ignoring forum topic page because parent chat %"
            G_GINT64_FORMAT " is no longer eligible\n",
            chatId.value());
        advanceForum();
        return;
    }

    const auto &page = static_cast<const td::td_api::forumTopics &>(*response);
    std::size_t invalidTopicCount = 0;
    for (const auto &topic : page.topics_) {
        if (!topic || !topic->info_) {
            m_reconciliationSafe = false;
            ++invalidTopicCount;
            continue;
        }

        ForumTopicMetadata metadata;
        if (!adaptForumTopicInfo(*topic->info_, metadata) ||
            metadata.target.chatId() != chatId) {
            m_reconciliationSafe = false;
            ++invalidTopicCount;
            continue;
        }
        m_seenTargets.insert(metadata.target);
        const TdAccountData::ForumTopicState *storedTopic =
            owner->upsertMetadata(metadata, metadataGeneration);
        if (storedTopic)
            projectTopic(*storedTopic);
        if (!m_active)
            return;
    }
    if (invalidTopicCount != 0) {
        purple_debug_warning(
            config::pluginId,
            "Ignored %lu invalid forum topic entries for chat %"
            G_GINT64_FORMAT "\n",
            static_cast<unsigned long>(invalidTopicCount),
            chatId.value());
    }

    if (page.topics_.empty()) {
        if (m_reconciliationSafe) {
            owner->account().reconcileForumTopics(
                chatId, m_seenTargets, metadataGeneration);
        } else {
            purple_debug_warning(
                config::pluginId,
                "Kept cached forum topics for chat %" G_GINT64_FORMAT
                " because the listing contained invalid entries\n",
                chatId.value());
        }
        advanceForum();
        return;
    }

    ForumTopicCursor nextCursor;
    nextCursor.date = page.next_offset_date_;
    nextCursor.messageId = page.next_offset_message_id_;
    nextCursor.topicId = page.next_offset_forum_topic_id_;
    if (m_requestedCursors.find(nextCursor) != m_requestedCursors.end()) {
        purple_debug_warning(
            config::pluginId,
            "Stopping forum topic discovery for chat %" G_GINT64_FORMAT
            " after a pagination cursor cycle\n",
            chatId.value());
        advanceForum();
        return;
    }

    m_cursor = nextCursor;
    requestPage();
}

void RoomListSession::advanceForum()
{
    if (!m_active)
        return;

    std::shared_ptr<ForumTopicsAdapterCore> owner = m_owner.lock();
    if (!owner || owner->shuttingDown())
        return;

    ++m_forumIndex;
    m_cursor = ForumTopicCursor();
    m_requestedCursors.clear();
    m_seenTargets.clear();
    m_reconciliationSafe = true;
    if (m_forumIndex >= m_forums.size())
        owner->finish(shared_from_this());
    else
        requestPage();
}

}

bool adaptForumTopicInfo(const td::td_api::forumTopicInfo &info,
                         ForumTopicMetadata &result)
{
    const ChatId chatId = chatIdFromValue(info.chat_id_);
    const ForumTopicId topicId = ForumTopicId::fromValue(info.forum_topic_id_);
    if (!chatId.valid() || !topicId.valid())
        return false;

    const bool isGeneralId = topicId == ForumTopicId::general();
    if (info.is_general_ != isGeneralId)
        return false;

    ForumTopicMetadata converted;
    converted.target = ChatTarget::forumTopic(chatId, topicId);
    converted.name = info.name_;
    converted.closed = info.is_closed_;
    converted.hidden = info.is_hidden_;
    result = std::move(converted);
    return true;
}

struct ForumTopicsAdapter::Impl {
    Impl(ForumTopicsAdapter *adapter, TdTransceiver &transceiver,
         TdAccountData &account)
        : core(std::make_shared<ForumTopicsAdapterCore>(
              adapter, transceiver, account))
    {}

    std::shared_ptr<ForumTopicsAdapterCore> core;
};

ForumTopicsAdapter::ForumTopicsAdapter(
    TdTransceiver &transceiver, TdAccountData &account)
    : m_impl(new Impl(this, transceiver, account))
{}

ForumTopicsAdapter::~ForumTopicsAdapter()
{
    shutdown();
}

void ForumTopicsAdapter::markRoomListsPending()
{
    const std::shared_ptr<ForumTopicsAdapterCore> core = m_impl->core;
    core->markRoomListsPending();
}

void ForumTopicsAdapter::markRoomListsReady()
{
    const std::shared_ptr<ForumTopicsAdapterCore> core = m_impl->core;
    core->markRoomListsReady();
}

void ForumTopicsAdapter::startRoomList(PurpleRoomlist *roomList)
{
    const std::shared_ptr<ForumTopicsAdapterCore> core = m_impl->core;
    core->startRoomList(roomList);
}

void ForumTopicsAdapter::cancelRoomList(PurpleRoomlist *roomList)
{
    const std::shared_ptr<ForumTopicsAdapterCore> core = m_impl->core;
    core->cancelRoomList(roomList);
}

void ForumTopicsAdapter::shutdown()
{
    if (!m_impl)
        return;

    const std::shared_ptr<ForumTopicsAdapterCore> core = m_impl->core;
    core->shutdown();
}

void ForumTopicsAdapter::processUpdate(
    const td::td_api::updateForumTopicInfo &update)
{
    const std::shared_ptr<ForumTopicsAdapterCore> core = m_impl->core;
    core->processUpdate(update);
}
