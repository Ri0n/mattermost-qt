#pragma once

#include <cstdint>

#include <QSet>
#include <QString>

#include "IndexedPostSource.h"
#include "backend/PostResidencyLease.h"

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

    /** Whether this post already has a server-confirmed logical thread index. */
    bool isPostPositionAuthoritative(const QString& postId) const
    {
        const int index = indexOfPost(postId);
        return index >= 0 && !provisionalPostIds.contains(postId);
    }

private:
    static constexpr int ServerBlockSize = 10;

    BackendPost* rootPost() const;
    int currentLogicalCount() const;
    int nearestEmptyIndex(int preferred) const;
    void seedCachedPosts();
    void hydrateCachedTail();
    void validateCachedTail();
    bool isAuthoritativeIndex(int index) const;
    bool isCursorReadyIndex(int index) const;
    void pruneProvisionalPostIds();
    void placeExactWindow(int first, const QStringList& ids);
    void placeInitial(const QStringList& ids);
    void placeTail(const QStringList& ids);
    void placeApproximate(int targetIndex, const QStringList& ids);
    uint64_t estimatedCreateAt(int logicalIndex) const;
    int estimatedIndexForPost(const BackendPost& post) const;
    void appendLiveReply(BackendPost& post);

    Backend& backend;
    QString rootId;
    PostResidencyLease rootResidencyLease;
    QSet<QString> provisionalPostIds;
    // A semantic jump may temporarily place one cached reply by timestamp.
    // Exact thread windows may move this identity when they contain it, but may
    // not silently overwrite its estimated slot with an unrelated reply.
    QString navigationProvisionalPostId;
};

} // namespace Mattermost
