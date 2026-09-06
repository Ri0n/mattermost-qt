#pragma once

#include <QHash>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "AbstractPostSource.h"

namespace Mattermost {

class BackendChannel;

/**
 * Shared logical-index bookkeeping for post sources.
 *
 * This class deliberately knows nothing about Mattermost transport pagination,
 * channel counters, thread roots/cursors, viewport geometry or widget lifetime.
 * Concrete sources decide where a server result belongs; this base only applies
 * identity mappings and publishes the corresponding source-level signals.
 */
class IndexedPostSource : public AbstractPostSource
{
    Q_OBJECT
public:
    explicit IndexedPostSource(BackendChannel& channel, QObject* parent = nullptr);

    int itemCount() const override { return static_cast<int>(postIds.size()); }
    bool isAvailable(int index) const override;
    BackendPost* postAt(int index) const override;
    int indexOfPost(const QString& postId) const override;

protected:
    struct ExactWindowMutation {
        int first = -1;
        int last = -1;
        bool mappingChanged = false;
        QSet<int> concreteChanged;

        bool isValid() const { return first >= 0 && last >= first; }
    };

    /**
     * Apply identities to an exact logical window and relocate duplicate
     * occurrences of those identities from other slots. No signals are emitted.
     */
    ExactWindowMutation assignExactWindow(int first, const QStringList& ids);

    /** Publish the signal set implied by assignExactWindow(). */
    void publishExactWindow(const ExactWindowMutation& mutation);

    /** Resize only the newest/tail side of the logical sequence. */
    bool resizeLogicalTail(int count);

    /** Insert unavailable logical slots without inventing post identity. */
    void insertEmptyLogicalSlots(int first, int count);

    /** Remove logical slots and shift later identities left. */
    void eraseLogicalSlots(int first, int count);

    void rebuildIndex();

    BackendChannel& channel;
    QVector<QString> postIds;
    QHash<QString, int> postIndexes;
};

} // namespace Mattermost
