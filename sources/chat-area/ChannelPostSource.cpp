#include "ChannelPostSource.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>

#include <QPointer>
#include <QSet>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

namespace {

QVector<BackendPost*> cachedRootPosts(const BackendChannel& channel)
{
    QVector<BackendPost*> result;
    result.reserve(static_cast<int>(channel.posts.size()));
    for (const BackendPost& post : channel.posts) {
        if (!post.hidden && post.root_id.isEmpty()) {
            result.push_back(channel.postIdToPost.value(post.id, nullptr));
        }
    }
    std::sort(result.begin(), result.end(), [](const BackendPost* lhs, const BackendPost* rhs) {
        if (!lhs || !rhs) {
            return lhs != nullptr;
        }
        if (lhs->create_at != rhs->create_at) {
            return lhs->create_at < rhs->create_at;
        }
        return lhs->id < rhs->id;
    });
    result.erase(std::remove(result.begin(), result.end(), nullptr), result.end());
    return result;
}

} // namespace

ChannelPostSource::ChannelPostSource(Backend& backendInstance,
                                     BackendChannel& channelInstance,
                                     QObject* parent)
    : AbstractPostSource(parent)
    , backend(backendInstance)
    , channel(channelInstance)
    , exactRootCount(channelInstance.has_total_msg_count_root)
{
    if (exactRootCount) {
        postIds.resize(currentLogicalCount());
        seedCachedPosts();
    } else {
        // Without total_msg_count_root there is no honest absolute oldest->newest
        // coordinate system yet. Start with at most a provably newest cached root
        // and discover older real rows with cursor requests + logical prepends.
        moreBeforeFirst = true;
        seedUnknownNewestPost();
        QTimer::singleShot(0, this, [this] {
            requestBeforeFirst(RequestReason::Initial, 0);
        });
    }

    connect(&channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& post) { appendLivePost(post); });
    connect(&channel, &BackendChannel::onPostEdited, this,
            [this](BackendPost& post) {
        const int index = indexOfPost(post.id);
        if (index >= 0) {
            emit itemsChanged(index, index);
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
    connect(&channel, &BackendChannel::onNewPosts, this,
            [this](const ChannelNewPosts&) {
        if (!exactRootCount) {
            return;
        }
        const int count = currentLogicalCount();
        if (count != static_cast<int>(postIds.size())) {
            postIds.resize(count);
            rebuildIndex();
            emit itemCountChanged(count);
        }
    });
}

bool ChannelPostSource::isAvailable(int index) const
{
    return index >= 0 && index < postIds.size() && !postIds.at(index).isEmpty()
        && channel.postIdToPost.contains(postIds.at(index));
}

BackendPost* ChannelPostSource::postAt(int index) const
{
    if (!isAvailable(index)) {
        return nullptr;
    }
    return channel.postIdToPost.value(postIds.at(index), nullptr);
}

int ChannelPostSource::indexOfPost(const QString& postId) const
{
    return postIndexes.value(postId, -1);
}

int ChannelPostSource::ensurePostIndex(const QString& postId)
{
    const int existing = indexOfPost(postId);
    if (existing >= 0) {
        return existing;
    }

    BackendPost* post = channel.postIdToPost.value(postId, nullptr);
    if (!post || post->hidden || !post->root_id.isEmpty()) {
        return -1;
    }

    // Estimated absolute placement is meaningful only when the root-post count
    // supplies a stable coordinate system. Unknown-count compatibility mode is
    // a contiguous discovered sequence and must not manufacture adjacency for an
    // arbitrary cached permalink target.
    if (!exactRootCount || postIds.isEmpty()) {
        return -1;
    }

    const int index = nearestEmptyIndex(estimateIndexForPost(*post));
    if (index < 0) {
        return -1;
    }

    postIds[index] = postId;
    rebuildIndex();
    emit rangeAvailable(index, index);
    return index;
}

void ChannelPostSource::requestRange(int first,
                                     int last,
                                     RequestReason reason,
                                     quint64 generation)
{
    Q_UNUSED(reason)
    Q_UNUSED(generation)

    if (!exactRootCount || postIds.isEmpty()) {
        emit rangeRequestFinished(first, last);
        return;
    }

    const int requestedFirst = std::max(0, first);
    const int requestedLast = std::min(static_cast<int>(postIds.size()) - 1, last);
    if (requestedLast < requestedFirst) {
        emit rangeRequestFinished(first, last);
        return;
    }

    const int firstPage = pageForIndex(requestedLast);
    const int lastPage = pageForIndex(requestedFirst);
    const int requestCount = lastPage - firstPage + 1;
    auto remaining = std::make_shared<int>(requestCount);
    QPointer<ChannelPostSource> guard(this);

    for (int page = firstPage; page <= lastPage; ++page) {
        PostTimelineService::instance(backend).loadChannelPage(
            channel, page, ServerPageSize,
            [guard, page, remaining, first, last](const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }
                if (result.success && !result.postIds.isEmpty()) {
                    guard->placePage(page, result.postIds);
                }
                --*remaining;
                if (*remaining == 0) {
                    emit guard->rangeRequestFinished(first, last);
                }
            });
    }
}

bool ChannelPostSource::canRequestBeforeFirst() const
{
    return !exactRootCount && moreBeforeFirst;
}

void ChannelPostSource::requestBeforeFirst(RequestReason reason, quint64 generation)
{
    Q_UNUSED(reason)
    Q_UNUSED(generation)

    if (exactRootCount || !moreBeforeFirst || beforeRequestInFlight) {
        return;
    }

    beforeRequestInFlight = true;
    QPointer<ChannelPostSource> guard(this);
    auto completed = [guard](const PostTimelineService::Page& result) {
        if (!guard) {
            return;
        }
        guard->beforeRequestInFlight = false;
        if (!result.success) {
            return;
        }

        QStringList discovered;
        discovered.reserve(result.postIds.size());
        for (const QString& id : result.postIds) {
            if (!id.isEmpty() && guard->indexOfPost(id) < 0) {
                discovered.push_back(id);
            }
        }

        if (!discovered.isEmpty()) {
            guard->prependDiscovered(discovered);
        }

        // An empty cursor or an empty non-overlapping result is a real boundary
        // for this compatibility walk. Keeping the flag set in that state would
        // cause every top-edge gesture to repeat the same request forever.
        guard->moreBeforeFirst = !result.prevPostId.isEmpty() && !discovered.isEmpty();
    };

    if (postIds.isEmpty()) {
        PostTimelineService::instance(backend).loadChannelPage(
            channel, 0, ServerPageSize, std::move(completed));
        return;
    }

    PostTimelineService::instance(backend).loadChannelBefore(
        channel, postIds.first(), ServerPageSize, std::move(completed));
}

int ChannelPostSource::currentLogicalCount() const
{
    if (exactRootCount) {
        return std::max(0, channel.total_msg_count_root);
    }
    return static_cast<int>(postIds.size());
}

int ChannelPostSource::pageForIndex(int index) const
{
    if (postIds.isEmpty()) {
        return 0;
    }
    index = std::max(0, std::min(index, static_cast<int>(postIds.size()) - 1));
    return (static_cast<int>(postIds.size()) - 1 - index) / ServerPageSize;
}

int ChannelPostSource::estimateIndexForPost(const BackendPost& post) const
{
    const int count = static_cast<int>(postIds.size());
    if (count <= 1) {
        return 0;
    }

    int lowerIndex = -1;
    int upperIndex = -1;
    uint64_t lowerTime = 0;
    uint64_t upperTime = std::numeric_limits<uint64_t>::max();

    for (int index = 0; index < count; ++index) {
        BackendPost* current = postAt(index);
        if (!current || current->id == post.id) {
            continue;
        }
        if (current->create_at <= post.create_at && current->create_at >= lowerTime) {
            lowerTime = current->create_at;
            lowerIndex = index;
        }
        if (current->create_at >= post.create_at && current->create_at <= upperTime) {
            upperTime = current->create_at;
            upperIndex = index;
        }
    }

    if (lowerIndex >= 0 && upperIndex >= 0 && upperIndex > lowerIndex
        && upperTime > lowerTime) {
        const long double fraction = static_cast<long double>(post.create_at - lowerTime)
            / static_cast<long double>(upperTime - lowerTime);
        return lowerIndex + static_cast<int>(std::llround(
            fraction * static_cast<long double>(upperIndex - lowerIndex)));
    }
    if (lowerIndex >= 0) {
        return std::min(count - 1, lowerIndex + 1);
    }
    if (upperIndex >= 0) {
        return std::max(0, upperIndex - 1);
    }

    const uint64_t oldest = channel.create_at;
    const uint64_t newest = std::max(channel.last_post_at, oldest);
    if (newest > oldest && post.create_at >= oldest) {
        const long double fraction = std::min<long double>(1.0L,
            static_cast<long double>(post.create_at - oldest)
                / static_cast<long double>(newest - oldest));
        return static_cast<int>(std::llround(fraction * (count - 1)));
    }
    return count / 2;
}

int ChannelPostSource::nearestEmptyIndex(int preferred) const
{
    const int count = static_cast<int>(postIds.size());
    if (count <= 0) {
        return -1;
    }
    preferred = std::max(0, std::min(preferred, count - 1));
    if (postIds.at(preferred).isEmpty()) {
        return preferred;
    }
    for (int distance = 1; distance < count; ++distance) {
        const int before = preferred - distance;
        if (before >= 0 && postIds.at(before).isEmpty()) {
            return before;
        }
        const int after = preferred + distance;
        if (after < count && postIds.at(after).isEmpty()) {
            return after;
        }
    }
    return -1;
}

void ChannelPostSource::seedCachedPosts()
{
    if (!exactRootCount) {
        return;
    }

    const QVector<BackendPost*> cached = cachedRootPosts(channel);
    if (cached.isEmpty()) {
        return;
    }

    // Cached channel startup data is useful only when it reaches the real newest
    // edge. Arbitrary permalink/context cache entries must not be guessed into
    // absolute logical positions; semantic navigation adopts those on demand.
    const BackendPost* newest = cached.last();
    if (postIds.size() > cached.size()
        && channel.last_post_at != 0
        && newest->create_at < channel.last_post_at) {
        return;
    }

    if (postIds.size() < cached.size()) {
        postIds.resize(cached.size());
    }
    const int first = std::max(0, static_cast<int>(postIds.size() - cached.size()));
    for (int offset = 0; offset < cached.size(); ++offset) {
        postIds[first + offset] = cached.at(offset)->id;
    }
    rebuildIndex();
    emit rangeAvailable(first, static_cast<int>(postIds.size()) - 1);
}

void ChannelPostSource::seedUnknownNewestPost()
{
    if (exactRootCount || channel.last_post_at == 0) {
        return;
    }

    const QVector<BackendPost*> cached = cachedRootPosts(channel);
    if (cached.isEmpty()) {
        return;
    }

    BackendPost* newest = cached.last();
    if (!newest || newest->create_at < channel.last_post_at) {
        return;
    }

    postIds.push_back(newest->id);
    rebuildIndex();
}

void ChannelPostSource::rebuildIndex()
{
    postIndexes.clear();
    for (int index = 0; index < postIds.size(); ++index) {
        if (!postIds.at(index).isEmpty()) {
            postIndexes.insert(postIds.at(index), index);
        }
    }
}

void ChannelPostSource::placePage(int page, const QStringList& chronologicalIds)
{
    if (!exactRootCount || postIds.isEmpty() || chronologicalIds.isEmpty()) {
        return;
    }

    const int first = std::max(0,
        static_cast<int>(postIds.size()) - page * ServerPageSize
            - static_cast<int>(chronologicalIds.size()));
    const int count = std::min(static_cast<int>(chronologicalIds.size()),
                               static_cast<int>(postIds.size()) - first);
    const int last = first + count - 1;

    // A semantic target may have occupied an estimated slot. Once an
    // authoritative page arrives, identity wins and the temporary occurrence is
    // removed before the page is written into its real logical range.
    for (int offset = 0; offset < count; ++offset) {
        const int existing = postIndexes.value(chronologicalIds.at(offset), -1);
        if (existing >= 0 && (existing < first || existing > last)) {
            postIds[existing].clear();
            emit itemsChanged(existing, existing);
        }
    }
    for (int offset = 0; offset < count; ++offset) {
        postIds[first + offset] = chronologicalIds.at(offset);
    }
    rebuildIndex();
    emit itemsChanged(first, last);
    emit rangeAvailable(first, last);
}

void ChannelPostSource::prependDiscovered(const QStringList& chronologicalIds)
{
    if (chronologicalIds.isEmpty()) {
        return;
    }

    QVector<QString> combined;
    combined.reserve(static_cast<int>(chronologicalIds.size()) + static_cast<int>(postIds.size()));
    for (const QString& id : chronologicalIds) {
        combined.push_back(id);
    }
    for (const QString& id : std::as_const(postIds)) {
        combined.push_back(id);
    }

    const int inserted = static_cast<int>(chronologicalIds.size());
    postIds = std::move(combined);
    rebuildIndex();
    emit itemsInserted(0, inserted);
    emit rangeAvailable(0, inserted - 1);
}

void ChannelPostSource::appendLivePost(BackendPost& post)
{
    if (post.hidden || !post.root_id.isEmpty()) {
        return;
    }

    const int existing = indexOfPost(post.id);
    if (existing >= 0) {
        emit itemsChanged(existing, existing);
        return;
    }

    int count = exactRootCount ? currentLogicalCount() : static_cast<int>(postIds.size()) + 1;
    if (count <= postIds.size()) {
        count = static_cast<int>(postIds.size()) + 1;
    }
    postIds.resize(count);
    const int index = count - 1;
    postIds[index] = post.id;
    postIndexes.insert(post.id, index);
    emit itemCountChanged(count);
    emit rangeAvailable(index, index);
}

} // namespace Mattermost
