#include "FollowingModel.h"

#include <algorithm>
#include <utility>

#include <QDateTime>
#include <QPointer>

#include "Backend.h"
#include "SidebarService.h"
#include "Storage.h"
#include "types/BackendChannel.h"
#include "types/BackendPost.h"

namespace Mattermost {
namespace {

constexpr int ThreadRefreshDelayMs = 300;
const QString ModelObjectName = QStringLiteral("mattermostFollowingModel");

uint64_t nowMs()
{
    return static_cast<uint64_t>(QDateTime::currentMSecsSinceEpoch());
}

} // namespace

FollowingModel& FollowingModel::instance(Backend& backend)
{
    if (auto* existing = backend.findChild<FollowingModel*>(
            ModelObjectName, Qt::FindDirectChildrenOnly)) {
        return *existing;
    }

    auto* model = new FollowingModel(backend);
    model->setObjectName(ModelObjectName);
    return *model;
}

FollowingModel::FollowingModel(Backend& backend)
    : QObject(&backend)
    , backend_(backend)
{
    threadRefreshTimer_.setSingleShot(true);
    threadRefreshTimer_.setInterval(ThreadRefreshDelayMs);
    connect(&threadRefreshTimer_, &QTimer::timeout,
            this, &FollowingModel::refreshThreads);

    connect(&backend_, &Backend::onNewPost, this,
            [this](BackendChannel& channel, const BackendPost& post) {
        noteIncomingPost(channel, post);
        syncConversations();
        emit changed();

        if (!post.root_id.isEmpty() || post.currentUserMentioned) {
            scheduleThreadRefresh();
        }
    });

    connect(&backend_, &Backend::onChannelViewed, this,
            [this](const BackendChannel& channel) {
        clearSyntheticMentions(channel.id);
        syncConversations();
        emit changed();
        scheduleThreadRefresh();
    });

    connect(&backend_, &Backend::onWebSocketConnect,
            this, &FollowingModel::scheduleThreadRefresh);
    connect(&backend_, &Backend::onAllTeamChannelsPopulated,
            this, &FollowingModel::scheduleThreadRefresh);

    auto& sidebar = SidebarService::instance(backend_);
    connect(&sidebar, &SidebarService::channelActivityChanged, this,
            [this](const QString&) { refresh(); });
    connect(&sidebar, &SidebarService::channelActivityReset,
            this, &FollowingModel::refresh);

    auto& followService = ThreadFollowService::instance(backend_);
    connect(&followService, &ThreadFollowService::followingChanged, this,
            [this](const QString&, const QString& threadId, bool following) {
        if (!following) {
            for (auto it = entries_.begin(); it != entries_.end();) {
                if (it->isThread() && it->threadId == threadId) {
                    it = entries_.erase(it);
                } else {
                    ++it;
                }
            }
            emit changed();
        }
        scheduleThreadRefresh();
    });

    syncConversations();

    // Recipient-specific mention flags can predate construction of the sidebar.
    for (auto it = backend_.getStorage().channels.cbegin();
         it != backend_.getStorage().channels.cend(); ++it) {
        BackendChannel* channel = it.value();
        if (!channel) {
            continue;
        }
        for (const BackendPost& post : channel->posts) {
            noteSyntheticMention(*channel, post);
        }
    }

    threadSnapshotDirty_ = true;
    if (!backend_.getStorage().teams.empty()) {
        scheduleThreadRefresh();
    }
}

const FollowingModel::Entry* FollowingModel::findEntry(const QString& channelId,
                                                        const QString& threadId) const
{
    for (const Entry& entry : entries_) {
        if (entry.channelId == channelId && entry.threadId == threadId) {
            return &entry;
        }
    }
    return nullptr;
}

FollowingModel::Entry* FollowingModel::findEntryMutable(const QString& channelId,
                                                         const QString& threadId)
{
    for (Entry& entry : entries_) {
        if (entry.channelId == channelId && entry.threadId == threadId) {
            return &entry;
        }
    }
    return nullptr;
}

const FollowingModel::Entry* FollowingModel::findThread(const QString& threadId) const
{
    for (const Entry& entry : entries_) {
        if (entry.isThread() && entry.threadId == threadId) {
            return &entry;
        }
    }
    return nullptr;
}

FollowingModel::Entry* FollowingModel::findThreadMutable(const QString& threadId)
{
    for (Entry& entry : entries_) {
        if (entry.isThread() && entry.threadId == threadId) {
            return &entry;
        }
    }
    return nullptr;
}

void FollowingModel::refresh()
{
    syncConversations();
    emit changed();
}

void FollowingModel::syncConversations()
{
    QVector<Entry> next;
    next.reserve(entries_.size() + backend_.getStorage().channels.size());

    // Thread membership comes from CRT and is preserved until the next shared
    // thread snapshot. DM/GM entries are ephemeral Following rows: once they are
    // read (or muted), discard the entry and its local resume cursor together.
    for (const Entry& entry : std::as_const(entries_)) {
        if (entry.isThread()) {
            next.push_back(entry);
        }
    }

    auto& sidebar = SidebarService::instance(backend_);
    for (auto it = backend_.getStorage().channels.cbegin();
         it != backend_.getStorage().channels.cend(); ++it) {
        BackendChannel* channel = it.value();
        if (!channel
            || (channel->type != BackendChannel::directChannel
                && channel->type != BackendChannel::groupChannel)) {
            continue;
        }

        const bool unread = sidebar.isChannelUnread(*channel);
        const bool mentioned = sidebar.hasUnreadMention(channel->id);
        const bool muted = sidebar.isChannelMuted(*channel);
        if (muted || (!unread && !mentioned)) {
            continue;
        }

        Entry entry;
        if (const Entry* old = findEntry(channel->id)) {
            entry = *old;
        }
        const bool wasAttention = entry.requiresAttention();
        entry.kind = Kind::Conversation;
        entry.channelId = channel->id;
        entry.threadId.clear();
        entry.teamId.clear();
        entry.unread = unread;
        entry.mentioned = mentioned;
        entry.muted = false;
        entry.unreadReplies = 0;
        entry.unreadMentions = 0;
        entry.synthetic = false;
        entry.urgent = false;
        entry.lastReplyAt = sidebar.channelActivityTime(*channel);
        if (entry.lastReplyAt == 0) {
            entry.lastReplyAt = channel->last_post_at;
        }
        noteAttentionTransition(entry, wasAttention);
        next.push_back(std::move(entry));
    }

    entries_ = std::move(next);
}

void FollowingModel::noteAttentionTransition(Entry& entry, bool wasAttention)
{
    const bool attention = entry.requiresAttention();
    if (!attention) {
        entry.attentionSince = 0;
        return;
    }
    if (!wasAttention || entry.attentionSince == 0) {
        entry.attentionSince = entry.lastReplyAt != 0 ? entry.lastReplyAt : nowMs();
    }
}

void FollowingModel::noteIncomingPost(BackendChannel& channel,
                                      const BackendPost& post)
{
    if (post.id.isEmpty()) {
        return;
    }

    if (!post.isOwnPost()) {
        if (Entry* conversation = findEntryMutable(channel.id, QString())) {
            noteIncomingForResume(*conversation, post);
        }
        if (!post.root_id.isEmpty()) {
            if (Entry* thread = findThreadMutable(post.root_id)) {
                noteIncomingForResume(*thread, post);
            }
        }
    }

    noteSyntheticMention(channel, post);
}

void FollowingModel::noteIncomingForResume(Entry& entry, const BackendPost& post)
{
    if (entry.resumeState != ResumeState::AtEnd) {
        return;
    }
    if (!entry.readThroughPostId.isEmpty()
        && !isAfter(post.create_at, post.id,
                    entry.readThroughCreateAt, entry.readThroughPostId)) {
        return;
    }

    entry.resumeState = ResumeState::FirstUnread;
    entry.firstUnreadPostId = post.id;
}

void FollowingModel::noteSyntheticMention(BackendChannel& channel,
                                           const BackendPost& post)
{
    if (!post.currentUserMentioned || !post.root_id.isEmpty()) {
        return;
    }
    if (channel.type == BackendChannel::directChannel
        || channel.type == BackendChannel::groupChannel) {
        return;
    }

    Entry* existing = findThreadMutable(post.id);
    if (existing && !existing->synthetic) {
        return;
    }

    if (!existing) {
        Entry entry;
        entry.kind = Kind::Thread;
        entry.channelId = channel.id;
        entry.threadId = post.id;
        entry.teamId = channel.team ? channel.team->id : QString();
        entry.synthetic = true;
        entries_.push_back(std::move(entry));
        existing = &entries_.last();
    }

    const bool wasAttention = existing->requiresAttention();
    existing->authorId = post.user_id;
    existing->message = post.message;
    existing->lastReplyAt = post.create_at;
    existing->unreadMentions = 1;
    existing->mentioned = true;
    existing->muted = SidebarService::instance(backend_).isChannelMuted(channel);
    noteAttentionTransition(*existing, wasAttention);
}

void FollowingModel::clearSyntheticMentions(const QString& channelId)
{
    for (auto it = entries_.begin(); it != entries_.end();) {
        if (it->isThread() && it->synthetic && it->channelId == channelId) {
            it = entries_.erase(it);
        } else {
            ++it;
        }
    }
}

void FollowingModel::consumeSyntheticThread(const QString& threadId)
{
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (it->isThread() && it->synthetic && it->threadId == threadId) {
            entries_.erase(it);
            emit changed();
            scheduleThreadRefresh();
            return;
        }
    }
}

void FollowingModel::scheduleThreadRefresh()
{
    threadSnapshotDirty_ = true;
    if (!threadRefreshTimer_.isActive()) {
        threadRefreshTimer_.start();
    }
}

void FollowingModel::ensureThreadsFresh()
{
    if (threadSnapshotDirty_) {
        scheduleThreadRefresh();
    }
}

void FollowingModel::refreshThreads()
{
    if (threadRefreshInFlight_) {
        threadRefreshRequested_ = true;
        threadSnapshotDirty_ = true;
        return;
    }

    threadRefreshInFlight_ = true;
    threadRefreshRequested_ = false;
    threadSnapshotDirty_ = false;

    QPointer<FollowingModel> guard(this);
    ThreadFollowService::instance(backend_).queryFollowingThreads(
        [guard](QVector<ThreadFollowService::ThreadSummary> threads) {
            if (!guard) {
                return;
            }
            guard->applyThreadSnapshot(std::move(threads));
            guard->threadRefreshInFlight_ = false;
            emit guard->changed();

            if (guard->threadRefreshRequested_) {
                guard->threadRefreshRequested_ = false;
                guard->scheduleThreadRefresh();
            } else if (guard->threadSnapshotDirty_) {
                guard->scheduleThreadRefresh();
            }
        });
}

void FollowingModel::applyThreadSnapshot(
    QVector<ThreadFollowService::ThreadSummary> threads)
{
    QVector<Entry> next;
    next.reserve(entries_.size() + threads.size());

    // Conversations are not part of the CRT response.
    for (const Entry& entry : std::as_const(entries_)) {
        if (!entry.isThread()) {
            next.push_back(entry);
        }
    }

    // Normalize pagination overlap without introducing an auxiliary identity map;
    // the loaded Following page is deliberately small and a linear lookup keeps
    // ownership in one structure.
    for (const ThreadFollowService::ThreadSummary& thread : std::as_const(threads)) {
        if (thread.id.isEmpty() || thread.channelId.isEmpty()) {
            continue;
        }

        int existingNextIndex = -1;
        for (int i = 0; i < next.size(); ++i) {
            if (next.at(i).isThread() && next.at(i).threadId == thread.id) {
                existingNextIndex = i;
                break;
            }
        }

        Entry entry;
        if (const Entry* old = findThread(thread.id)) {
            entry = *old;
        }
        const bool wasAttention = entry.requiresAttention();
        const bool readPending = entry.readAcknowledgementPending;

        entry.kind = Kind::Thread;
        entry.channelId = thread.channelId;
        entry.threadId = thread.id;
        entry.teamId = thread.teamId;
        entry.authorId = thread.authorId;
        entry.message = thread.message;
        entry.lastViewedAt = thread.lastViewedAt;
        entry.lastReplyAt = thread.lastReplyAt;
        entry.unreadReplies = thread.unreadReplies;
        entry.unreadMentions = thread.unreadMentions;
        entry.mentioned = thread.unreadMentions > 0;
        entry.urgent = thread.urgent;
        entry.synthetic = false;

        BackendChannel* channel = backend_.getStorage().getChannelById(thread.channelId);
        entry.muted = channel
            ? SidebarService::instance(backend_).isChannelMuted(*channel)
            : false;

        if (readPending) {
            if (entry.unreadReplies <= 0 && entry.unreadMentions <= 0) {
                entry.readAcknowledgementPending = false;
                entry.readAcknowledgementAt = 0;
            } else if (entry.lastReplyAt <= entry.readAcknowledgementAt) {
                // This can be a CRT response that started before our read PUT.
                entry.unreadReplies = 0;
                entry.unreadMentions = 0;
                entry.mentioned = false;
                entry.readAcknowledgementPending = true;
            } else {
                // A genuinely newer reply arrived after the local read watermark.
                entry.readAcknowledgementPending = false;
                entry.readAcknowledgementAt = 0;
            }
        }

        noteAttentionTransition(entry, wasAttention);
        if (existingNextIndex >= 0) {
            next[existingNextIndex] = std::move(entry);
        } else {
            next.push_back(std::move(entry));
        }
    }

    // Keep only temporary root mentions that have not yet been replaced by a
    // real server ThreadResponse. They are already part of the shared structure.
    for (const Entry& old : std::as_const(entries_)) {
        if (!old.isThread() || !old.synthetic) {
            continue;
        }
        bool replaced = false;
        for (const Entry& candidate : std::as_const(next)) {
            if (candidate.isThread() && candidate.threadId == old.threadId) {
                replaced = true;
                break;
            }
        }
        if (!replaced) {
            next.push_back(old);
        }
    }

    entries_ = std::move(next);
}

bool FollowingModel::isAfter(uint64_t lhsCreateAt,
                             const QString& lhsId,
                             uint64_t rhsCreateAt,
                             const QString& rhsId)
{
    if (lhsCreateAt != rhsCreateAt) {
        return lhsCreateAt > rhsCreateAt;
    }
    return lhsId > rhsId;
}

QString FollowingModel::nextCachedPostId(const BackendChannel& channel,
                                         const QString& threadId,
                                         const BackendPost& after) const
{
    const BackendPost* next = nullptr;
    for (const BackendPost& candidate : channel.posts) {
        if (candidate.isDeleted || candidate.id.isEmpty()) {
            continue;
        }
        if (!threadId.isEmpty()
            && candidate.id != threadId
            && candidate.root_id != threadId) {
            continue;
        }
        if (!isAfter(candidate.create_at, candidate.id,
                     after.create_at, after.id)) {
            continue;
        }
        if (!next
            || isAfter(next->create_at, next->id,
                       candidate.create_at, candidate.id)) {
            next = &candidate;
        }
    }
    return next ? next->id : QString();
}

void FollowingModel::observeReadThrough(const QString& channelId,
                                        const QString& threadId,
                                        const BackendPost& post,
                                        bool sourceAtEnd)
{
    Entry* entry = findEntryMutable(channelId, threadId);
    if (!entry || post.id.isEmpty()) {
        // Deliberately do not allocate anything here. A random channel/thread
        // that is not represented by Following has no resume state to maintain.
        return;
    }

    const bool sameBoundary = entry->readThroughPostId == post.id;
    if (!entry->readThroughPostId.isEmpty() && !sameBoundary
        && !isAfter(post.create_at, post.id,
                    entry->readThroughCreateAt, entry->readThroughPostId)) {
        return;
    }

    BackendChannel* channel = backend_.getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    const ResumeState oldState = entry->resumeState;
    const QString oldFirstUnread = entry->firstUnreadPostId;
    const QString oldReadThrough = entry->readThroughPostId;
    const uint64_t oldReadThroughCreateAt = entry->readThroughCreateAt;

    entry->readThroughPostId = post.id;
    entry->readThroughCreateAt = post.create_at;

    const QString nextPostId = nextCachedPostId(*channel, threadId, post);
    if (!nextPostId.isEmpty()) {
        entry->resumeState = ResumeState::FirstUnread;
        entry->firstUnreadPostId = nextPostId;
    } else if (sourceAtEnd) {
        entry->resumeState = ResumeState::AtEnd;
        entry->firstUnreadPostId.clear();
    } else {
        // The logical source says there is more content but has not materialized
        // the next semantic identity yet. Retain the high-water boundary only.
        entry->resumeState = ResumeState::Unknown;
        entry->firstUnreadPostId.clear();
    }

    if (oldState != entry->resumeState
        || oldFirstUnread != entry->firstUnreadPostId
        || oldReadThrough != entry->readThroughPostId
        || oldReadThroughCreateAt != entry->readThroughCreateAt) {
        emit changed();
    }
}

void FollowingModel::markThreadRead(const QString& teamId,
                                    const QString& threadId,
                                    std::function<void(bool)> callback)
{
    if (teamId.isEmpty() || threadId.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    if (Entry* entry = findThreadMutable(threadId)) {
        entry->unreadReplies = 0;
        entry->unreadMentions = 0;
        entry->mentioned = false;
        entry->attentionSince = 0;
        entry->readAcknowledgementPending = true;
        entry->readAcknowledgementAt = nowMs();
        emit changed();
    }

    QPointer<FollowingModel> guard(this);
    ThreadFollowService::instance(backend_).markThreadRead(
        teamId, threadId,
        [guard, threadId, callback = std::move(callback)](bool success) mutable {
            if (!guard) {
                return;
            }

            if (!success) {
                if (Entry* entry = guard->findThreadMutable(threadId)) {
                    entry->readAcknowledgementPending = false;
                    entry->readAcknowledgementAt = 0;
                }
                guard->scheduleThreadRefresh();
            } else {
                // Keep the watermark until a shared CRT snapshot confirms it.
                guard->scheduleThreadRefresh();
            }

            if (callback) {
                callback(success);
            }
        });
}

} // namespace Mattermost
