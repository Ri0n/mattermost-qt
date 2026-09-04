#include "ThreadPostSource.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QPointer>

#include "backend/Backend.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

namespace {

QVector<BackendPost*> cachedThreadReplies(const BackendChannel& channel, const QString& rootId)
{
    QVector<BackendPost*> result;
    for (const BackendPost& post : channel.posts) {
        // BackendChannel marks replies hidden so the main channel renders only
        // root posts. That flag is expected on thread replies and must not hide
        // them from the thread's own logical sequence.
        if (post.root_id == rootId) {
            BackendPost* cached = channel.postIdToPost.value(post.id, nullptr);
            if (cached) {
                result.push_back(cached);
            }
        }
    }
    std::sort(result.begin(), result.end(), [](const BackendPost* lhs, const BackendPost* rhs) {
        if (lhs->create_at != rhs->create_at) {
            return lhs->create_at < rhs->create_at;
        }
        return lhs->id < rhs->id;
    });
    return result;
}

} // namespace

ThreadPostSource::ThreadPostSource(Backend& backendInstance,
                                   BackendChannel& channelInstance,
                                   QString sourceRootId,
                                   QObject* parent)
    : AbstractPostSource(parent)
    , backend(backendInstance)
    , channel(channelInstance)
    , rootId(std::move(sourceRootId))
{
    postIds.resize(currentLogicalCount());
    if (!postIds.isEmpty() && rootPost()) {
        postIds[0] = rootId;
    }
    seedCachedPosts();

    connect(&channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& post) { appendLiveReply(post); });
    connect(&channel, &BackendChannel::onPostEdited, this,
            [this](BackendPost& post) {
        const int index = indexOfPost(post.id);
        if (index >= 0) {
            emit itemsChanged(index, index);
        }
        if (post.id == rootId) {
            const int count = currentLogicalCount();
            if (count != static_cast<int>(postIds.size())) {
                postIds.resize(count);
                if (!postIds.isEmpty()) {
                    postIds[0] = rootId;
                }
                rebuildIndex();
                emit itemCountChanged(count);
            }
        }
    });
    connect(&channel, &BackendChannel::onPostReactionUpdated, this,
            [this](BackendPost& post) {
        const int index = indexOfPost(post.id);
        if (index >= 0) {
            emit itemsChanged(index, index);
        }
    });
    connect(&channel, &BackendChannel::onPostDeleted, this,
            [this](const QString& postId) {
        const int index = indexOfPost(postId);
        if (index >= 0) {
            emit itemsChanged(index, index);
        }
    });
}

bool ThreadPostSource::isAvailable(int index) const
{
    return index >= 0 && index < postIds.size() && !postIds.at(index).isEmpty()
        && channel.postIdToPost.contains(postIds.at(index));
}

BackendPost* ThreadPostSource::postAt(int index) const
{
    if (!isAvailable(index)) {
        return nullptr;
    }
    return channel.postIdToPost.value(postIds.at(index), nullptr);
}

int ThreadPostSource::indexOfPost(const QString& postId) const
{
    return postIndexes.value(postId, -1);
}

int ThreadPostSource::ensurePostIndex(const QString& postId)
{
    const int existing = indexOfPost(postId);
    if (existing >= 0) {
        return existing;
    }

    BackendPost* post = channel.postIdToPost.value(postId, nullptr);
    if (!post || postIds.isEmpty()
        || (post->id != rootId && post->root_id != rootId)) {
        return -1;
    }
    if (post->id == rootId) {
        postIds[0] = rootId;
        rebuildIndex();
        emit rangeAvailable(0, 0);
        return 0;
    }

    const int index = nearestEmptyIndex(estimatedIndexForPost(*post));
    if (index < 1) {
        return -1;
    }
    postIds[index] = postId;
    rebuildIndex();
    emit rangeAvailable(index, index);
    return index;
}

void ThreadPostSource::requestRange(int first,
                                    int last,
                                    RequestReason reason,
                                    quint64 generation)
{
    Q_UNUSED(reason)
    Q_UNUSED(generation)

    if (postIds.isEmpty() || !rootPost()) {
        emit rangeRequestFinished(first, last);
        return;
    }

    const int requestedFirst = std::max(0, first);
    const int requestedLast = std::min(static_cast<int>(postIds.size()) - 1, last);
    if (requestedLast < requestedFirst) {
        emit rangeRequestFinished(first, last);
        return;
    }

    if (requestedFirst == 0 && !postIds.isEmpty()) {
        postIds[0] = rootId;
        rebuildIndex();
        emit rangeAvailable(0, 0);
    }

    // The two real boundaries are authoritative. Middle seeks intentionally use
    // only an approximate timestamp mapping; LongListWidget owns the actual
    // visual target/geometry and never sees this server-specific approximation.
    QPointer<ThreadPostSource> guard(this);
    BackendPost* root = rootPost();
    if (requestedLast >= static_cast<int>(postIds.size()) - ServerBlockSize) {
        PostTimelineService::instance(backend).loadThreadTail(
            channel, rootId, ServerBlockSize, root->last_reply_at,
            [guard, first, last](const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }
                if (result.success && !result.postIds.isEmpty()) {
                    guard->placeTail(result.postIds);
                }
                emit guard->rangeRequestFinished(first, last);
            });
        return;
    }

    if (requestedFirst <= 1) {
        PostTimelineService::instance(backend).loadThreadPage(
            channel, rootId, ServerBlockSize, QString(), 0,
            [guard, first, last](const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }
                if (result.success && !result.postIds.isEmpty()) {
                    guard->placeInitial(result.postIds);
                }
                emit guard->rangeRequestFinished(first, last);
            });
        return;
    }

    const int target = (requestedFirst + requestedLast) / 2;
    PostTimelineService::instance(backend).loadThreadFromTime(
        channel, rootId, ServerBlockSize, estimatedCreateAt(target),
        [guard, target, first, last](const PostTimelineService::Page& result) {
            if (!guard) {
                return;
            }
            if (result.success && !result.postIds.isEmpty()) {
                guard->placeApproximate(target, result.postIds);
            }
            emit guard->rangeRequestFinished(first, last);
        });
}

BackendPost* ThreadPostSource::rootPost() const
{
    return channel.postIdToPost.value(rootId, nullptr);
}

int ThreadPostSource::currentLogicalCount() const
{
    BackendPost* root = rootPost();
    if (!root) {
        return 0;
    }
    const int64_t boundedReplies = std::min<int64_t>(root->reply_count,
                                                     std::numeric_limits<int>::max() - 1);
    return std::max(1, static_cast<int>(boundedReplies) + 1);
}

int ThreadPostSource::nearestEmptyIndex(int preferred) const
{
    const int count = static_cast<int>(postIds.size());
    if (count <= 1) {
        return -1;
    }
    preferred = std::max(1, std::min(preferred, count - 1));
    if (postIds.at(preferred).isEmpty()) {
        return preferred;
    }
    for (int distance = 1; distance < count; ++distance) {
        const int before = preferred - distance;
        if (before >= 1 && postIds.at(before).isEmpty()) {
            return before;
        }
        const int after = preferred + distance;
        if (after < count && postIds.at(after).isEmpty()) {
            return after;
        }
    }
    return -1;
}

void ThreadPostSource::seedCachedPosts()
{
    BackendPost* root = rootPost();
    if (!root || postIds.isEmpty()) {
        return;
    }

    postIds[0] = rootId;
    const QVector<BackendPost*> replies = cachedThreadReplies(channel, rootId);
    if (replies.isEmpty()) {
        rebuildIndex();
        emit rangeAvailable(0, 0);
        return;
    }

    if (postIds.size() - 1 == replies.size()) {
        for (int i = 0; i < replies.size(); ++i) {
            postIds[i + 1] = replies.at(i)->id;
        }
        rebuildIndex();
        emit rangeAvailable(0, static_cast<int>(postIds.size()) - 1);
        return;
    }

    const BackendPost* newest = replies.last();
    if (root->last_reply_at != 0 && newest->create_at >= root->last_reply_at) {
        const int first = std::max(1,
            static_cast<int>(postIds.size()) - static_cast<int>(replies.size()));
        const int count = std::min(static_cast<int>(replies.size()),
                                   static_cast<int>(postIds.size()) - first);
        for (int offset = 0; offset < count; ++offset) {
            postIds[first + offset] = replies.at(replies.size() - count + offset)->id;
        }
        rebuildIndex();
        emit rangeAvailable(0, 0);
        emit rangeAvailable(first, first + count - 1);
        return;
    }

    rebuildIndex();
    emit rangeAvailable(0, 0);
}

void ThreadPostSource::rebuildIndex()
{
    postIndexes.clear();
    for (int index = 0; index < postIds.size(); ++index) {
        if (!postIds.at(index).isEmpty()) {
            postIndexes.insert(postIds.at(index), index);
        }
    }
}

void ThreadPostSource::placeInitial(const QStringList& ids)
{
    if (postIds.isEmpty() || ids.isEmpty()) {
        return;
    }

    int first = ids.first() == rootId ? 0 : 1;
    const int count = std::min(static_cast<int>(ids.size()),
                               static_cast<int>(postIds.size()) - first);
    const int last = first + count - 1;

    for (int offset = 0; offset < count; ++offset) {
        const QString& id = ids.at(offset);
        const int existing = postIndexes.value(id, -1);
        if (existing >= 0 && (existing < first || existing > last)) {
            postIds[existing].clear();
            emit itemsChanged(existing, existing);
        }
    }
    for (int offset = 0; offset < count; ++offset) {
        postIds[first + offset] = ids.at(offset);
    }
    if (first > 0) {
        postIds[0] = rootId;
    }
    rebuildIndex();
    emit itemsChanged(first, last);
    emit rangeAvailable(0, std::max(0, last));
}

void ThreadPostSource::placeTail(const QStringList& ids)
{
    if (postIds.size() <= 1 || ids.isEmpty()) {
        return;
    }
    const int count = std::min(static_cast<int>(ids.size()),
                               static_cast<int>(postIds.size()) - 1);
    const int first = static_cast<int>(postIds.size()) - count;
    const int last = first + count - 1;

    for (int offset = 0; offset < count; ++offset) {
        const QString& id = ids.at(ids.size() - count + offset);
        const int existing = postIndexes.value(id, -1);
        if (existing >= 0 && (existing < first || existing > last)) {
            postIds[existing].clear();
            emit itemsChanged(existing, existing);
        }
        postIds[first + offset] = id;
    }
    rebuildIndex();
    emit itemsChanged(first, last);
    emit rangeAvailable(first, last);
}

void ThreadPostSource::placeApproximate(int targetIndex, const QStringList& ids)
{
    if (postIds.size() <= 1 || ids.isEmpty()) {
        return;
    }
    const int count = std::min(static_cast<int>(ids.size()),
                               static_cast<int>(postIds.size()) - 1);
    int first = targetIndex - (count - 1) / 2;
    first = std::max(1, std::min(first, static_cast<int>(postIds.size()) - count));
    const int last = first + count - 1;

    for (int offset = 0; offset < count; ++offset) {
        const QString& id = ids.at(offset);
        const int existing = postIndexes.value(id, -1);
        if (existing >= 0 && (existing < first || existing > last)) {
            postIds[existing].clear();
            emit itemsChanged(existing, existing);
        }
        postIds[first + offset] = id;
    }
    rebuildIndex();
    emit itemsChanged(first, last);
    emit rangeAvailable(first, last);
}

uint64_t ThreadPostSource::estimatedCreateAt(int logicalIndex) const
{
    BackendPost* root = rootPost();
    if (!root || postIds.size() <= 1) {
        return root ? root->create_at : 0;
    }

    logicalIndex = std::max(1, std::min(logicalIndex, static_cast<int>(postIds.size()) - 1));
    const uint64_t oldest = root->create_at;
    const uint64_t newest = std::max(root->last_reply_at, oldest);
    if (newest <= oldest) {
        return oldest;
    }

    const long double fraction = static_cast<long double>(logicalIndex - 1)
        / static_cast<long double>(std::max(1, static_cast<int>(postIds.size()) - 2));
    const long double estimate = static_cast<long double>(oldest)
        + fraction * static_cast<long double>(newest - oldest);
    return static_cast<uint64_t>(std::llround(estimate));
}

int ThreadPostSource::estimatedIndexForPost(const BackendPost& post) const
{
    const int count = static_cast<int>(postIds.size());
    if (post.id == rootId || count <= 1) {
        return 0;
    }
    if (count == 2) {
        return 1;
    }

    BackendPost* root = rootPost();
    if (!root) {
        return std::max(1, count / 2);
    }
    const uint64_t oldest = root->create_at;
    const uint64_t newest = std::max(root->last_reply_at, oldest);
    if (newest <= oldest || post.create_at <= oldest) {
        return 1;
    }
    if (post.create_at >= newest) {
        return count - 1;
    }

    const long double fraction = static_cast<long double>(post.create_at - oldest)
        / static_cast<long double>(newest - oldest);
    return 1 + static_cast<int>(std::llround(fraction * (count - 2)));
}

void ThreadPostSource::appendLiveReply(BackendPost& post)
{
    if (post.root_id != rootId) {
        return;
    }

    const int existing = indexOfPost(post.id);
    if (existing >= 0) {
        emit itemsChanged(existing, existing);
        return;
    }

    int count = currentLogicalCount();
    if (count <= postIds.size()) {
        count = static_cast<int>(postIds.size()) + 1;
    }
    postIds.resize(count);
    if (!postIds.isEmpty()) {
        postIds[0] = rootId;
    }
    const int index = count - 1;
    postIds[index] = post.id;
    rebuildIndex();
    emit itemCountChanged(count);
    emit rangeAvailable(index, index);
}

} // namespace Mattermost
