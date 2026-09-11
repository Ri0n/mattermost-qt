#include "ThreadPostSource.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QLoggingCategory>
#include <QPointer>
#include <QTimer>

#include "ThreadTimelineSizing.h"
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
    : IndexedPostSource(channelInstance, parent)
    , backend(backendInstance)
    , rootId(std::move(sourceRootId))
{
    postIds.resize(currentLogicalCount());
    if (!postIds.isEmpty() && rootPost()) {
        postIds[0] = rootId;
    }
    seedCachedPosts();

    BackendPost* root = rootPost();
    if (root) {
        rootResidencyLease = PostTimelineService::instance(backend).leasePost(*root);
    }
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_INIT source=" << static_cast<const void*>(this)
        << " root=" << shortId(rootId)
        << " replyCount=" << (root ? root->reply_count : -1)
        << " lastReplyAt=" << (root ? root->last_reply_at : 0)
        << ' ' << slotSummary(postIds);

    const auto syncLogicalCount = [this](const BackendPost& rootPost) {
        if (rootPost.id != rootId) {
            return;
        }
        const int count = currentLogicalCount();
        if (count == static_cast<int>(postIds.size())) {
            return;
        }

        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_COUNT_CHANGE source=" << static_cast<const void*>(this)
            << " old=" << postIds.size()
            << " new=" << count
            << " replyCount=" << rootPost.reply_count;
        resizeLogicalTail(count);
        pruneProvisionalPostIds();
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_SLOTS source=" << static_cast<const void*>(this)
            << ' ' << slotSummary(postIds);
    };

    connect(&channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& post) { appendLiveReply(post); });
    connect(&channel, &BackendChannel::onPostEdited, this,
            [this, syncLogicalCount](BackendPost& post) {
        const int index = indexOfPost(post.id);
        if (index >= 0) {
            qCDebug(lcThreadTimelineTrace).nospace()
                << "THREAD_POST_EDIT source=" << static_cast<const void*>(this)
                << " post=" << shortId(post.id)
                << " index=" << index;
            emit itemsChanged(index, index);
        }
        syncLogicalCount(post);
    });
    connect(&channel, &BackendChannel::onThreadSummaryChanged, this,
            [syncLogicalCount](BackendPost& rootPost) {
        // Reply-count/thread-summary changes alter the logical thread topology,
        // but not the root post body. Keep the source count current without
        // publishing itemsChanged(0), which would rematerialize the thread root.
        syncLogicalCount(rootPost);
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

    QTimer::singleShot(0, this, [this] { hydrateCachedTail(); });
}

bool ThreadPostSource::isAvailable(int index) const
{
    if (!IndexedPostSource::isAvailable(index)) {
        return false;
    }

    // A body fetched for permalink navigation does not make its timestamp-based
    // estimated slot a renderable row. Keep only the semantic identity/index for
    // scrolling and range selection; materialize after a server window confirms
    // or relocates the same post.
    return !navigationPlacement.blocksMaterialization(postIds.at(index));
}

int ThreadPostSource::ensurePostIndex(const QString& postId)
{
    const int existing = indexOfPost(postId);
    if (existing >= 0) {
        if (provisionalPostIds.contains(postId)) {
            // Cached thread windows may already expose a provisional row. Keep
            // that row renderable, but protect its semantic identity until a
            // server window confirms or relocates it.
            navigationPlacement.trackExistingProvisional(postId, existing);
        }
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
        navigationPlacement.clear();
        emit rangeAvailable(0, 0);
        return 0;
    }

    const int index = nearestEmptyIndex(estimatedIndexForPost(*post));
    if (index < 1) {
        return -1;
    }
    postIds[index] = postId;
    provisionalPostIds.insert(postId);
    navigationPlacement.trackEstimated(postId, index);
    rebuildIndex();
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PROVISIONAL source=" << static_cast<const void*>(this)
        << " post=" << shortId(postId)
        << " index=" << index
        << " materializable=false"
        << ' ' << slotSummary(postIds);

    // Deliberately do not emit rangeAvailable(). LongListWidget can navigate to
    // this logical estimate while it remains unavailable and will request the
    // surrounding server window without constructing a temporary PostWidget.
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

    // The oldest edge is authoritative too. When LongList explicitly asks for a
    // contiguous start window, satisfy that demand in one server request instead
    // of exposing another transport-sized subdivision to the view.
    if (requestedFirst <= 1) {
        const int fetchCount = std::max(ServerBlockSize, requestedLast + 1);
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_REQUEST_BRANCH source=" << static_cast<const void*>(this)
            << " branch=initial perPage=" << fetchCount;
        PostTimelineService::instance(backend).loadThreadPage(
            channel, rootId, fetchCount, QString(), 0,
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

    // Select one actual contiguous unavailable run. A requested range can
    // contain authoritative islands left by earlier random seeks; spanning those
    // islands as one synthetic gap hides their usable cursors and may send the
    // source back through timestamp approximation unnecessarily.
    int firstMissing = -1;
    int lastMissing = -1;
    for (int index = requestedFirst; index <= requestedLast; ++index) {
        if (isCursorReadyIndex(index)) {
            if (firstMissing >= 0) {
                break;
            }
            continue;
        }
        if (firstMissing < 0) {
            firstMissing = index;
        }
        lastMissing = index;
    }
    if (firstMissing < 0) {
        emit rangeRequestFinished(first, last);
        return;
    }

    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_MISSING_RUN source=" << static_cast<const void*>(this)
        << " requested=[" << requestedFirst << ',' << requestedLast << ']'
        << " missing=[" << firstMissing << ',' << lastMissing << ']';

    const int missingCount = lastMissing - firstMissing + 1;

    if (boundaryRequestGate.isActive() && reason != RequestReason::Seek
        && boundaryRequestGate.canAttach(firstMissing, lastMissing)) {
        const PostSourceRequestGate::Range expected = boundaryRequestGate.expectedRange();
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_REQUEST_ATTACH source=" << static_cast<const void*>(this)
            << " requested=[" << first << ',' << last << ']'
            << " expected=[" << expected.first << ',' << expected.last << ']';
        boundaryRequestGate.attach(first, last);
        return;
    }

    // Once either side of a gap is known, that identity is a stronger anchor
    // than a timestamp estimate. Fill sequentially from the adjacent cursor.
    if (firstMissing > 0 && isCursorReadyIndex(firstMissing - 1)) {
        const int anchorIndex = firstMissing - 1;
        const QString anchorId = postIds.at(anchorIndex);
        BackendPost* anchorPost = channel.postIdToPost.value(anchorId, nullptr);
        const uint64_t anchorCreateAt = anchorPost ? anchorPost->create_at : 0;
        const int fetchCount = std::max(ServerBlockSize, missingCount);
        const bool gated = reason != RequestReason::Seek && !boundaryRequestGate.isActive();
        if (gated) {
            boundaryRequestGate.begin(
                anchorIndex + 1,
                std::min(static_cast<int>(postIds.size()) - 1, anchorIndex + fetchCount),
                first, last);
        }
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_REQUEST_BRANCH source=" << static_cast<const void*>(this)
            << " branch=cursor-forward anchorIndex=" << anchorIndex
            << " anchor=" << shortId(anchorId)
            << " firstMissing=" << firstMissing
            << " perPage=" << fetchCount
            << " gated=" << gated;
        PostTimelineService::instance(backend).loadThreadAfter(
            channel, rootId, anchorId, anchorCreateAt, fetchCount,
            [guard, anchorIndex, anchorId, first, last, gated](
                const PostTimelineService::Page& result) {
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
                if (gated) {
                    guard->finishBoundaryRequest();
                } else {
                    emit guard->rangeRequestFinished(first, last);
                }
            });
        return;
    }

    if (lastMissing >= 1
        && lastMissing + 1 < static_cast<int>(postIds.size())
        && isCursorReadyIndex(lastMissing + 1)) {
        const int anchorIndex = lastMissing + 1;
        const QString anchorId = postIds.at(anchorIndex);
        BackendPost* anchorPost = channel.postIdToPost.value(anchorId, nullptr);
        const uint64_t anchorCreateAt = anchorPost ? anchorPost->create_at : 0;
        const int fetchCount = std::max(ServerBlockSize, missingCount);
        const bool gated = reason != RequestReason::Seek && !boundaryRequestGate.isActive();
        if (gated) {
            boundaryRequestGate.begin(
                std::max(1, anchorIndex - fetchCount),
                anchorIndex - 1,
                first, last);
        }
        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_REQUEST_BRANCH source=" << static_cast<const void*>(this)
            << " branch=cursor-backward anchorIndex=" << anchorIndex
            << " anchor=" << shortId(anchorId)
            << " lastMissing=" << lastMissing
            << " perPage=" << fetchCount
            << " gated=" << gated;
        PostTimelineService::instance(backend).loadThreadBefore(
            channel, rootId, anchorId, anchorCreateAt, fetchCount,
            [guard, anchorIndex, anchorId, first, last, gated](
                const PostTimelineService::Page& result) {
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
                if (gated) {
                    guard->finishBoundaryRequest();
                } else {
                    emit guard->rangeRequestFinished(first, last);
                }
            });
        return;
    }

    // The newest edge is exact. If the demand reaches it, bootstrap the whole
    // requested suffix with one tail request. This is deliberately sized from
    // viewport demand rather than ServerBlockSize so FullHD/UHD viewports do not
    // fan out into several concurrent ten-message transport decisions.
    if (requestedLast == static_cast<int>(postIds.size()) - 1) {
        const int fetchCount = std::max(
            ServerBlockSize, static_cast<int>(postIds.size()) - firstMissing);
        const int expectedFirst = std::max(
            1, static_cast<int>(postIds.size()) - fetchCount);
        const bool gated = reason != RequestReason::Seek && !boundaryRequestGate.isActive();
        if (gated) {
            boundaryRequestGate.begin(expectedFirst,
                                      static_cast<int>(postIds.size()) - 1,
                                      first, last);
        }

        qCDebug(lcThreadTimelineTrace).nospace()
            << "THREAD_REQUEST_BRANCH source=" << static_cast<const void*>(this)
            << " branch=tail lastReplyAt=" << root->last_reply_at
            << " perPage=" << fetchCount
            << " expectedFirst=" << expectedFirst
            << " gated=" << gated;
        PostTimelineService::instance(backend).loadThreadTail(
            channel, rootId, fetchCount, root->last_reply_at,
            [guard, first, last, gated](const PostTimelineService::Page& result) {
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
                if (gated) {
                    guard->finishBoundaryRequest();
                } else {
                    emit guard->rangeRequestFinished(first, last);
                }
            });
        return;
    }

    // Only a genuinely disconnected random middle window needs timestamp seek.
    const int target = (requestedFirst + requestedLast) / 2;
    const uint64_t estimatedTime = estimatedCreateAt(target);
    const int fetchCount = std::max(ServerBlockSize, missingCount);
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_REQUEST_BRANCH source=" << static_cast<const void*>(this)
        << " branch=approx target=" << target
        << " fromCreateAt=" << estimatedTime
        << " perPage=" << fetchCount;
    PostTimelineService::instance(backend).loadThreadFromTime(
        channel, rootId, fetchCount, estimatedTime,
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

void ThreadPostSource::finishBoundaryRequest()
{
    const QVector<PostSourceRequestGate::Range> waiters = boundaryRequestGate.finish();
    for (const PostSourceRequestGate::Range& waiter : waiters) {
        emit rangeRequestFinished(waiter.first, waiter.last);
    }
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

    int deletedReplyTombstones = 0;
    for (const BackendPost& post : channel.posts) {
        if (post.root_id == rootId && (post.isDeleted || post.delete_at != 0)) {
            ++deletedReplyTombstones;
        }
    }

    int count = threadLogicalItemCount(root->reply_count, deletedReplyTombstones);

    // reply_count is metadata, not authority to destroy an identity that this
    // source has already mapped. This matters in particular for deleted replies:
    // Mattermost decrements reply_count while the client keeps the deleted reply
    // as a tombstone. Preserve the furthest mapped row even if its body is later
    // evicted from the residency cache.
    for (int index = static_cast<int>(postIds.size()) - 1; index >= count; --index) {
        if (!postIds.at(index).isEmpty()) {
            count = index + 1;
            break;
        }
    }
    return count;
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

void ThreadPostSource::hydrateCachedTail()
{
    BackendPost* root = rootPost();
    if (!root || postIds.size() <= 1 || root->reply_count <= 0) {
        return;
    }

    PostTimelineService& repository = PostTimelineService::instance(backend);
    repository.recordChannelOpened(channel.id);
    QPointer<ThreadPostSource> guard(this);
    repository.loadCachedThreadTail(
        channel, rootId, ServerBlockSize,
        [guard](const PostTimelineService::Page& result) {
            if (!guard || !result.success || result.postIds.isEmpty()) {
                return;
            }
            BackendPost* root = guard->rootPost();
            BackendPost* newest = guard->channel.postIdToPost.value(
                result.postIds.last(), nullptr);
            if (!root || !newest || (root->last_reply_at != 0
                                     && newest->create_at != root->last_reply_at)) {
                qCDebug(lcThreadTimelineTrace).nospace()
                    << "THREAD_CACHE_TAIL_SKIP source="
                    << static_cast<const void*>(guard.data())
                    << " reason=newest-mismatch cached="
                    << (newest ? newest->create_at : 0)
                    << " root=" << (root ? root->last_reply_at : 0);
                return;
            }

            const int usableCount = std::min(
                static_cast<int>(result.postIds.size()),
                static_cast<int>(guard->postIds.size()) - 1);
            if (usableCount <= 0) {
                return;
            }
            const QStringList ids = result.postIds.mid(
                result.postIds.size() - usableCount);
            const int first = static_cast<int>(guard->postIds.size()) - usableCount;

            // Cached-tail hydration deliberately remains renderable while it is
            // validated, but it must not turn a freshly estimated permalink slot
            // into a concrete widget merely because the cache overlaps it. The
            // explicit navigation request will fetch the confirming server page.
            if (guard->navigationPlacement.isActive()
                && guard->navigationPlacement.blocksMaterialization(
                    guard->navigationPlacement.postId())) {
                const int estimate = guard->indexOfPost(
                    guard->navigationPlacement.postId());
                if (estimate >= first && estimate < first + usableCount) {
                    qCDebug(lcThreadTimelineTrace).nospace()
                        << "THREAD_CACHE_TAIL_SKIP source="
                        << static_cast<const void*>(guard.data())
                        << " reason=navigation-estimate target="
                        << shortId(guard->navigationPlacement.postId())
                        << " index=" << estimate;
                    return;
                }
            }

            for (int offset = 0; offset < usableCount; ++offset) {
                const int target = first + offset;
                const QString& id = ids.at(offset);
                const int existingIndex = guard->indexOfPost(id);
                if ((existingIndex >= 0 && existingIndex != target)
                    || (!guard->postIds.at(target).isEmpty()
                        && guard->postIds.at(target) != id)) {
                    qCDebug(lcThreadTimelineTrace).nospace()
                        << "THREAD_CACHE_TAIL_SKIP source="
                        << static_cast<const void*>(guard.data())
                        << " reason=identity-collision target=" << target
                        << " id=" << shortId(id);
                    return;
                }
            }

            for (const QString& id : ids) {
                if (guard->indexOfPost(id) < 0) {
                    guard->provisionalPostIds.insert(id);
                }
            }
            const ExactWindowMutation mutation = guard->assignExactWindow(first, ids);
            guard->publishExactWindow(mutation);
            qCDebug(lcThreadTimelineTrace).nospace()
                << "THREAD_CACHE_TAIL_HYDRATE source="
                << static_cast<const void*>(guard.data())
                << " target=[" << first << ',' << (first + usableCount - 1) << ']'
                << " ids=" << idsSummary(ids);
            guard->validateCachedTail();
        });
}

void ThreadPostSource::validateCachedTail()
{
    BackendPost* root = rootPost();
    if (!root) {
        return;
    }

    QPointer<ThreadPostSource> guard(this);
    if (static_cast<int>(postIds.size()) - 1 <= ServerBlockSize) {
        PostTimelineService::instance(backend).loadThreadPage(
            channel, rootId, ServerBlockSize, QString(), 0,
            [guard](const PostTimelineService::Page& result) {
                if (!guard || !result.success || result.postIds.isEmpty()) {
                    return;
                }
                guard->placeInitial(result.postIds);
            });
        return;
    }

    PostTimelineService::instance(backend).loadThreadTail(
        channel, rootId, ServerBlockSize, root->last_reply_at,
        [guard](const PostTimelineService::Page& result) {
            if (!guard || !result.success || result.postIds.isEmpty()) {
                return;
            }
            guard->placeTail(result.postIds);
        });
}

bool ThreadPostSource::isAuthoritativeIndex(int index) const
{
    if (index < 0 || index >= static_cast<int>(postIds.size())) {
        return false;
    }
    const QString& id = postIds.at(index);
    return !id.isEmpty() && !provisionalPostIds.contains(id);
}

bool ThreadPostSource::isCursorReadyIndex(int index) const
{
    // Identity authority survives body eviction, but a Mattermost compound
    // cursor also needs the resident create_at value. Do not confuse those two
    // states or a known-but-evicted ID becomes an unusable cursor anchor.
    return isAuthoritativeIndex(index) && isAvailable(index);
}

void ThreadPostSource::pruneProvisionalPostIds()
{
    for (auto it = provisionalPostIds.begin(); it != provisionalPostIds.end();) {
        if (indexOfPost(*it) < 0) {
            it = provisionalPostIds.erase(it);
        } else {
            ++it;
        }
    }

    if (navigationPlacement.isActive()) {
        const int index = indexOfPost(navigationPlacement.postId());
        if (index < 0) {
            navigationPlacement.clear();
        } else {
            navigationPlacement.updateIndex(index);
        }
    }
}

void ThreadPostSource::placeExactWindow(int first, const QStringList& ids)
{
    if (ids.isEmpty()) {
        return;
    }

    // A thread jump can seed one cached reply at a timestamp-estimated slot.
    // Preserve that semantic identity until an exact window actually contains
    // it. Numeric overlap alone is not authority to replace the jump target.
    if (navigationPlacement.isActive()) {
        const QString targetPostId = navigationPlacement.postId();
        const int currentIndex = indexOfPost(targetPostId);
        if (currentIndex >= 0) {
            navigationPlacement.updateIndex(currentIndex);
        }
        const int last = first + static_cast<int>(ids.size()) - 1;
        if (currentIndex >= first && currentIndex <= last
            && !ids.contains(targetPostId)) {
            qCDebug(lcThreadTimelineTrace).nospace()
                << "THREAD_NAV_WINDOW_DEFER source=" << static_cast<const void*>(this)
                << " target=" << shortId(targetPostId)
                << " targetIndex=" << currentIndex
                << " incoming=[" << first << ',' << last << ']';
            return;
        }
    }

    const ThreadNavigationPlacement::Confirmation confirmation =
        navigationPlacement.confirmExactWindow(first, ids);
    for (const QString& id : ids) {
        provisionalPostIds.remove(id);
    }

    const ExactWindowMutation mutation = assignExactWindow(first, ids);
    publishExactWindow(mutation);

    // If the server confirmed the estimated identity at exactly the same slot,
    // assignExactWindow() has no mapping delta and therefore emits nothing. The
    // row nevertheless changed from navigation metadata to a materializable
    // source item, so publish that availability transition explicitly.
    if (confirmation.isValid() && confirmation.wasEstimated
        && !mutation.mappingChanged
        && confirmation.authoritativeIndex >= 0
        && confirmation.authoritativeIndex < static_cast<int>(postIds.size())
        && isAvailable(confirmation.authoritativeIndex)) {
        emit rangeAvailable(confirmation.authoritativeIndex,
                            confirmation.authoritativeIndex);
    }

    pruneProvisionalPostIds();
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

    placeExactWindow(first, ids.mid(0, count));

    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_PLACE_APPROX_DONE source=" << static_cast<const void*>(this)
        << ' ' << slotSummary(postIds);
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

    const int oldCount = static_cast<int>(postIds.size());
    int count = currentLogicalCount();

    // BackendChannel::addPost() advances the root thread summary before the
    // posted event is forwarded as onNewPost(reply). onThreadSummaryChanged()
    // therefore normally grows postIds first, leaving one empty newest slot for
    // this exact live reply. Do not count the same reply twice.
    const bool metadataReservedTail = count == oldCount && count > 1
        && postIds.at(count - 1).isEmpty();
    if (count > oldCount) {
        resizeLogicalTail(count);
    } else if (!metadataReservedTail) {
        // Defensive fallback for a producer that delivers the live reply before
        // root metadata has advanced. In that ordering the event itself is the
        // only evidence that the logical thread grew.
        count = oldCount + 1;
        resizeLogicalTail(count);
    }
    const int index = count - 1;
    publishExactWindow(assignExactWindow(index, QStringList { post.id }));
    qCDebug(lcThreadTimelineTrace).nospace()
        << "THREAD_LIVE_APPEND source=" << static_cast<const void*>(this)
        << " post=" << shortId(post.id)
        << " index=" << index
        << ' ' << slotSummary(postIds);
}

} // namespace Mattermost
