#include "ReadCursorService.h"

#include <QMap>

#include "Backend.h"
#include "Storage.h"
#include "types/BackendChannel.h"
#include "types/BackendPost.h"

namespace Mattermost {

ReadCursorService& ReadCursorService::instance(Backend& backend)
{
    static QMap<Backend*, ReadCursorService*> instances;
    auto it = instances.find(&backend);
    if (it == instances.end()) {
        it = instances.insert(&backend, new ReadCursorService(backend));
    }
    return **it;
}

ReadCursorService::ReadCursorService(Backend& backend)
    : QObject(&backend)
    , backend_(backend)
{
    connect(&backend_, &Backend::onNewPost, this,
            [this](BackendChannel& channel, const BackendPost& post) {
        noteIncomingPost(channel, post);
    });
}

QString ReadCursorService::key(const QString& channelId, const QString& rootId)
{
    return channelId + QChar(0x1f) + rootId;
}

bool ReadCursorService::isAfter(uint64_t lhsCreateAt,
                                const QString& lhsId,
                                uint64_t rhsCreateAt,
                                const QString& rhsId)
{
    if (lhsCreateAt != rhsCreateAt) {
        return lhsCreateAt > rhsCreateAt;
    }
    return lhsId > rhsId;
}

ReadCursorService::Cursor ReadCursorService::cursor(const QString& channelId,
                                                    const QString& rootId) const
{
    return entries_.value(key(channelId, rootId)).cursor;
}

QString ReadCursorService::nextCachedPostId(const BackendChannel& channel,
                                            const QString& rootId,
                                            const BackendPost& after) const
{
    const BackendPost* next = nullptr;
    for (const BackendPost& candidate : channel.posts) {
        if (candidate.isDeleted || candidate.id.isEmpty()) {
            continue;
        }
        if (!rootId.isEmpty()
            && candidate.id != rootId
            && candidate.root_id != rootId) {
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

void ReadCursorService::observeReadThrough(const QString& channelId,
                                           const QString& rootId,
                                           const BackendPost& post,
                                           bool sourceAtEnd)
{
    if (channelId.isEmpty() || post.id.isEmpty()) {
        return;
    }

    BackendChannel* channel = backend_.getStorage().getChannelById(channelId);
    if (!channel) {
        return;
    }

    Entry& entry = entries_[key(channelId, rootId)];
    Cursor before = entry.cursor;

    const bool sameBoundary = entry.cursor.readThroughPostId == post.id;
    if (!entry.cursor.readThroughPostId.isEmpty() && !sameBoundary
        && !isAfter(post.create_at, post.id,
                    entry.cursor.readThroughCreateAt,
                    entry.cursor.readThroughPostId)) {
        return;
    }

    entry.cursor.readThroughPostId = post.id;
    entry.cursor.readThroughCreateAt = post.create_at;

    const QString nextPostId = nextCachedPostId(*channel, rootId, post);
    if (!nextPostId.isEmpty()) {
        entry.cursor.state = State::FirstUnread;
        entry.cursor.postId = nextPostId;
    } else if (sourceAtEnd) {
        entry.cursor.state = State::AtEnd;
        entry.cursor.postId.clear();
    } else {
        // We know that the user read through this semantic post, but the source
        // still has a logical successor whose identity has not arrived yet.
        // Keep the high-water mark and let a later materialization refine it.
        entry.cursor.state = State::Unknown;
        entry.cursor.postId.clear();
    }

    if (before.state != entry.cursor.state
        || before.postId != entry.cursor.postId
        || before.readThroughPostId != entry.cursor.readThroughPostId
        || before.readThroughCreateAt != entry.cursor.readThroughCreateAt) {
        emit cursorChanged(channelId, rootId);
    }
}

void ReadCursorService::noteIncomingPost(BackendChannel& channel,
                                         const BackendPost& post)
{
    if (post.id.isEmpty() || post.isOwnPost()) {
        return;
    }

    // DM/GM Following rows represent the whole conversation, including replies.
    // Tracking the channel key for every post is harmless for team channels and
    // makes the same service reusable if channel-level Following grows later.
    noteIncomingForKey(channel.id, QString(), post);

    if (!post.root_id.isEmpty()) {
        noteIncomingForKey(channel.id, post.root_id, post);
    }
}

void ReadCursorService::noteIncomingForKey(const QString& channelId,
                                           const QString& rootId,
                                           const BackendPost& post)
{
    const QString cursorKey = key(channelId, rootId);
    auto it = entries_.find(cursorKey);
    if (it == entries_.end() || it->cursor.state != State::AtEnd) {
        return;
    }

    const Cursor before = it->cursor;
    if (!before.readThroughPostId.isEmpty()
        && !isAfter(post.create_at, post.id,
                    before.readThroughCreateAt,
                    before.readThroughPostId)) {
        return;
    }

    it->cursor.state = State::FirstUnread;
    it->cursor.postId = post.id;
    emit cursorChanged(channelId, rootId);
}

} // namespace Mattermost
