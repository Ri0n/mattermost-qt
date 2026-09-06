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

    // A semantic navigation island has exact local adjacency but only an
    // estimated global offset. Continue resolving it exclusively by identity.
    if (requestTouchesProvisionalWindow(requestedFirst, requestedLast)) {
        requestProvisionalRange(first, last);
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

    // A cursor request can return at most ServerPageSize adjacent rows.
    // Looking farther than that for an anchor is actively harmful: after a
    // remote thumb seek it makes us walk from the newest materialized island
    // toward the requested index one tiny request at a time. Search only the
    // distance that one cursor request can actually bridge.
    const int leftLimit = std::max(0, firstMissing - ServerPageSize);
    int leftAnchor = firstMissing - 1;
    while (leftAnchor >= leftLimit
           && !isAuthoritativePost(postIds.at(leftAnchor))) {
        --leftAnchor;
    }
    if (leftAnchor < leftLimit) {
        leftAnchor = -1;
    }

    const int rightLimit = std::min(static_cast<int>(postIds.size()) - 1,
                                    lastMissing + ServerPageSize);
    int rightAnchor = lastMissing + 1;
    while (rightAnchor <= rightLimit
           && !isAuthoritativePost(postIds.at(rightAnchor))) {
        ++rightAnchor;
    }
    if (rightAnchor > rightLimit) {
        rightAnchor = -1;
    }

    QPointer<ChannelPostSource> guard(this);
    auto finish = [guard, first, last] {
        if (guard) {
            emit guard->rangeRequestFinished(first, last);
        }
    };

    // Sequential scrolling is cursor-authoritative. Once one side of a
    // missing interval has a concrete identity, page-number arithmetic is no
    // longer allowed to move that identity or infer its adjacency.
    if (leftAnchor >= 0 || rightAnchor >= 0) {
        const bool useLeft = leftAnchor >= 0
            && (rightAnchor < 0
                || firstMissing - leftAnchor <= rightAnchor - lastMissing);

        if (useLeft) {
            const QString anchorId = postIds.at(leftAnchor);
            PostTimelineService::instance(backend).loadChannelAfter(
                channel, anchorId, ServerPageSize,
                [guard, anchorId, finish](const PostTimelineService::Page& result) {
                    if (!guard) {
                        return;
                    }

                    if (result.success) {
                        int anchorIndex = guard->indexOfPost(anchorId);
                        if (anchorIndex >= 0) {
                            const bool reachedNewest = result.nextPostId.isEmpty()
                                || static_cast<int>(result.postIds.size()) < ServerPageSize;

                            // If an identity cursor proves the newest boundary,
                            // every logical slot after the returned adjacency is
                            // a stale total_msg_count_root estimate. Remove it
                            // structurally instead of letting a later page shift
                            // already authoritative identities.
                            if (reachedNewest) {
                                const int expectedLast = anchorIndex
                                    + static_cast<int>(result.postIds.size());
                                const int removeCount = static_cast<int>(guard->postIds.size())
                                    - expectedLast - 1;
                                if (removeCount > 0) {
                                    guard->rootCountOverestimate += removeCount;
                                    guard->removeLogicalRange(expectedLast + 1,
                                                              removeCount);
                                }
                            }

                            QStringList context;
                            context.reserve(result.postIds.size() + 1);
                            context.push_back(anchorId);
                            context.append(result.postIds);
                            if (!result.postIds.isEmpty() || reachedNewest) {
                                guard->placeNavigationContext(anchorId, context,
                                                              false, reachedNewest);
                            }
                        }
                    }
                    finish();
                });
            return;
        }

        const QString anchorId = postIds.at(rightAnchor);
        PostTimelineService::instance(backend).loadChannelBefore(
            channel, anchorId, ServerPageSize,
            [guard, anchorId, finish](const PostTimelineService::Page& result) {
                if (!guard) {
                    return;
                }

                if (result.success) {
                    int anchorIndex = guard->indexOfPost(anchorId);
                    if (anchorIndex >= 0) {
                        const bool reachedOldest = result.prevPostId.isEmpty()
                            || static_cast<int>(result.postIds.size()) < ServerPageSize;

                        // A proven oldest boundary also tells us exactly
                        // how much total_msg_count_root overestimated the
                        // logical prefix. Removing that prefix shifts all
                        // newer authoritative identities together.
                        if (reachedOldest) {
                            const int phantomPrefix = anchorIndex
                                - static_cast<int>(result.postIds.size());
                            if (phantomPrefix > 0) {
                                guard->rootCountOverestimate += phantomPrefix;
                                guard->removeLogicalRange(0, phantomPrefix);
                            }
                        }

                        QStringList context = result.postIds;
                        context.push_back(anchorId);
                        if (!result.postIds.isEmpty() || reachedOldest) {
                            guard->placeNavigationContext(anchorId, context,
                                                          reachedOldest, false);
                        }
                    }
                }
                finish();
            });
        return;
    }

    // No authoritative identity is close enough to satisfy this request
    // with one cursor page. This is a random/remote seek, not sequential
    // scrolling. Use one wider absolute page as a provisional seed instead of
    // walking a distant island ten posts at a time.
    const int seedIndex = (firstMissing + lastMissing) / 2;
    const int page = (static_cast<int>(postIds.size()) - 1 - seedIndex) / SeekPageSize;
    qCDebug(lcTimelineChannel).nospace()
        << "RANGE_SEED requested=[" << requestedFirst << ',' << requestedLast
        << "] missing=[" << firstMissing << ',' << lastMissing
        << "] page=" << page << " perPage=" << SeekPageSize;
    PostTimelineService::instance(backend).loadChannelPage(
        channel, page, SeekPageSize,
        [guard, page, finish](const PostTimelineService::Page& result) {
            if (!guard) {
                return;
            }

            if (result.success && !result.postIds.isEmpty()) {
                const bool reachedOldest = result.prevPostId.isEmpty()
                    || static_cast<int>(result.postIds.size()) < SeekPageSize;
                const bool reachedNewest = page == 0;

                if (reachedOldest && page > 0) {
                    const int actualCount = page * SeekPageSize
                        + static_cast<int>(result.postIds.size());
                    const int phantomPrefix = static_cast<int>(guard->postIds.size()) - actualCount;
                    if (phantomPrefix > 0) {
                        guard->rootCountOverestimate += phantomPrefix;
                        guard->removeLogicalRange(0, phantomPrefix);
                    }
                }

                const QString targetId = result.postIds.at(result.postIds.size() / 2);
                guard->placeNavigationContext(targetId, result.postIds,
                                              reachedOldest, reachedNewest);
            }
            finish();
        });
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

bool ChannelPostSource::requestTouchesProvisionalWindow(int first, int last) const
{
    if (!provisionalWindow.isValid()) {
        return false;
    }
    return first <= provisionalWindow.last() + ServerPageSize
        && last >= provisionalWindow.first - ServerPageSize;
}

void ChannelPostSource::requestProvisionalRange(int first, int last)
{
    pendingProvisionalRequests.push_back(qMakePair(first, last));
    if (provisionalRequestInFlight || !provisionalWindow.isValid()) {
        if (!provisionalWindow.isValid()) {
            finishProvisionalRequests();
        }
        return;
    }

    const bool needBefore = first < provisionalWindow.first && !provisionalWindow.reachedOldest;
    const bool needAfter = last > provisionalWindow.last() && !provisionalWindow.reachedNewest;
    if (!needBefore && !needAfter) {
        finishProvisionalRequests();
        return;
    }

    struct ExpansionState {
        ProvisionalWindow base;
        PostTimelineService::Page before;
        PostTimelineService::Page after;
        bool needBefore = false;
        bool needAfter = false;
        int pending = 0;
    };

    provisionalRequestInFlight = true;
    auto state = std::make_shared<ExpansionState>();
    state->base = provisionalWindow;
    state->needBefore = needBefore;
    state->needAfter = needAfter;
    state->pending = (needBefore ? 1 : 0) + (needAfter ? 1 : 0);

    QPointer<ChannelPostSource> guard(this);
    const auto finishPart = [guard, state]() {
        if (!guard || --state->pending != 0) {
            return;
        }

        QStringList combined;
        if (state->needBefore && state->before.success) {
            combined.append(state->before.postIds);
        }
        combined.append(state->base.postIds);
        if (state->needAfter && state->after.success) {
            combined.append(state->after.postIds);
        }
        combined = uniqueChronological(combined);

        const bool reachedOldest = state->base.reachedOldest
            || (state->needBefore && state->before.success
                && (state->before.prevPostId.isEmpty()
                    || static_cast<int>(state->before.postIds.size()) < ServerPageSize));
        const bool reachedNewest = state->base.reachedNewest
            || (state->needAfter && state->after.success
                && (state->after.nextPostId.isEmpty()
                    || static_cast<int>(state->after.postIds.size()) < ServerPageSize));

        if (!combined.isEmpty()) {
            guard->placeNavigationContext(state->base.targetPostId, combined,
                                          reachedOldest, reachedNewest);
        }
        guard->finishProvisionalRequests();
    };

    if (needBefore) {
        PostTimelineService::instance(backend).loadChannelBefore(
            channel, provisionalWindow.postIds.first(), ServerPageSize,
            [state, finishPart](const PostTimelineService::Page& result) {
                state->before = result;
                finishPart();
            });
    }
    if (needAfter) {
        PostTimelineService::instance(backend).loadChannelAfter(
            channel, provisionalWindow.postIds.last(), ServerPageSize,
            [state, finishPart](const PostTimelineService::Page& result) {
                state->after = result;
                finishPart();
            });
    }
}

void ChannelPostSource::finishProvisionalRequests()
{
    provisionalRequestInFlight = false;
    const QVector<QPair<int, int>> requests = std::move(pendingProvisionalRequests);
    pendingProvisionalRequests.clear();
    for (const auto& request : requests) {
        emit rangeRequestFinished(request.first, request.second);
    }
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

    int first = std::max(0,
        static_cast<int>(postIds.size()) - page * ServerPageSize
            - static_cast<int>(chronologicalIds.size()));

    // /channels/{id}/posts omits deleted posts, while total_msg_count_root can
    // still leave the source with a larger logical estimate. A short non-zero
    // page is the oldest boundary, so a positive calculated first index is not a
    // real gap: it is exactly the number of phantom deleted-root slots. Remove
    // them structurally before publishing the page. For the reported 1070-row
    // DM, page 106 contained seven posts and proved that indices 0..2 did not
    // exist, which was the trigger for the infinite [0,9] seek/reload loop.
    if (page > 0
        && chronologicalIds.size() < ServerPageSize
        && first > 0) {
        qCDebug(lcTimelineChannel).nospace()
            << "OLDEST_COUNT_RECONCILE page=" << page
            << " removePrefix=" << first
            << " oldCount=" << postIds.size()
            << " returned=" << chronologicalIds.size();
        rootCountOverestimate += first;
        removeLogicalRange(0, first);
        first = std::max(0,
            static_cast<int>(postIds.size()) - page * ServerPageSize
                - static_cast<int>(chronologicalIds.size()));
    }

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
