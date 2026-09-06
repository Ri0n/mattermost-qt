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
        if (index < 0) {
            return;
        }

        BackendPost* deletedPost = channel.postIdToPost.value(postId, nullptr);
        const bool removeLikeOfficialClient = deletedPost
            && deletedPost->isOwnPost()
            && deletedPost->root_id.isEmpty()
            && !deletedPost->hidden;

        if (removeLikeOfficialClient) {
            if (exactRootCount) {
                ++rootCountOverestimate;
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
    if (!exactRootCount || postIds.isEmpty() || targetPostId.isEmpty()) {
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

    // A large channel is unlikely to have an exact oldest page when the server
    // count still includes deleted roots. Avoid downloading one or two guessed
    // ten-post pages just to discover that they are empty: validate the
    // estimated oldest page with one root first, then jump inward by the 3%
    // heuristic if that cheap probe is empty. Small channels keep the normal
    // ten-post path because their likely oldest block is cheap and useful.
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
            [guard, page, finishPage](const PostTimelineService::Page& result) {
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

    const int page = pageForIndex(0);
    const int offset = page * ServerPageSize;
    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_BOUNDARY_INITIAL page=" << page
        << " offset=" << offset
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
                // The estimate reached real data, so turn the cheap validation
                // into useful materialization. A short page proves the exact
                // oldest boundary; a full page is still the best first block for
                // the requested top viewport.
                guard->oldestBoundaryNonEmptyPage = page;
                guard->loadOldestBoundaryPage(page);
                return;
            }

            guard->oldestBoundaryEmptyPage = page;
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

    // If the 3% heuristic is only one page, materializing the candidate block
    // is cheaper than maintaining a separate probe phase. Likewise, once binary
    // search leaves at most one unknown page, fetch the page immediately before
    // the known empty boundary. It either proves the boundary or shrinks it by
    // one page, and the payload is useful to the top viewport/prefetch window.
    if ((oldestBoundaryNonEmptyPage < 0 && oldestBoundaryProbeStep == 1)
        || (oldestBoundaryNonEmptyPage >= 0
            && oldestBoundaryEmptyPage - oldestBoundaryNonEmptyPage <= 2)) {
        if (oldestBoundaryNonEmptyPage < 0) {
            oldestBoundaryProbeStep = 2;
        }
        loadOldestBoundaryPage(std::max(0, oldestBoundaryEmptyPage - 1));
        return;
    }

    int page = -1;
    if (oldestBoundaryNonEmptyPage < 0) {
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
                }
                guard->probeOldestBoundary();
                return;
            }

            guard->oldestBoundaryNonEmptyPage = std::max(
                guard->oldestBoundaryNonEmptyPage, page);
            const int returned = static_cast<int>(result.postIds.size());
            if (returned < ServerPageSize) {
                guard->reconcileRootCount(page * ServerPageSize + returned);
                guard->placePage(page, result.postIds);
                guard->finishOldestBoundaryProbe();
                return;
            }

            guard->placePage(page, result.postIds);
            if (guard->oldestBoundaryEmptyPage == page + 1) {
                guard->reconcileRootCount((page + 1) * ServerPageSize);
                guard->finishOldestBoundaryProbe();
                return;
            }

            // A proactive top check can reach a full estimated oldest page
            // without having an independently proven empty page after it. The
            // block is still useful materialization; avoid inventing boundary
            // evidence from the approximate count itself.
            if (guard->oldestBoundaryEmptyPage < 0) {
                guard->finishOldestBoundaryProbe();
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

    for (auto& waiter : waiters) {
        if (waiter) {
            waiter();
        }
    }
}

void ChannelPostSource::reconcileRootCount(int actualCount)
{
    actualCount = std::max(0, actualCount);
    const int removePrefix = static_cast<int>(postIds.size()) - actualCount;
    if (removePrefix <= 0) {
        return;
    }

    qCDebug(lcTimelineChannel).nospace()
        << "OLDEST_COUNT_RECONCILE removePrefix=" << removePrefix
        << " oldCount=" << postIds.size()
        << " actualCount=" << actualCount;
    rootCountOverestimate += removePrefix;
    removeLogicalRange(0, removePrefix);
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
        const int correctedServerCount = std::max(
            0, channel.total_msg_count_root - rootCountOverestimate);
        return std::max(static_cast<int>(postIds.size()), correctedServerCount);
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
    for (int i = 0; i < count; ++i) {
        postIds.removeAt(first);
    }
    rebuildIndex();
    emit itemsRemoved(first, count);
}

void ChannelPostSource::placePage(int page, const QStringList& chronologicalIds)
{
    if (!exactRootCount || postIds.isEmpty() || chronologicalIds.isEmpty()) {
        return;
    }

    // /channels/{id}/posts omits deleted roots while total_msg_count_root
    // may continue counting them. A short absolute page is authoritative proof
    // of the real oldest boundary, including page zero for small channels.
    if (chronologicalIds.size() < ServerPageSize) {
        reconcileRootCount(
            page * ServerPageSize + static_cast<int>(chronologicalIds.size()));
    }

    const int first = std::max(0,
        static_cast<int>(postIds.size()) - page * ServerPageSize
            - static_cast<int>(chronologicalIds.size()));

    const int count = std::min(static_cast<int>(chronologicalIds.size()),
                               static_cast<int>(postIds.size()) - first);
    if (count <= 0) {
        return;
    }
    const int last = first + count - 1;

    bool touchesProvisionalIdentity = false;
    bool mappingChanged = false;
    QSet<int> concreteChanged;

    for (int offset = 0; offset < count; ++offset) {
        const QString& id = chronologicalIds.at(offset);
        if (provisionalPostIds.contains(id)) {
            touchesProvisionalIdentity = true;
        }
        const int existing = postIndexes.value(id, -1);
        if (existing >= 0 && (existing < first || existing > last)
            && !postIds.at(existing).isEmpty()) {
            postIds[existing].clear();
            concreteChanged.insert(existing);
            mappingChanged = true;
        }
    }

    for (int offset = 0; offset < count; ++offset) {
        const int index = first + offset;
        const QString& id = chronologicalIds.at(offset);
        if (postIds.at(index) != id) {
            if (!postIds.at(index).isEmpty()) {
                concreteChanged.insert(index);
            }
            postIds[index] = id;
            mappingChanged = true;
        }
        provisionalPostIds.remove(id);
    }
    rebuildIndex();

    // An absolute page that happens to intersect the provisional island by ID
    // provides the missing exact offset. Re-adopt the whole local context before
    // publishing page changes so the target never disappears between states.
    if (touchesProvisionalIdentity && provisionalWindow.isValid()) {
        const ProvisionalWindow window = provisionalWindow;
        placeNavigationContext(window.targetPostId, window.postIds,
                               window.reachedOldest, window.reachedNewest);
    }

    // Re-fetching an already known page must be a no-op. In particular, do not
    // emit rangeAvailable/itemsChanged for identical identities: both signals
    // schedule another synchronization, which used to clear request suppression
    // and immediately ask for the same impossible oldest range again.
    if (!mappingChanged) {
        return;
    }

    for (int index : std::as_const(concreteChanged)) {
        emit itemsChanged(index, index);
    }
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
