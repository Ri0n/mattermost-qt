#include "ChannelPostSource.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

#include <QLoggingCategory>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/PostTimelineService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

namespace {

Q_LOGGING_CATEGORY(lcTimelineChannel, "mattermost.timeline.channel", QtWarningMsg)

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

QStringList uniqueChronological(const QStringList& ids)
{
    QStringList result;
    result.reserve(ids.size());
    QSet<QString> seen;
    for (const QString& id : ids) {
        if (id.isEmpty() || seen.contains(id)) {
            continue;
        }
        seen.insert(id);
        result.push_back(id);
    }
    return result;
}

} // namespace

ChannelPostSource::ChannelPostSource(Backend& backendInstance,
                                     BackendChannel& channelInstance,
                                     QObject* parent)
    : IndexedPostSource(channelInstance, parent)
    , backend(backendInstance)
    , hasRootCountEstimate(channelInstance.has_total_msg_count_root)
{
    if (hasRootCountEstimate) {
        // A zero message counter is still only an estimate: a channel may
        // contain count-excluded system roots. Keep one unavailable bootstrap
        // slot so LongList requests page zero and lets /posts prove empty vs.
        // non-empty history. An actually empty channel immediately reconciles
        // back to zero.
        postIds.resize(std::max(1, currentLogicalCount()));
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
        if (index < 0) {
            return;
        }

        BackendPost* deletedPost = channel.postIdToPost.value(postId, nullptr);
        const bool removeLikeOfficialClient = deletedPost
            && deletedPost->isOwnPost()
            && deletedPost->root_id.isEmpty()
            && !deletedPost->hidden;

        if (removeLikeOfficialClient) {
            if (hasRootCountEstimate) {
                --rootCountAdjustment;
            }
            removeLogicalRange(index, 1);
        } else {
            // Mattermost keeps a deleted placeholder when another user's post
            // disappears, while deletion of the current user's own post is a
            // structural removal. BackendPost::isDeleted already contains the
            // tombstone state set by BackendChannel::deletePost().
            emit itemsChanged(index, index);
        }
    });
    connect(&channel, &BackendChannel::onNewPosts, this,
            [this](const ChannelNewPosts&) {
        if (!hasRootCountEstimate) {
            return;
        }
        resizeLogicalTail(currentLogicalCount());
    });
}

int ChannelPostSource::ensurePostIndex(const QString& postId)
{
    // A cached semantic target is not enough to manufacture adjacency. Random
    // navigation must first adopt a bounded server context so the target cannot
    // be overwritten by an absolute page derived from a guessed index.
    return indexOfPost(postId);
}

bool ChannelPostSource::adoptNavigationContext(const QString& targetPostId,
                                               const QStringList& chronologicalIds,
                                               bool reachedOldest,
                                               bool reachedNewest)
{
    if (!hasRootCountEstimate || postIds.isEmpty() || targetPostId.isEmpty()) {
        return indexOfPost(targetPostId) >= 0;
    }

    QStringList validIds;
    validIds.reserve(chronologicalIds.size());
    QSet<QString> seen;
    for (const QString& id : chronologicalIds) {
        if (id.isEmpty() || seen.contains(id)) {
            continue;
        }
        BackendPost* post = channel.postIdToPost.value(id, nullptr);
        if (!post || post->hidden || !post->root_id.isEmpty()) {
            continue;
        }
        seen.insert(id);
        validIds.push_back(id);
    }

    const int targetOffset = validIds.indexOf(targetPostId);
    if (targetOffset < 0) {
        return false;
    }

    // Determine exact placement from the entire fetched context before slicing
    // it for the first paint. An authoritative overlap 16..30 rows away from the
    // target is still valuable even though only 15 rows per side are rendered.
    const int validCount = static_cast<int>(validIds.size());
    const int logicalCount = static_cast<int>(postIds.size());
    bool fullExact = false;
    int fullExactFirst = -1;
    const auto acceptFullExactFirst = [&](int candidate) {
        if (candidate < 0 || candidate + validCount > logicalCount) {
            return false;
        }
        if (!fullExact) {
            fullExact = true;
            fullExactFirst = candidate;
            return true;
        }
        return fullExactFirst == candidate;
    };

    if (reachedOldest && !acceptFullExactFirst(0)) {
        qCWarning(lcTimelineChannel) << "conflicting oldest full navigation context"
                                     << targetPostId;
        return false;
    }
    if (reachedNewest && !acceptFullExactFirst(logicalCount - validCount)) {
        qCWarning(lcTimelineChannel) << "conflicting newest full navigation context"
                                     << targetPostId;
        return false;
    }
    for (int offset = 0; offset < validCount; ++offset) {
        const QString& id = validIds.at(offset);
        const int existing = postIndexes.value(id, -1);
        if (existing < 0 || provisionalPostIds.contains(id)) {
            continue;
        }
        if (!acceptFullExactFirst(existing - offset)) {
            qCWarning(lcTimelineChannel) << "inconsistent full authoritative context overlap"
                                         << targetPostId << id << existing << offset;
            return false;
        }
    }

    // A semantic jump needs only the immediate neighbourhood. PostRepository may
    // have fetched a larger compatibility window, but publishing more than
    // 15 + target + 15 makes the initial materialization unnecessarily noisy.
    const int selectedFirst = std::max(0, targetOffset - 15);
    const int selectedLast = std::min(validCount - 1, targetOffset + 15);
    const QStringList selected = validIds.mid(selectedFirst,
                                              selectedLast - selectedFirst + 1);
    const bool selectedReachedOldest = reachedOldest && selectedFirst == 0;
    const bool selectedReachedNewest = reachedNewest && selectedLast == validCount - 1;
    const int exactSelectedFirst = fullExact ? fullExactFirst + selectedFirst : -1;
    return placeNavigationContext(targetPostId, selected,
                                  selectedReachedOldest, selectedReachedNewest,
                                  exactSelectedFirst);
}

void ChannelPostSource::requestRange(int first,
                                     int last,
                                     RequestReason reason,
                                     quint64 generation)
{
    Q_UNUSED(reason)
    Q_UNUSED(generation)

    if (!hasRootCountEstimate || postIds.isEmpty()) {
        emit rangeRequestFinished(first, last);
        return;
    }

    const int requestedFirst = std::max(0, first);
    const int requestedLast = std::min(static_cast<int>(postIds.size()) - 1, last);
    if (requestedLast < requestedFirst) {
        emit rangeRequestFinished(first, last);
        return;
    }

    // total_msg_count_root is only an initial coordinate estimate. Deleted
    // roots can make it too large, while join/leave and other count-excluded
    // system roots returned by /posts can make it too small. Large top-edge
    // seeks start 3% inside that estimate and repair the oldest boundary in
    // either direction before ordinary page placement resumes.
    if (requestedFirst == 0 && !oldestBoundaryFastPathTried
        && initialBoundaryProbePages() > 1) {
        oldestBoundaryFastPathTried = true;
        QPointer<ChannelPostSource> guard(this);
        probeEstimatedOldestBoundary([guard, first, last] {
            if (guard) {
                emit guard->rangeRequestFinished(first, last);
            }
        });
        return;
    }

    int firstMissing = requestedFirst;
    while (firstMissing <= requestedLast && isAvailable(firstMissing)) {
        ++firstMissing;
    }
    if (firstMissing > requestedLast) {
        emit rangeRequestFinished(first, last);
        return;
    }

    int lastMissing = requestedLast;
    while (lastMissing > firstMissing && isAvailable(lastMissing)) {
        --lastMissing;
    }

    // Mattermost channel history has one paging unit here: ten root posts.
    // Logical list blocks are anchored at the oldest end while Mattermost page
    // numbers are anchored at the newest end, so one ten-item logical request
    // can straddle two server pages. Load every absolute page intersecting the
    // missing range; do not turn already known post identities into paging
    // cursors. Those identities are useful for semantic-position estimation and
    // overlap reconciliation, not for choosing the next HTTP request boundary.
    const int newestPage = pageForIndex(lastMissing);
    const int oldestPage = pageForIndex(firstMissing);
    const int pageCount = oldestPage - newestPage + 1;
    if (pageCount <= 0) {
        emit rangeRequestFinished(first, last);
        return;
    }

    QPointer<ChannelPostSource> guard(this);
    auto pending = std::make_shared<int>(pageCount);
    const auto finishPage = [guard, pending, first, last] {
        if (!guard) {
            return;
        }
        if (--(*pending) == 0) {
            emit guard->rangeRequestFinished(first, last);
        }
    };

    for (int page = newestPage; page <= oldestPage; ++page) {
        qCDebug(lcTimelineChannel).nospace()
            << "RANGE_PAGE requested=[" << requestedFirst << ',' << requestedLast
            << "] missing=[" << firstMissing << ',' << lastMissing
            << "] page=" << page << " perPage=" << ServerPageSize;
        PostTimelineService::instance(backend).loadChannelPage(
            channel, page, ServerPageSize,
            [guard, page, requestedFirst, oldestPage, finishPage](
                const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }
                if (!result.success) {
                    finishPage();
                    return;
                }
                if (result.postIds.isEmpty()) {
                    guard->resolveOldestBoundary(page, finishPage);
                    return;
                }

                guard->placePage(page, result.postIds);

                // For small estimates we deliberately take the normal ten-post
                // path first. If the estimated oldest page is full, however,
                // the count may be an underestimate (for example because
                // join/leave system posts are excluded from the counter). Keep
                // this request open and search farther into older pages.
                if (requestedFirst == 0 && page == oldestPage
                    && result.postIds.size() == ServerPageSize
                    && !guard->oldestBoundaryFastPathTried) {
                    guard->resolveOldestBoundaryFromNonEmpty(page, finishPage);
                    return;
                }
                finishPage();
            });
    }
}

void ChannelPostSource::probeEstimatedOldestBoundary(
    std::function<void()> completion)
{
    if (completion) {
        oldestBoundaryWaiters.push_back(std::move(completion));
    }
    if (oldestBoundaryProbeInFlight || postIds.isEmpty()) {
        return;
    }

    oldestBoundaryProbeInFlight = true;
    oldestBoundaryNonEmptyPage = -1;
    oldestBoundaryEmptyPage = -1;
    oldestBoundaryProbeStep = initialBoundaryProbePages();
    oldestBoundarySearchLimitPage = pageForIndex(0);
    oldestBoundaryMaterializedFullPage = -1;

    // Start at the expected deletion distance instead of spending a round trip
    // on the estimated oldest page. Empty means the estimate was too large and
    // we step inward. Existing data is searched outward to the estimate; if the
    // estimated page is full, search continues beyond it because count-excluded
    // system roots can make total_msg_count_root an underestimate as well.
    const int page = std::max(0,
                              oldestBoundarySearchLimitPage - oldestBoundaryProbeStep);
    const int offset = page * ServerPageSize;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_INITIAL page=" << page
        << " offset=" << offset
        << " limit=" << oldestBoundarySearchLimitPage
        << " step=" << oldestBoundaryProbeStep
        << " perPage=1";

    QPointer<ChannelPostSource> guard(this);
    PostTimelineService::instance(backend).loadChannelPage(
        channel, offset, 1,
        [guard, page, offset](const PostTimelineService::Page& result) {
            if (!guard || !guard->oldestBoundaryProbeInFlight) {
                return;
            }
            if (!result.success) {
                guard->finishOldestBoundaryProbe();
                return;
            }

            const bool exists = !result.postIds.isEmpty();
            qCDebug(lcTimelineChannel).nospace()
                << "OLDEST_BOUNDARY_INITIAL_RESULT page=" << page
                << " offset=" << offset
                << " exists=" << exists;

            if (exists) {
                guard->oldestBoundaryNonEmptyPage = page;
            } else {
                guard->oldestBoundaryEmptyPage = page;
            }
            guard->probeOldestBoundary();
        });
}

void ChannelPostSource::resolveOldestBoundary(int emptyPage,
                                              std::function<void()> completion)
{
    if (completion) {
        oldestBoundaryWaiters.push_back(std::move(completion));
    }

    oldestBoundaryFastPathTried = true;
    emptyPage = std::max(0, emptyPage);
    if (oldestBoundaryProbeInFlight) {
        if (oldestBoundaryEmptyPage < 0 || emptyPage < oldestBoundaryEmptyPage) {
            oldestBoundaryEmptyPage = emptyPage;
        }
        return;
    }

    oldestBoundaryProbeInFlight = true;
    oldestBoundaryNonEmptyPage = -1;
    oldestBoundaryEmptyPage = emptyPage;
    oldestBoundaryProbeStep = initialBoundaryProbePages();
    oldestBoundarySearchLimitPage = -1;
    oldestBoundaryMaterializedFullPage = -1;
    probeOldestBoundary();
}

void ChannelPostSource::resolveOldestBoundaryFromNonEmpty(
    int nonEmptyPage, std::function<void()> completion)
{
    if (completion) {
        oldestBoundaryWaiters.push_back(std::move(completion));
    }

    oldestBoundaryFastPathTried = true;
    nonEmptyPage = std::max(0, nonEmptyPage);
    if (oldestBoundaryProbeInFlight) {
        if (oldestBoundaryEmptyPage < 0 || nonEmptyPage < oldestBoundaryEmptyPage) {
            oldestBoundaryNonEmptyPage = std::max(oldestBoundaryNonEmptyPage,
                                                  nonEmptyPage);
            oldestBoundaryMaterializedFullPage = std::max(
                oldestBoundaryMaterializedFullPage, nonEmptyPage);
        }
        return;
    }

    oldestBoundaryProbeInFlight = true;
    oldestBoundaryNonEmptyPage = nonEmptyPage;
    oldestBoundaryEmptyPage = -1;
    // First check the adjacent page. Exact multiples of ten are common, so one
    // cheap probe should prove them before exponential outward stepping begins.
    oldestBoundaryProbeStep = 1;
    oldestBoundarySearchLimitPage = -1;
    oldestBoundaryMaterializedFullPage = nonEmptyPage;
    probeOldestBoundary();
}

void ChannelPostSource::probeOldestBoundary()
{
    if (!oldestBoundaryProbeInFlight) {
        return;
    }

    if (oldestBoundaryEmptyPage == 0) {
        reconcileRootCount(0);
        finishOldestBoundaryProbe();
        return;
    }

    // A full materialized page followed immediately by known emptiness proves
    // an exact multiple-of-ten boundary without re-fetching that page.
    if (oldestBoundaryNonEmptyPage >= 0
        && oldestBoundaryEmptyPage == oldestBoundaryNonEmptyPage + 1
        && oldestBoundaryMaterializedFullPage == oldestBoundaryNonEmptyPage) {
        reconcileRootCount(oldestBoundaryEmptyPage * ServerPageSize);
        finishOldestBoundaryProbe();
        return;
    }

    // If the 3% heuristic is only one page, materializing the candidate block
    // is cheaper than maintaining a separate probe phase. Once binary search
    // leaves at most two unknown pages, stop spending one-root probes on them:
    // materialize the first unknown ten-post page instead. A short/empty page
    // resolves the boundary immediately; a full page needs at most one adjacent
    // ten-post page and both payloads are useful to the viewport/prefetch window.
    if ((oldestBoundaryNonEmptyPage < 0 && oldestBoundaryProbeStep == 1)
        || (oldestBoundaryNonEmptyPage >= 0 && oldestBoundaryEmptyPage >= 0
            && oldestBoundaryEmptyPage - oldestBoundaryNonEmptyPage <= 3)) {
        if (oldestBoundaryNonEmptyPage < 0) {
            oldestBoundaryProbeStep = 2;
            loadOldestBoundaryPage(std::max(0, oldestBoundaryEmptyPage - 1));
        } else if (oldestBoundaryEmptyPage == oldestBoundaryNonEmptyPage + 1) {
            loadOldestBoundaryPage(oldestBoundaryNonEmptyPage);
        } else {
            loadOldestBoundaryPage(oldestBoundaryNonEmptyPage + 1);
        }
        return;
    }

    int page = -1;
    if (oldestBoundaryNonEmptyPage >= 0 && oldestBoundaryEmptyPage < 0) {
        if (oldestBoundarySearchLimitPage >= 0) {
            // The initial 3% probe found data. Walk outward toward the reported
            // boundary without assuming it is empty.
            if (oldestBoundaryNonEmptyPage >= oldestBoundarySearchLimitPage) {
                loadOldestBoundaryPage(oldestBoundaryNonEmptyPage);
                return;
            }
            page = oldestBoundaryNonEmptyPage
                + (oldestBoundarySearchLimitPage - oldestBoundaryNonEmptyPage + 1) / 2;
        } else {
            // A full reported-boundary page proves that the count may be too
            // small. Probe one adjacent page first, then grow the outward step
            // exponentially. Existence is monotonic in absolute page space.
            const int step = std::max(1, oldestBoundaryProbeStep);
            const qint64 candidate = static_cast<qint64>(oldestBoundaryNonEmptyPage)
                + step;
            if (candidate > std::numeric_limits<int>::max()) {
                finishOldestBoundaryProbe();
                return;
            }
            page = static_cast<int>(candidate);
            if (step == 1) {
                oldestBoundaryProbeStep = std::max(2, initialBoundaryProbePages());
            } else {
                oldestBoundaryProbeStep = std::min(
                    std::numeric_limits<int>::max() / 2, step * 2);
            }
        }
    } else if (oldestBoundaryNonEmptyPage < 0) {
        page = std::max(0, oldestBoundaryEmptyPage - oldestBoundaryProbeStep);
        oldestBoundaryProbeStep = std::min(oldestBoundaryEmptyPage + 1,
                                           oldestBoundaryProbeStep * 2);
    } else {
        page = oldestBoundaryNonEmptyPage
            + (oldestBoundaryEmptyPage - oldestBoundaryNonEmptyPage) / 2;
    }

    const int offset = page * ServerPageSize;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_PROBE page=" << page
        << " offset=" << offset
        << " nonEmpty=" << oldestBoundaryNonEmptyPage
        << " empty=" << oldestBoundaryEmptyPage
        << " step=" << oldestBoundaryProbeStep
        << " perPage=1";

    QPointer<ChannelPostSource> guard(this);
    PostTimelineService::instance(backend).loadChannelPage(
        channel, offset, 1,
        [guard, page, offset](const PostTimelineService::Page& result) {
            if (!guard || !guard->oldestBoundaryProbeInFlight) {
                return;
            }
            if (!result.success) {
                guard->finishOldestBoundaryProbe();
                return;
            }

            const bool exists = !result.postIds.isEmpty();
            qCDebug(lcTimelineChannel).nospace()
                << "OLDEST_BOUNDARY_RESULT page=" << page
                << " offset=" << offset
                << " exists=" << exists;

            if (exists) {
                guard->oldestBoundaryNonEmptyPage = std::max(
                    guard->oldestBoundaryNonEmptyPage, page);
            } else if (guard->oldestBoundaryEmptyPage < 0) {
                guard->oldestBoundaryEmptyPage = page;
            } else {
                guard->oldestBoundaryEmptyPage = std::min(
                    guard->oldestBoundaryEmptyPage, page);
            }
            guard->probeOldestBoundary();
        });
}

void ChannelPostSource::loadOldestBoundaryPage(int page)
{
    if (!oldestBoundaryProbeInFlight) {
        return;
    }

    page = std::max(0, page);
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_PAGE page=" << page
        << " perPage=" << ServerPageSize;

    QPointer<ChannelPostSource> guard(this);
    PostTimelineService::instance(backend).loadChannelPage(
        channel, page, ServerPageSize,
        [guard, page](const PostTimelineService::Page& result) {
            if (!guard || !guard->oldestBoundaryProbeInFlight) {
                return;
            }
            if (!result.success) {
                guard->finishOldestBoundaryProbe();
                return;
            }

            qCDebug(lcTimelineChannel).nospace()
                << "OLDEST_BOUNDARY_PAGE_RESULT page=" << page
                << " returned=" << result.postIds.size();

            if (result.postIds.isEmpty()) {
                if (guard->oldestBoundaryEmptyPage < 0) {
                    guard->oldestBoundaryEmptyPage = page;
                } else {
                    guard->oldestBoundaryEmptyPage = std::min(
                        guard->oldestBoundaryEmptyPage, page);
                }
                if (guard->oldestBoundaryNonEmptyPage >= page) {
                    guard->oldestBoundaryNonEmptyPage = -1;
                    guard->oldestBoundaryMaterializedFullPage = -1;
                }
                if (guard->oldestBoundaryNonEmptyPage >= 0
                    && guard->oldestBoundaryEmptyPage
                        == guard->oldestBoundaryNonEmptyPage + 1) {
                    if (guard->oldestBoundaryMaterializedFullPage
                        == guard->oldestBoundaryNonEmptyPage) {
                        guard->reconcileRootCount(
                            guard->oldestBoundaryEmptyPage * ServerPageSize);
                        guard->finishOldestBoundaryProbe();
                    } else {
                        guard->loadOldestBoundaryPage(
                            guard->oldestBoundaryNonEmptyPage);
                    }
                    return;
                }
                guard->probeOldestBoundary();
                return;
            }

            guard->oldestBoundaryNonEmptyPage = std::max(
                guard->oldestBoundaryNonEmptyPage, page);
            const int returned = static_cast<int>(result.postIds.size());
            if (returned < ServerPageSize) {
                guard->placePage(page, result.postIds);
                guard->finishOldestBoundaryProbe();
                return;
            }

            guard->placePage(page, result.postIds);
            guard->oldestBoundaryMaterializedFullPage = page;
            if (guard->oldestBoundaryEmptyPage == page + 1) {
                guard->reconcileRootCount((page + 1) * ServerPageSize);
                guard->finishOldestBoundaryProbe();
                return;
            }
            if (guard->oldestBoundaryEmptyPage == page + 2) {
                guard->loadOldestBoundaryPage(page + 1);
                return;
            }

            // Reaching a full reported oldest page is not a boundary proof:
            // total_msg_count_root excludes join/leave and some other visible
            // system roots. Switch from the estimated limit to outward
            // exponential probing until /posts itself proves the edge.
            if (guard->oldestBoundaryEmptyPage < 0) {
                if (guard->oldestBoundarySearchLimitPage >= 0
                    && page >= guard->oldestBoundarySearchLimitPage) {
                    guard->oldestBoundarySearchLimitPage = -1;
                    guard->oldestBoundaryProbeStep = 1;
                }
                guard->probeOldestBoundary();
                return;
            }
            guard->probeOldestBoundary();
        });
}

void ChannelPostSource::finishOldestBoundaryProbe()
{
    auto waiters = std::move(oldestBoundaryWaiters);
    oldestBoundaryWaiters.clear();
    oldestBoundaryProbeInFlight = false;
    oldestBoundaryNonEmptyPage = -1;
    oldestBoundaryEmptyPage = -1;
    oldestBoundaryProbeStep = 1;
    oldestBoundarySearchLimitPage = -1;
    oldestBoundaryMaterializedFullPage = -1;

    for (auto& waiter : waiters) {
        if (waiter) {
            waiter();
        }
    }
}

void ChannelPostSource::reconcileRootCount(int actualCount)
{
    actualCount = std::max(0, actualCount);
    const int oldCount = static_cast<int>(postIds.size());

    // Preserve the discovered difference from Mattermost's message counter so
    // later metadata refreshes do not recreate a deleted-root phantom prefix or
    // discard count-excluded system roots. The server counter can subsequently
    // advance with normal messages; this signed adjustment advances with it.
    rootCountAdjustment = actualCount - std::max(0, channel.total_msg_count_root);

    if (actualCount == oldCount) {
        return;
    }

    if (actualCount > oldCount) {
        const int addPrefix = actualCount - oldCount;
        qCDebug(lcTimelineChannel).nospace()
            << "OLDEST_COUNT_RECONCILE addPrefix=" << addPrefix
            << " oldCount=" << oldCount
            << " actualCount=" << actualCount;
        insertLogicalPrefix(addPrefix);
        return;
    }

    const int removePrefix = oldCount - actualCount;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_COUNT_RECONCILE removePrefix=" << removePrefix
        << " oldCount=" << oldCount
        << " actualCount=" << actualCount;
    removeLogicalRange(0, removePrefix);
}

void ChannelPostSource::ensureMinimumRootCount(int minimumCount)
{
    minimumCount = std::max(0, minimumCount);
    const int oldCount = static_cast<int>(postIds.size());
    if (minimumCount <= oldCount) {
        return;
    }

    const int addPrefix = minimumCount - oldCount;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_COUNT_EXPAND addPrefix=" << addPrefix
        << " oldCount=" << oldCount
        << " minimumCount=" << minimumCount;
    insertLogicalPrefix(addPrefix);
}

void ChannelPostSource::insertLogicalPrefix(int count)
{
    count = std::max(0, count);
    if (count == 0) {
        return;
    }

    if (provisionalWindow.isValid()) {
        provisionalWindow.first += count;
    }
    insertEmptyLogicalSlots(0, count);
}

bool ChannelPostSource::canRequestBeforeFirst() const
{
    return !hasRootCountEstimate && moreBeforeFirst;
}

void ChannelPostSource::requestBeforeFirst(RequestReason reason, quint64 generation)
{
    Q_UNUSED(reason)
    Q_UNUSED(generation)

    if (hasRootCountEstimate || !moreBeforeFirst || beforeRequestInFlight) {
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
    if (hasRootCountEstimate) {
        const int adjustedServerCount = std::max(
            0, channel.total_msg_count_root + rootCountAdjustment);
        return std::max(static_cast<int>(postIds.size()), adjustedServerCount);
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

int ChannelPostSource::initialBoundaryProbePages() const
{
    const qint64 numerator = static_cast<qint64>(postIds.size())
        * InitialBoundaryProbePercent;
    const qint64 denominator = 100LL * ServerPageSize;
    return std::max(1, static_cast<int>((numerator + denominator - 1) / denominator));
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
        const QString& id = postIds.at(index);
        if (id.isEmpty() || !isAuthoritativePost(id) || id == post.id) {
            continue;
        }
        BackendPost* current = channel.postIdToPost.value(id, nullptr);
        if (!current) {
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

    const auto interpolate = [](int firstIndex, uint64_t firstTime,
                                int lastIndex, uint64_t lastTime,
                                uint64_t targetTime) {
        if (lastIndex <= firstIndex || lastTime <= firstTime) {
            return firstIndex;
        }
        const long double fraction = std::max<long double>(0.0L,
            std::min<long double>(1.0L,
                static_cast<long double>(targetTime - firstTime)
                    / static_cast<long double>(lastTime - firstTime)));
        return firstIndex + static_cast<int>(std::llround(
            fraction * static_cast<long double>(lastIndex - firstIndex)));
    };

    if (lowerIndex >= 0 && upperIndex >= 0) {
        if (lowerTime == upperTime) {
            return (lowerIndex + upperIndex) / 2;
        }
        if (upperIndex > lowerIndex && upperTime > lowerTime) {
            return interpolate(lowerIndex, lowerTime, upperIndex, upperTime, post.create_at);
        }
    }

    // One-sided authoritative knowledge only gives an inequality, not
    // adjacency. Use the corresponding channel time boundary as the other
    // interpolation anchor instead of the old and badly wrong index +/- 1.
    if (lowerIndex >= 0) {
        const uint64_t newestTime = std::max(channel.last_post_at, lowerTime);
        if (lowerIndex < count - 1 && newestTime > lowerTime
            && post.create_at >= lowerTime) {
            return interpolate(lowerIndex, lowerTime, count - 1, newestTime, post.create_at);
        }
        return lowerIndex;
    }
    if (upperIndex >= 0) {
        const uint64_t oldestTime = std::min(channel.create_at, upperTime);
        if (upperIndex > 0 && upperTime > oldestTime
            && post.create_at <= upperTime && post.create_at >= oldestTime) {
            return interpolate(0, oldestTime, upperIndex, upperTime, post.create_at);
        }
        return upperIndex;
    }

    const uint64_t oldest = channel.create_at;
    const uint64_t newest = std::max(channel.last_post_at, oldest);
    if (newest > oldest && post.create_at >= oldest) {
        return interpolate(0, oldest, count - 1, newest,
                           std::min(post.create_at, newest));
    }
    return count / 2;
}

int ChannelPostSource::findFreeWindowFirst(const QVector<QString>& ids,
                                           int windowSize,
                                           int preferredFirst) const
{
    const int count = static_cast<int>(ids.size());
    if (windowSize <= 0 || windowSize > count) {
        return -1;
    }

    const int maxFirst = count - windowSize;
    preferredFirst = std::max(0, std::min(preferredFirst, maxFirst));

    const auto isFree = [&ids, windowSize](int first) {
        for (int offset = 0; offset < windowSize; ++offset) {
            if (!ids.at(first + offset).isEmpty()) {
                return false;
            }
        }
        return true;
    };

    if (isFree(preferredFirst)) {
        return preferredFirst;
    }
    for (int distance = 1; distance <= maxFirst; ++distance) {
        const int before = preferredFirst - distance;
        if (before >= 0 && isFree(before)) {
            return before;
        }
        const int after = preferredFirst + distance;
        if (after <= maxFirst && isFree(after)) {
            return after;
        }
    }
    return -1;
}

bool ChannelPostSource::isAuthoritativePost(const QString& postId) const
{
    return !postId.isEmpty() && postIndexes.contains(postId)
        && !provisionalPostIds.contains(postId);
}

bool ChannelPostSource::placeNavigationContext(const QString& targetPostId,
                                               const QStringList& chronologicalIds,
                                               bool reachedOldest,
                                               bool reachedNewest,
                                               int exactFirstHint)
{
    const QStringList ids = uniqueChronological(chronologicalIds);
    const int targetOffset = ids.indexOf(targetPostId);
    const int contextCount = static_cast<int>(ids.size());
    const int count = static_cast<int>(postIds.size());
    if (targetOffset < 0 || contextCount <= 0 || contextCount > count) {
        return false;
    }

    BackendPost* target = channel.postIdToPost.value(targetPostId, nullptr);
    if (!target || target->hidden || !target->root_id.isEmpty()) {
        return false;
    }

    const ProvisionalWindow oldWindow = provisionalWindow;
    const int oldTargetIndex = oldWindow.isValid() && oldWindow.targetPostId == targetPostId
        ? indexOfPost(targetPostId) : -1;

    bool exactPlacement = false;
    int exactFirst = -1;
    const auto acceptExactFirst = [&](int candidate) {
        if (candidate < 0 || candidate + contextCount > count) {
            return false;
        }
        if (!exactPlacement) {
            exactPlacement = true;
            exactFirst = candidate;
            return true;
        }
        return exactFirst == candidate;
    };

    if (exactFirstHint >= 0 && !acceptExactFirst(exactFirstHint)) {
        qCWarning(lcTimelineChannel) << "invalid exact navigation context hint"
                                     << targetPostId << exactFirstHint;
        return false;
    }
    if (reachedOldest && !acceptExactFirst(0)) {
        qCWarning(lcTimelineChannel) << "conflicting oldest navigation context" << targetPostId;
        return false;
    }
    if (reachedNewest && !acceptExactFirst(count - contextCount)) {
        qCWarning(lcTimelineChannel) << "conflicting newest navigation context" << targetPostId;
        return false;
    }

    for (int offset = 0; offset < contextCount; ++offset) {
        const QString& id = ids.at(offset);
        const int existing = postIndexes.value(id, -1);
        if (existing < 0 || provisionalPostIds.contains(id)) {
            continue;
        }
        if (!acceptExactFirst(existing - offset)) {
            qCWarning(lcTimelineChannel) << "inconsistent authoritative context overlap"
                                         << targetPostId << id << existing << offset;
            return false;
        }
    }

    QVector<QString> next = postIds;
    if (!provisionalPostIds.isEmpty()) {
        for (int index = 0; index < next.size(); ++index) {
            if (provisionalPostIds.contains(next.at(index))) {
                next[index].clear();
            }
        }
    }

    int first = -1;
    if (exactPlacement) {
        first = exactFirst;
        for (int offset = 0; offset < contextCount; ++offset) {
            const QString& existing = next.at(first + offset);
            if (!existing.isEmpty() && existing != ids.at(offset)) {
                qCWarning(lcTimelineChannel) << "authoritative navigation context collision"
                                             << targetPostId << first + offset
                                             << existing << ids.at(offset);
                return false;
            }
        }
    } else {
        const int preferredTarget = oldTargetIndex >= 0
            ? oldTargetIndex : estimateIndexForPost(*target);
        const int preferredFirst = preferredTarget - targetOffset;
        first = findFreeWindowFirst(next, contextCount, preferredFirst);
        if (first < 0) {
            qCWarning(lcTimelineChannel) << "no free provisional context span"
                                         << targetPostId << preferredFirst << contextCount;
            return false;
        }
    }

    for (int offset = 0; offset < contextCount; ++offset) {
        next[first + offset] = ids.at(offset);
    }

    postIds = std::move(next);
    rebuildIndex();

    if (exactPlacement) {
        provisionalPostIds.clear();
        provisionalWindow.clear();
    } else {
        provisionalPostIds.clear();
        for (const QString& id : ids) {
            provisionalPostIds.insert(id);
        }
        provisionalWindow.targetPostId = targetPostId;
        provisionalWindow.postIds = ids;
        provisionalWindow.first = first;
        provisionalWindow.reachedOldest = reachedOldest;
        provisionalWindow.reachedNewest = reachedNewest;
    }

    const int newLast = first + contextCount - 1;
    if (oldWindow.isValid()
        && (oldWindow.first != first || oldWindow.last() != newLast)) {
        emit itemsChanged(oldWindow.first, oldWindow.last());
    }
    emit itemsChanged(first, newLast);
    emit rangeAvailable(first, newLast);

    qCDebug(lcTimelineChannel).nospace()
        << "NAV_CONTEXT target=" << targetPostId
        << " first=" << first
        << " last=" << newLast
        << " targetIndex=" << first + targetOffset
        << " exact=" << exactPlacement
        << " reachedOldest=" << reachedOldest
        << " reachedNewest=" << reachedNewest;
    return true;
}

void ChannelPostSource::seedCachedPosts()
{
    if (!hasRootCountEstimate) {
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
    if (hasRootCountEstimate || channel.last_post_at == 0) {
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

void ChannelPostSource::removeLogicalRange(int first, int count)
{
    first = std::max(0, std::min(first, static_cast<int>(postIds.size())));
    count = std::max(0, std::min(count, static_cast<int>(postIds.size()) - first));
    if (count == 0) {
        return;
    }

    const int last = first + count - 1;

    if (provisionalWindow.isValid()) {
        const int oldFirst = provisionalWindow.first;
        const int oldLast = provisionalWindow.last();
        const int removedBeforeWindow = first < oldFirst
            ? std::min(count, oldFirst - first) : 0;
        const int overlapFirst = std::max(first, oldFirst);
        const int overlapLast = std::min(last, oldLast);

        if (overlapFirst <= overlapLast) {
            const int offset = overlapFirst - oldFirst;
            const int overlapCount = overlapLast - overlapFirst + 1;
            const QStringList removedIds = provisionalWindow.postIds.mid(offset, overlapCount);
            const bool targetRemoved = removedIds.contains(provisionalWindow.targetPostId);
            for (const QString& id : removedIds) {
                provisionalPostIds.remove(id);
            }
            for (int i = 0; i < overlapCount; ++i) {
                provisionalWindow.postIds.removeAt(offset);
            }
            provisionalWindow.first -= removedBeforeWindow;
            if (targetRemoved || provisionalWindow.postIds.isEmpty()) {
                provisionalWindow.clear();
            }
        } else if (last < oldFirst) {
            provisionalWindow.first -= count;
        }
    }

    for (int index = first; index <= last; ++index) {
        provisionalPostIds.remove(postIds.at(index));
    }
    eraseLogicalSlots(first, count);
}

void ChannelPostSource::placePage(int page, const QStringList& chronologicalIds)
{
    if (!hasRootCountEstimate || postIds.isEmpty() || chronologicalIds.isEmpty()) {
        return;
    }

    const int returned = static_cast<int>(chronologicalIds.size());

    // Absolute /posts pages are the timeline authority. A short page proves the
    // exact oldest boundary. A full page only proves a minimum count; grow at
    // the oldest side so already known newest-anchored page mappings keep their
    // indices. This handles total_msg_count_root underestimates caused by system
    // roots that are visible in /posts but excluded from channel message counts.
    if (returned < ServerPageSize) {
        reconcileRootCount(page * ServerPageSize + returned);
    } else {
        ensureMinimumRootCount((page + 1) * ServerPageSize);
    }

    const int first = std::max(0,
        static_cast<int>(postIds.size()) - page * ServerPageSize - returned);

    const int count = std::min(returned,
                               static_cast<int>(postIds.size()) - first);
    if (count <= 0) {
        return;
    }
    const int last = first + count - 1;

    bool touchesProvisionalIdentity = false;
    const QStringList pageIds = chronologicalIds.mid(0, count);
    for (const QString& id : pageIds) {
        touchesProvisionalIdentity = touchesProvisionalIdentity
            || provisionalPostIds.contains(id);
        provisionalPostIds.remove(id);
    }
    const ExactWindowMutation mutation = assignExactWindow(first, pageIds);

    // An absolute page that happens to intersect the provisional island by ID
    // provides the missing exact offset. Re-adopt the whole local context before
    // publishing page changes so the target never disappears between states.
    if (touchesProvisionalIdentity && provisionalWindow.isValid()) {
        const ProvisionalWindow window = provisionalWindow;
        placeNavigationContext(window.targetPostId, window.postIds,
                               window.reachedOldest, window.reachedNewest);
    }

    // Re-fetching an already known page is deliberately a no-op. The shared
    // publisher emits nothing unless the identity mapping actually changed.
    publishExactWindow(mutation);
}

void ChannelPostSource::prependDiscovered(const QStringList& chronologicalIds)
{
    if (chronologicalIds.isEmpty()) {
        return;
    }

    const int inserted = static_cast<int>(chronologicalIds.size());
    insertEmptyLogicalSlots(0, inserted);
    publishExactWindow(assignExactWindow(0, chronologicalIds));
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

    int count = hasRootCountEstimate ? currentLogicalCount() : static_cast<int>(postIds.size()) + 1;
    if (count <= postIds.size()) {
        count = static_cast<int>(postIds.size()) + 1;
    }
    resizeLogicalTail(count);
    const int index = count - 1;
    publishExactWindow(assignExactWindow(index, QStringList { post.id }));
}

} // namespace Mattermost
