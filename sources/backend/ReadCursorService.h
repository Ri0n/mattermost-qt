#pragma once

#include <cstdint>

#include <QHash>
#include <QObject>
#include <QString>

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;

/**
 * Local high-water read cursor used by navigation surfaces such as Following.
 *
 * Server unread state remains authoritative for badges/Attention. This service
 * answers a different UI question: after content has actually crossed the
 * viewport's read boundary, where should the next explicit resume jump go?
 */
class ReadCursorService final : public QObject
{
    Q_OBJECT
public:
    enum class State {
        Unknown,
        FirstUnread,
        AtEnd,
    };

    struct Cursor {
        State state = State::Unknown;
        QString postId;
        QString readThroughPostId;
        uint64_t readThroughCreateAt = 0;

        bool hasLocalProgress() const
        {
            return !readThroughPostId.isEmpty();
        }
    };

    static ReadCursorService& instance(Backend& backend);

    Cursor cursor(const QString& channelId,
                  const QString& rootId = QString()) const;

    /**
     * Advance the local high-water mark through one post that was actually read.
     * A post is considered read by the caller only once its lower edge entered
     * the viewport. Back-scrolling can therefore never move this cursor back.
     */
    void observeReadThrough(const QString& channelId,
                            const QString& rootId,
                            const BackendPost& post,
                            bool sourceAtEnd);

signals:
    void cursorChanged(const QString& channelId, const QString& rootId);

private:
    struct Entry {
        Cursor cursor;
    };

    explicit ReadCursorService(Backend& backend);

    static QString key(const QString& channelId, const QString& rootId);
    static bool isAfter(uint64_t lhsCreateAt,
                        const QString& lhsId,
                        uint64_t rhsCreateAt,
                        const QString& rhsId);

    QString nextCachedPostId(const BackendChannel& channel,
                             const QString& rootId,
                             const BackendPost& after) const;
    void noteIncomingPost(BackendChannel& channel, const BackendPost& post);
    void noteIncomingForKey(const QString& channelId,
                            const QString& rootId,
                            const BackendPost& post);

    Backend& backend_;
    QHash<QString, Entry> entries_;
};

} // namespace Mattermost
