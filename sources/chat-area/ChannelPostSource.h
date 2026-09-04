#pragma once

#include <limits>

#include <QHash>
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

    void requestRange(int first,
                      int last,
                      RequestReason reason,
                      quint64 generation) override;

    bool canRequestBeforeFirst() const override;
    void requestBeforeFirst(RequestReason reason, quint64 generation) override;

private:
    static constexpr int ServerPageSize = 10;

    int currentLogicalCount() const;
    int pageForIndex(int index) const;
    int estimateIndexForPost(const BackendPost& post) const;
    int nearestEmptyIndex(int preferred) const;
    void seedCachedPosts();
    void seedUnknownNewestPost();
    void rebuildIndex();
    void placePage(int page, const QStringList& chronologicalIds);
    void prependDiscovered(const QStringList& chronologicalIds);
    void appendLivePost(BackendPost& post);

    Backend& backend;
    BackendChannel& channel;
    QVector<QString> postIds;
    QHash<QString, int> postIndexes;

    const bool exactRootCount;
    bool moreBeforeFirst = false;
    bool beforeRequestInFlight = false;
};

} // namespace Mattermost
