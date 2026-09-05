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

    // A semantic jump needs only the immediate neighbourhood. PostRepository may
    // have fetched a larger compatibility window, but publishing more than
    // 15 + target + 15 makes the initial materialization unnecessarily noisy.
    const int selectedFirst = std::max(0, targetOffset - 15);
    const int selectedLast = std::min(validIds.size() - 1, targetOffset + 15);
    const QStringList selected = validIds.mid(selectedFirst,
                                              selectedLast - selectedFirst + 1);
    const bool selectedReachedOldest = reachedOldest && selectedFirst == 0;
    const bool selectedReachedNewest = reachedNewest && selectedLast == validIds.size() - 1;
    return placeNavigationContext(targetPostId, selected,
                                  selectedReachedOldest, selectedReachedNewest);
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

    // A provisional semantic window has exact local identity/order but only an
    // estimated global offset. Absolute page numbers in its neighbourhood are
    // therefore invalid provenance. Extend that island only with identity
    // cursors until it intersects an authoritative page or channel boundary.
    if (requestTouchesProvisionalWindow(requestedFirst, requestedLast)) {
        requestProvisionalRange(first, last);
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
                                               bool reachedNewest)
{
    const QStringList ids = uniqueChronological(chronologicalIds);
    const int targetOffset = ids.indexOf(targetPostId);
    const int count = static_cast<int>(postIds.size());
    if (targetOffset < 0 || ids.isEmpty() || ids.size() > count) {
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
        if (candidate < 0 || candidate + ids.size() > count) {
            return false;
        }
        if (!exactPlacement) {
            exactPlacement = true;
            exactFirst = candidate;
            return true;
        }
        return exactFirst == candidate;
    };

    if (reachedOldest && !acceptExactFirst(0)) {
        qCWarning(lcTimelineChannel) << "conflicting oldest navigation context" << targetPostId;
        return false;
    }
    if (reachedNewest && !acceptExactFirst(count - ids.size())) {
        qCWarning(lcTimelineChannel) << "conflicting newest navigation context" << targetPostId;
        return false;
    }

    for (int offset = 0; offset < ids.size(); ++offset) {
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
        for (int offset = 0; offset < ids.size(); ++offset) {
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
        first = findFreeWindowFirst(next, ids.size(), preferredFirst);
        if (first < 0) {
            qCWarning(lcTimelineChannel) << "no free provisional context span"
                                         << targetPostId << preferredFirst << ids.size();
            return false;
        }
    }

    for (int offset = 0; offset < ids.size(); ++offset) {
        next[first + offset] = ids.at(offset);
    }

    postIds = std::move(next);
    rebuildIndex();

    if (exactPlacement) {
        provisionalPostIds.clear();
        provisionalWindow.clear();
    } else {
        provisionalPostIds = QSet<QString>(ids.cbegin(), ids.cend());
        provisionalWindow.targetPostId = targetPostId;
        provisionalWindow.postIds = ids;
        provisionalWindow.first = first;
        provisionalWindow.reachedOldest = reachedOldest;
        provisionalWindow.reachedNewest = reachedNewest;
    }

    const int newLast = first + ids.size() - 1;
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
                    || state->before.postIds.size() < ServerPageSize));
        const bool reachedNewest = state->base.reachedNewest
            || (state->needAfter && state->after.success
                && (state->after.nextPostId.isEmpty()
                    || state->after.postIds.size() < ServerPageSize));

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
            },
            true);
    }
    if (needAfter) {
        PostTimelineService::instance(backend).loadChannelAfter(
            channel, provisionalWindow.postIds.last(), ServerPageSize,
            [state, finishPart](const PostTimelineService::Page& result) {
                state->after = result;
                finishPart();
            },
            true);
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

    bool touchesProvisionalIdentity = false;
    for (int offset = 0; offset < count; ++offset) {
        const QString& id = chronologicalIds.at(offset);
        if (provisionalPostIds.contains(id)) {
            touchesProvisionalIdentity = true;
        }
        const int existing = postIndexes.value(id, -1);
        if (existing >= 0 && (existing < first || existing > last)) {
            postIds[existing].clear();
        }
    }
    for (int offset = 0; offset < count; ++offset) {
        postIds[first + offset] = chronologicalIds.at(offset);
        provisionalPostIds.remove(chronologicalIds.at(offset));
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
