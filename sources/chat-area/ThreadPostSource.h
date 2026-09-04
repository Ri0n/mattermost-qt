#pragma once

#include <QHash>
#include <QVector>

#include "AbstractPostSource.h"

namespace Mattermost {

class Backend;
class BackendChannel;

/** Thread root + replies mapped onto stable oldest->newest logical indices. */
class ThreadPostSource : public AbstractPostSource
{
    Q_OBJECT
public:
    explicit ThreadPostSource(Backend& backend,
                              BackendChannel& channel,
                              QString rootId,
                              QObject* parent = nullptr);

    int itemCount() const override { return postIds.size(); }
    bool isAvailable(int index) const override;
    BackendPost* postAt(int index) const override;
    int indexOfPost(const QString& postId) const override;

    void requestRange(int first,
                      int last,
                      RequestReason reason,
                      quint64 generation) override;

    const QString& rootPostId() const { return rootId; }

private:
    static constexpr int ServerBlockSize = 10;

    BackendPost* rootPost() const;
    int currentLogicalCount() const;
    void seedCachedPosts();
    void rebuildIndex();
    void placeInitial(const QStringList& ids);
    void placeTail(const QStringList& ids);
    void placeApproximate(int targetIndex, const QStringList& ids);
    uint64_t estimatedCreateAt(int logicalIndex) const;
    void appendLiveReply(BackendPost& post);

    Backend& backend;
    BackendChannel& channel;
    QString rootId;
    QVector<QString> postIds;
    QHash<QString, int> postIndexes;
};

} // namespace Mattermost
