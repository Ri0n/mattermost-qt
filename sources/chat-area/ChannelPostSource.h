#pragma once

#include <functional>
#include <vector>

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "AbstractPostSource.h"

namespace Mattermost {

class Backend;
class BackendChannel;

/** Main-channel root posts mapped onto oldest->newest logical indices. */
class ChannelPostSource : public AbstractPostSource
{
    Q_OBJECT
public:
    explicit ChannelPostSource(Backend& backend,
                               BackendChannel& channel,
                               QObject* parent = nullptr);

    int itemCount() const override { return static_cast<int>(postIds.size()); }
    bool isAvailable(int index) const override;
    BackendPost* postAt(int index) const override;
    int indexOfPost(const QString& postId) const override;
    int ensurePostIndex(const QString& postId) override;

    /**
     * Atomically adopt a server-provided chronological context around a semantic
     * target. The context is placed exactly when it intersects an authoritative
     * row (or a confirmed channel boundary); otherwise its target gets a
     * timestamp-based estimated absolute position and the whole context remains
     * provisional until an absolute page later reconciles it by identity.
     */
    bool adoptNavigationContext(const QString& targetPostId,
                                const QStringList& chronologicalIds,
                                bool reachedOldest,
                                bool reachedNewest);

    void requestRange(int first,
                      int last,
                      RequestReason reason,
                      quint64 generation) override;

    bool canRequestBeforeFirst() const override;
    void requestBeforeFirst(RequestReason reason, quint64 generation) override;

private:
    // Visible/prefetch channel ranges use Mattermost's ten-post absolute pages.
    // Distant boundary discovery probes candidate page starts with per_page=1;
    // once at most two candidate pages remain, normal ten-post requests double
    // as both boundary evidence and useful viewport/prefetch materialization.
    static constexpr int ServerPageSize = 10;
    // Large-channel top-edge search starts this far inside the estimated count.
    // This is a latency heuristic only; inward/outward boundary search keeps
    // correctness independent of whether the estimate is high or low.
    static constexpr int InitialBoundaryProbePercent = 3;

    struct ProvisionalWindow {
        QString targetPostId;
        QStringList postIds;
        int first = -1;
        bool reachedOldest = false;
        bool reachedNewest = false;

        bool isValid() const { return first >= 0 && !postIds.isEmpty(); }
        int last() const { return first + static_cast<int>(postIds.size()) - 1; }
        void clear()
        {
            targetPostId.clear();
            postIds.clear();
            first = -1;
            reachedOldest = false;
            reachedNewest = false;
        }
    };

    int currentLogicalCount() const;
    int pageForIndex(int index) const;
    int initialBoundaryProbePages() const;
    int estimateIndexForPost(const BackendPost& post) const;
    int findFreeWindowFirst(const QVector<QString>& ids,
                            int windowSize,
                            int preferredFirst) const;
    bool isAuthoritativePost(const QString& postId) const;
    bool placeNavigationContext(const QString& targetPostId,
                                const QStringList& chronologicalIds,
                                bool reachedOldest,
                                bool reachedNewest,
                                int exactFirstHint = -1);
    void seedCachedPosts();
    void seedUnknownNewestPost();
    void rebuildIndex();
    void removeLogicalRange(int first, int count);
    void placePage(int page, const QStringList& chronologicalIds);
    void probeEstimatedOldestBoundary(std::function<void()> completion);
    void resolveOldestBoundary(int emptyPage, std::function<void()> completion);
    void resolveOldestBoundaryFromNonEmpty(int nonEmptyPage,
                                           std::function<void()> completion);
    void probeOldestBoundary();
    void loadOldestBoundaryPage(int page);
    void finishOldestBoundaryProbe();
    void reconcileRootCount(int actualCount);
    void ensureMinimumRootCount(int minimumCount);
    void insertLogicalPrefix(int count);
    void prependDiscovered(const QStringList& chronologicalIds);
    void appendLivePost(BackendPost& post);

    Backend& backend;
    BackendChannel& channel;
    QVector<QString> postIds;
    QHash<QString, int> postIndexes;

    const bool hasRootCountEstimate;
    // total_msg_count_root is a message-count estimate, not /posts row count.
    // Deleted roots can make it too large; count-excluded system roots can
    // make it too small. Once /posts proves the exact oldest boundary, keep
    // the signed difference so later metadata refreshes preserve that mapping.
    int rootCountAdjustment = 0;
    bool moreBeforeFirst = false;
    bool beforeRequestInFlight = false;

    // Shared reconciliation state lets concurrent top-edge/range requests wait
    // on one absolute-page boundary search instead of starting competing probes.
    bool oldestBoundaryFastPathTried = false;
    bool oldestBoundaryProbeInFlight = false;
    int oldestBoundaryNonEmptyPage = -1;
    int oldestBoundaryEmptyPage = -1;
    int oldestBoundaryProbeStep = 1;
    int oldestBoundarySearchLimitPage = -1;
    int oldestBoundaryMaterializedFullPage = -1;
    std::vector<std::function<void()>> oldestBoundaryWaiters;

    ProvisionalWindow provisionalWindow;
    QSet<QString> provisionalPostIds;
};

} // namespace Mattermost