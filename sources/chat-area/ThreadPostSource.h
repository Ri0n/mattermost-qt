#pragma once

#include <QHash>
#include <QVector>

#include "IndexedPostSource.h"

namespace Mattermost {

class Backend;
class BackendChannel;

/** Thread root + replies mapped onto stable oldest->newest logical indices. */
class ThreadPostSource : public IndexedPostSource
{
    Q_OBJECT
public:
    explicit ThreadPostSource(Backend& backend,
                              BackendChannel& channel,
                              QString rootId,
                              QObject* parent = nullptr);

    int ensurePostIndex(const QString& postId) override;

    void requestRange(int first,
                      int last,
                      RequestReason reason,
                      quint64 generation) override;

    const QString& rootPostId() const { return rootId; }

private:
    static constexpr int ServerBlockSize = 10;

    BackendPost* rootPost() const;
    int currentLogicalCount() const;
    int nearestEmptyIndex(int preferred) const;
    void seedCachedPosts();
    void placeExactWindow(int first, const QStringList& ids);
    void placeInitial(const QStringList& ids);
    void placeTail(const QStringList& ids);
    void placeApproximate(int targetIndex, const QStringList& ids);
    uint64_t estimatedCreateAt(int logicalIndex) const;
    int estimatedIndexForPost(const BackendPost& post) const;
    void appendLiveReply(BackendPost& post);

    Backend& backend;
    QString rootId;
};

} // namespace Mattermost
