#include "ThreadPostSource.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QLoggingCategory>
#include <QPointer>

#include "backend/Backend.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

namespace {

Q_LOGGING_CATEGORY(lcThreadTimelineTrace, "mattermost.timeline.thread", QtWarningMsg)

QString shortId(const QString& id)
{
    return id.isEmpty() ? QStringLiteral("-") : id.left(8);
}

QString slotSummary(const QVector<QString>& postIds)
{
    int available = 0;
    for (const QString& id : postIds) {
        available += !id.isEmpty();
    }

    QString result = QStringLiteral("count=%1 known=%2 missing=%3")
        .arg(postIds.size())
        .arg(available)
        .arg(postIds.size() - available);
    if (postIds.size() > 50) {
        return result;
    }

    result += QStringLiteral(" slots=");
    for (int index = 0; index < postIds.size(); ++index) {
        if (index != 0) {
            result += QLatin1Char(' ');
        }
        result += QString::number(index);
        result += QLatin1Char(':');
        result += shortId(postIds.at(index));
    }
    return result;
}

QString idsSummary(const QStringList& ids)
{
    QStringList result;
    result.reserve(ids.size());
    for (const QString& id : ids) {
        result.push_back(shortId(id));
    }
    return result.join(QLatin1Char(','));
}

const char* requestReasonName(AbstractPostSource::RequestReason reason)
{
    switch (reason) {
    case AbstractPostSource::RequestReason::Initial:
        return "initial";
    case AbstractPostSource::RequestReason::Scroll:
        return "scroll";
    case AbstractPostSource::RequestReason::Seek:
        return "seek";
    case AbstractPostSource::RequestReason::EnsureVisible:
        return "ensure-visible";
    }
    return "unknown";
}

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

    BackendPost* root = rootPost();
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_INIT source=" << static_cast<const void*>(this)
        << " root=" << shortId(rootId)
        << " replyCount=" << (root ? root->reply_count : -1)
        << " lastReplyAt=" << (root ? root->last_reply_at : 0)
        << ' ' << slotSummary(postIds);

    connect(&channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& post) { appendLiveReply(post); });
    connect(&channel, &BackendChannel::onPostEdited, this,
            [this](BackendPost& post) {
        const int index = indexOfPost(post.id);
        if (index >= 0) {
            qCDebug(lcThreadTimelineTrace).nospace()
                << "THREAD_POST_EDIT source=" << static_cast<const void*>(this)
                << " post=" << shortId(post.id)
                << " index=" << index;
            emit itemsChanged(index, index);
        }
        if (post.id == rootId) {
            const int count = currentLogicalCount();
            if (count != static_cast<int>(postIds.size())) {
                qCDebug(lcThreadTimelineTrace).nospace()
                    << "THREAD_COUNT_CHANGE source=" << static_cast<const void*>(this)
                    << " old=" << postIds.size()
                    << " new=" << count
                    << " replyCount=" << post.reply_count;
                postIds.resize(count);
                if (!postIds.isEmpty()) {
                    postIds[0] = rootId;
                }
                rebuildIndex();
                qCDebug(lcThreadTimelineTrace).nospace()
                    << "THREAD_SLOTS source=" << static_cast<const void*>(this)
                    << ' ' << slotSummary(postIds);
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
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PROVISIONAL source=" << static_cast<const void*>(this)
        << " post=" << shortId(postId)
        << " index=" << index
        << ' ' << slotSummary(postIds);
    emit rangeAvailable(index, index);
    return index;
}

void ThreadPostSource::requestRange(int first,
                                    int last,
                                    RequestReason reason,
                                    quint64 generation)
{
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_REQUEST source=" << static_cast<const void*>(this)
        << " root=" << shortId(rootId)
        << " requested=[" << first << ',' << last << ']'
        << " reason=" << requestReasonName(reason)
        << " generation=" << generation
        << ' ' << slotSummary(postIds);

    if (postIds.isEmpty() || !rootPost()) {
        qCDebug(lcThreadTimelineTrace) << "THREAD_REQUEST_EMPTY";
        emit rangeRequestFinished(first, last);
        return;
    }

    const int requestedFirst = std::max(0, first);
    const int requestedLast = std::min(static_cast<int>(postIds.size()) - 1, last);
    if (requestedLast < requestedFirst) {
        emit rangeRequestFinished(first, last);
        return;
    }

    if (requestedFirst == 0) {
        postIds[0] = rootId;
        rebuildIndex();
        emit rangeAvailable(0, 0);
    }

    QPointer<ThreadPostSource> guard(this);
    BackendPost* root = rootPost();

    // The oldest edge is authoritative. Fetch it directly even if the same
    // logical block also overlaps the tail of a short thread.
    if (requestedFirst <= 1) {
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_REQUEST_BRANCH source=" << static_cast<const void*>(this)
            << " branch=initial";
        PostTimelineService::instance(backend).loadThreadPage(
            channel, rootId, ServerBlockSize, QString(), 0,
            [guard, first, last](const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }
                qCDebug(lcThreadTimelineTrace).nospace()
                    << "THREAD_RESPONSE source=" << static_cast<const void*>(guard.data())
                    << " branch=initial success=" << result.success
                    << " ids=" << idsSummary(result.postIds)
                    << " prev=" << shortId(result.prevPostId)
                    << " next=" << shortId(result.nextPostId)
                    << " hasNext=" << result.hasNext;
                if (result.success && !result.postIds.isEmpty()) {
                    guard->placeInitial(result.postIds);
                }
                emit guard->rangeRequestFinished(first, last);
            });
        return;
    }

    int firstMissing = -1;
    int lastMissing = -1;
    for (int index = requestedFirst; index <= requestedLast; ++index) {
        if (!postIds.at(index).isEmpty()) {
            continue;
        }
        if (firstMissing < 0) {
            firstMissing = index;
        }
        lastMissing = index;
    }

    // Once either side of a gap is known, that identity is a stronger anchor
    // than a timestamp estimate. Fill sequentially from the adjacent cursor.
    if (firstMissing > 0 && !postIds.at(firstMissing - 1).isEmpty()) {
        const int anchorIndex = firstMissing - 1;
        const QString anchorId = postIds.at(anchorIndex);
        BackendPost* anchorPost = channel.postIdToPost.value(anchorId, nullptr);
        const uint64_t anchorCreateAt = anchorPost ? anchorPost->create_at : 0;
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_REQUEST_BRANCH source=" << static_cast<const void*>(this)
            << " branch=cursor-forward anchorIndex=" << anchorIndex
            << " anchor=" << shortId(anchorId)
            << " firstMissing=" << firstMissing;
        PostTimelineService::instance(backend).loadThreadAfter(
            channel, rootId, anchorId, anchorCreateAt, ServerBlockSize,
            [guard, anchorIndex, anchorId, first, last](const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }
                qCDebug(lcThreadTimelineTrace).nospace()
                    << "THREAD_RESPONSE source=" << static_cast<const void*>(guard.data())
                    << " branch=cursor-forward anchorIndex=" << anchorIndex
                    << " anchor=" << shortId(anchorId)
                    << " success=" << result.success
                    << " ids=" << idsSummary(result.postIds)
                    << " prev=" << shortId(result.prevPostId)
                    << " next=" << shortId(result.nextPostId)
                    << " hasNext=" << result.hasNext;
                if (result.success && !result.postIds.isEmpty()) {
                    guard->placeExactWindow(anchorIndex + 1, result.postIds);
                }
                emit guard->rangeRequestFinished(first, last);
            });
        return;
    }

    if (lastMissing >= 1
        && lastMissing + 1 < postIds.size()
        && !postIds.at(lastMissing + 1).isEmpty()) {
        const int anchorIndex = lastMissing + 1;
        const QString anchorId = postIds.at(anchorIndex);
        BackendPost* anchorPost = channel.postIdToPost.value(anchorId, nullptr);
        const uint64_t anchorCreateAt = anchorPost ? anchorPost->create_at : 0;
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_REQUEST_BRANCH source=" << static_cast<const void*>(this)
            << " branch=cursor-backward anchorIndex=" << anchorIndex
            << " anchor=" << shortId(anchorId)
            << " lastMissing=" << lastMissing;
        PostTimelineService::instance(backend).loadThreadBefore(
            channel, rootId, anchorId, anchorCreateAt, ServerBlockSize,
            [guard, anchorIndex, anchorId, first, last](const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }
                qCDebug(lcThreadTimelineTrace).nospace()
                    << "THREAD_RESPONSE source=" << static_cast<const void*>(guard.data())
                    << " branch=cursor-backward anchorIndex=" << anchorIndex
                    << " anchor=" << shortId(anchorId)
                    << " success=" << result.success
                    << " ids=" << idsSummary(result.postIds)
                    << " prev=" << shortId(result.prevPostId)
                    << " next=" << shortId(result.nextPostId)
                    << " hasNext=" << result.hasNext;
                if (result.success && !result.postIds.isEmpty()) {
                    const int pageCount = std::min(static_cast<int>(result.postIds.size()),
                                                   std::max(0, anchorIndex - 1));
                    if (pageCount > 0) {
                        const QStringList page = result.postIds.mid(result.postIds.size() - pageCount);
                        guard->placeExactWindow(anchorIndex - pageCount, page);
                    }
                }
                emit guard->rangeRequestFinished(first, last);
            });
        return;
    }

    // With no adjacent known identity, the newest boundary is still exact.
    if (requestedLast >= static_cast<int>(postIds.size()) - ServerBlockSize) {
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_REQUEST_BRANCH source=" << static_cast<const void*>(this)
            << " branch=tail lastReplyAt=" << root->last_reply_at;
        PostTimelineService::instance(backend).loadThreadTail(
            channel, rootId, ServerBlockSize, root->last_reply_at,
            [guard, first, last](const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }
                qCDebug(lcThreadTimelineTrace).nospace()
                    << "THREAD_RESPONSE source=" << static_cast<const void*>(guard.data())
                    << " branch=tail success=" << result.success
                    << " ids=" << idsSummary(result.postIds)
                    << " prev=" << shortId(result.prevPostId)
                    << " next=" << shortId(result.nextPostId)
                    << " hasNext=" << result.hasNext;
                if (result.success && !result.postIds.isEmpty()) {
                    guard->placeTail(result.postIds);
                }
                emit guard->rangeRequestFinished(first, last);
            });
        return;
    }

    // Only a genuinely disconnected random middle window needs timestamp seek.
    const int target = (requestedFirst + requestedLast) / 2;
    const uint64_t estimatedTime = estimatedCreateAt(target);
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_REQUEST_BRANCH source=" << static_cast<const void*>(this)
        << " branch=approx target=" << target
        << " fromCreateAt=" << estimatedTime;
    PostTimelineService::instance(backend).loadThreadFromTime(
        channel, rootId, ServerBlockSize, estimatedTime,
        [guard, target, first, last](const PostTimelineService::Page& result) {
            if (!guard) {
                return;
            }
            qCDebug(lcThreadTimelineTrace).nospace()
                << "THREAD_RESPONSE source=" << static_cast<const void*>(guard.data())
                << " branch=approx target=" << target
                << " success=" << result.success
                << " ids=" << idsSummary(result.postIds)
                << " prev=" << shortId(result.prevPostId)
                << " next=" << shortId(result.nextPostId)
                << " hasNext=" << result.hasNext;
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
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_SEED source=" << static_cast<const void*>(this)
        << " root=" << shortId(rootId)
        << " replyCount=" << root->reply_count
        << " cachedReplies=" << replies.size();
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
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_SEED_ALL source=" << static_cast<const void*>(this)
            << ' ' << slotSummary(postIds);
        emit rangeAvailable(0, static_cast<int>(postIds.size()) - 1);
        return;
    }

    // A partial BackendChannel cache has no contiguity/provenance metadata. It
    // may contain a tail page, an older context page, or both. Packing all such
    // replies into a suffix manufactures authoritative indices that we do not
    // actually know and later forces destructive remaps. Until the cache can
    // describe contiguous windows, only seed a partial thread at its real root.
    rebuildIndex();
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_SEED_ROOT_ONLY source=" << static_cast<const void*>(this)
        << " partialCachedReplies=" << replies.size()
        << ' ' << slotSummary(postIds);
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

void ThreadPostSource::placeExactWindow(int first, const QStringList& ids)
{
    if (postIds.isEmpty() || ids.isEmpty()) {
        return;
    }

    first = std::max(0, std::min(first, static_cast<int>(postIds.size()) - 1));
    const int count = std::min(static_cast<int>(ids.size()),
                               static_cast<int>(postIds.size()) - first);
    if (count <= 0) {
        return;
    }
    const int last = first + count - 1;
    const QVector<QString> before = postIds;

    // Apply identity changes atomically. Exact cursor windows have authoritative
    // logical placement, so old provisional occurrences move to this window;
    // they are not allowed to override the cursor origin.
    for (int offset = 0; offset < count; ++offset) {
        const QString& id = ids.at(offset);
        const int target = first + offset;
        const int existing = postIndexes.value(id, -1);
        if (existing >= 0 && existing != target) {
            postIds[existing].clear();
        }
    }
    for (int offset = 0; offset < count; ++offset) {
        postIds[first + offset] = ids.at(offset);
    }
    rebuildIndex();

    // Newly filled empty slots need only availability. Existing concrete rows
    // are rematerialized only when their identity really changed.
    for (int index = 0; index < postIds.size(); ++index) {
        if (before.at(index) != postIds.at(index) && !before.at(index).isEmpty()) {
            emit itemsChanged(index, index);
        }
    }
    emit rangeAvailable(first, last);
}

void ThreadPostSource::placeInitial(const QStringList& ids)
{
    if (postIds.isEmpty() || ids.isEmpty()) {
        return;
    }

    const int first = ids.first() == rootId ? 0 : 1;
    const int count = std::min(static_cast<int>(ids.size()),
                               static_cast<int>(postIds.size()) - first);
    if (count <= 0) {
        return;
    }
    const QStringList page = ids.mid(0, count);
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_INITIAL source=" << static_cast<const void*>(this)
        << " ids=" << idsSummary(page)
        << " target=[" << first << ',' << (first + count - 1) << ']'
        << " before=" << slotSummary(postIds);

    placeExactWindow(first, page);
    if (first > 0 && !postIds.isEmpty()) {
        postIds[0] = rootId;
        rebuildIndex();
        emit rangeAvailable(0, 0);
    }

    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_INITIAL_DONE source=" << static_cast<const void*>(this)
        << ' ' << slotSummary(postIds);
}

void ThreadPostSource::placeTail(const QStringList& ids)
{
    if (postIds.size() <= 1 || ids.isEmpty()) {
        return;
    }

    const int count = std::min(static_cast<int>(ids.size()),
                               static_cast<int>(postIds.size()) - 1);
    if (count <= 0) {
        return;
    }
    const int first = static_cast<int>(postIds.size()) - count;
    const QStringList page = ids.mid(ids.size() - count);
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_TAIL source=" << static_cast<const void*>(this)
        << " ids=" << idsSummary(page)
        << " target=[" << first << ',' << (first + count - 1) << ']'
        << " before=" << slotSummary(postIds);

    placeExactWindow(first, page);

    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_TAIL_DONE source=" << static_cast<const void*>(this)
        << ' ' << slotSummary(postIds);
}

void ThreadPostSource::placeApproximate(int targetIndex, const QStringList& ids)
{
    if (postIds.size() <= 1 || ids.isEmpty()) {
        return;
    }
    const int count = std::min(static_cast<int>(ids.size()),
                               static_cast<int>(postIds.size()) - 1);
    const int maxFirst = std::max(1, static_cast<int>(postIds.size()) - count);

    // A forward timestamp page starts near the estimated logical target. The
    // estimate is only a fallback; overlap with already mapped identities gives
    // an exact page origin and must keep those rows stationary.
    const int fallbackFirst = std::max(1, std::min(targetIndex, maxFirst));
    int first = fallbackFirst;

    int alignedFirst = -1;
    int overlapCount = 0;
    bool overlapConflict = false;
    for (int offset = 0; offset < count; ++offset) {
        const int existing = postIndexes.value(ids.at(offset), -1);
        if (existing < 1) {
            continue;
        }
        const int candidateFirst = existing - offset;
        if (candidateFirst < 1 || candidateFirst > maxFirst) {
            continue;
        }
        ++overlapCount;
        if (alignedFirst < 0) {
            alignedFirst = candidateFirst;
        } else if (alignedFirst != candidateFirst) {
            overlapConflict = true;
        }
    }
    if (alignedFirst >= 0 && !overlapConflict) {
        first = alignedFirst;
    }

    const int last = first + count - 1;
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_APPROX source=" << static_cast<const void*>(this)
        << " target=" << targetIndex
        << " ids=" << idsSummary(ids)
        << " fallbackFirst=" << fallbackFirst
        << " alignedFirst=" << alignedFirst
        << " overlaps=" << overlapCount
        << " conflict=" << overlapConflict
        << " targetRange=[" << first << ',' << last << ']'
        << " before=" << slotSummary(postIds);

    const QVector<QString> before = postIds;
    for (int offset = 0; offset < count; ++offset) {
        const QString& id = ids.at(offset);
        const int target = first + offset;
        const int existing = postIndexes.value(id, -1);
        if (existing >= 0 && existing != target) {
            postIds[existing].clear();
        }
    }
    for (int offset = 0; offset < count; ++offset) {
        postIds[first + offset] = ids.at(offset);
    }
    rebuildIndex();

    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_APPROX_DONE source=" << static_cast<const void*>(this)
        << ' ' << slotSummary(postIds);

    for (int index = 0; index < postIds.size(); ++index) {
        if (before.at(index) != postIds.at(index) && !before.at(index).isEmpty()) {
            emit itemsChanged(index, index);
        }
    }
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
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_LIVE_EXISTING source=" << static_cast<const void*>(this)
            << " post=" << shortId(post.id)
            << " index=" << existing;
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
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_LIVE_APPEND source=" << static_cast<const void*>(this)
        << " post=" << shortId(post.id)
        << " index=" << index
        << ' ' << slotSummary(postIds);
    emit itemCountChanged(count);
    emit rangeAvailable(index, index);
}

} // namespace Mattermost
