#include "ChannelPostSource.h"

#include <algorithm>
#include <memory>

#include <QPointer>
#include <QSet>

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
{
    postIds.resize(currentLogicalCount());
    seedCachedPosts();

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
        const int count = currentLogicalCount();
        if (count != postIds.size()) {
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

void ChannelPostSource::requestRange(int first,
                                     int last,
                                     RequestReason reason,
                                     quint64 generation)
{
    Q_UNUSED(reason)
    Q_UNUSED(generation)

    if (postIds.isEmpty()) {
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

int ChannelPostSource::currentLogicalCount() const
{
    if (channel.has_total_msg_count_root) {
        return std::max(0, channel.total_msg_count_root);
    }

    const QVector<BackendPost*> cached = cachedRootPosts(channel);
    if (!cached.isEmpty()) {
        return static_cast<int>(cached.size());
    }
    return std::max(0, channel.total_msg_count);
}

int ChannelPostSource::pageForIndex(int index) const
{
    if (postIds.isEmpty()) {
        return 0;
    }
    index = std::max(0, std::min(index, static_cast<int>(postIds.size()) - 1));
    return (static_cast<int>(postIds.size()) - 1 - index) / ServerPageSize;
}

void ChannelPostSource::seedCachedPosts()
{
    const QVector<BackendPost*> cached = cachedRootPosts(channel);
    if (cached.isEmpty()) {
        return;
    }

    // Cached channel startup data is useful only when it reaches the real newest
    // edge. Arbitrary permalink/context cache entries must not be guessed into
    // absolute logical positions; normal range requests will place them from an
    // authoritative server page instead.
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
    if (postIds.isEmpty() || chronologicalIds.isEmpty()) {
        return;
    }

    const int first = std::max(0,
        static_cast<int>(postIds.size()) - page * ServerPageSize - chronologicalIds.size());
    const int count = std::min(static_cast<int>(chronologicalIds.size()),
                               static_cast<int>(postIds.size()) - first);
    for (int offset = 0; offset < count; ++offset) {
        postIds[first + offset] = chronologicalIds.at(offset);
    }
    rebuildIndex();
    emit rangeAvailable(first, first + count - 1);
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

    int count = currentLogicalCount();
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
