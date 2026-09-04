#pragma once

#include <QObject>
#include <QString>

namespace Mattermost {

class BackendPost;

/**
 * Logical post sequence consumed by ChatLogWidget.
 *
 * A source owns post identity/range retrieval only. It never manipulates the
 * view, scrollbar, widget geometry or materialization.
 */
class AbstractPostSource : public QObject
{
    Q_OBJECT
public:
    enum class RequestReason {
        Initial,
        Scroll,
        Seek,
        EnsureVisible,
    };
    Q_ENUM(RequestReason)

    explicit AbstractPostSource(QObject* parent = nullptr)
        : QObject(parent)
    {
    }

    ~AbstractPostSource() override = default;

    virtual int itemCount() const = 0;
    virtual bool isAvailable(int index) const = 0;
    virtual BackendPost* postAt(int index) const = 0;
    virtual int indexOfPost(const QString& postId) const = 0;

    /**
     * Return a logical index for an already cached semantic target. Sources may
     * temporarily place a target into an estimated empty slot when the server
     * has not yet supplied an authoritative page boundary. Later page loads are
     * allowed to replace that estimate by identity.
     */
    virtual int ensurePostIndex(const QString& postId)
    {
        return indexOfPost(postId);
    }

    virtual void requestRange(int first,
                              int last,
                              RequestReason reason,
                              quint64 generation) = 0;

signals:
    void itemCountChanged(int count);
    void rangeAvailable(int first, int last);
    void itemsChanged(int first, int last);

    /** Every requestRange() call must eventually emit this exact requested range. */
    void rangeRequestFinished(int first, int last);
};

} // namespace Mattermost
