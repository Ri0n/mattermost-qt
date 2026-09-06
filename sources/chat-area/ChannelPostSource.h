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
    // Channel history range loading always uses Mattermost's ten-post absolute
    // pages. Identity cursors are reserved for semantic/compatibility paths and
    // never define ordinary scroll or seek request boundaries.
    static constexpr int ServerPageSize = 10;

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
    void resolveOldestBoundary(int emptyPage, std::function<void()> completion);
    void probeOldestBoundary();
    void finishOldestBoundaryProbe();
    void reconcileRootCount(int actualCount, int page, int returnedCount);
    void prependDiscovered(const QStringList& chronologicalIds);
    void appendLivePost(BackendPost& post);

    Backend& backend;
    BackendChannel& channel;
    QVector<QString> postIds;
    QHash<QString, int> postIndexes;

    const bool exactRootCount;
    // total_msg_count_root can overestimate /posts because deleted roots are
    // omitted. Empty/short absolute pages resolve the real oldest boundary;
    // keep that correction local so later channel metadata cannot recreate the
    // phantom logical prefix.
    int rootCountOverestimate = 0;
    bool moreBeforeFirst = false;
    bool beforeRequestInFlight = false;

    // Shared reconciliation state lets concurrent empty range requests wait on
    // one absolute-page search instead of starting competing boundary probes.
    bool oldestBoundaryProbeInFlight = false;
    int oldestBoundaryNonEmptyPage = -1;
    int oldestBoundaryEmptyPage = -1;
    int oldestBoundaryProbeStep = 1;
    QStringList oldestBoundaryNonEmptyIds;
    std::vector<std::function<void()>> oldestBoundaryWaiters;

    ProvisionalWindow provisionalWindow;
    QSet<QString> provisionalPostIds;
};

} // namespace Mattermost
