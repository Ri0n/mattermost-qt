#pragma once

#include <cstdint>
#include <functional>

#include <QObject>
#include <QString>
#include <QTimer>
#include <QVector>

#include "ThreadFollowService.h"

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;

/**
 * Shared semantic data backing the Following and Attention sidebar projections.
 *
 * The model owns the loaded followed-thread snapshot, current unread DM/GM
 * entries, synthetic root mentions and each entry's local resume cursor. Views
 * own only presentation details such as selection retention.
 */
class FollowingModel final : public QObject
{
    Q_OBJECT
public:
    enum class Kind {
        Conversation,
        Thread,
    };

    enum class ResumeState {
        Unknown,
        FirstUnread,
        AtEnd,
    };

    struct Entry {
        Kind kind = Kind::Conversation;
        QString channelId;
        QString threadId;
        QString teamId;
        QString authorId;
        QString message;

        uint64_t lastViewedAt = 0;
        uint64_t lastReplyAt = 0;
        uint64_t attentionSince = 0;

        int unreadReplies = 0;
        int unreadMentions = 0;
        bool unread = false;
        bool mentioned = false;
        bool urgent = false;
        bool synthetic = false;
        bool muted = false;

        ResumeState resumeState = ResumeState::Unknown;
        QString firstUnreadPostId;
        QString readThroughPostId;
        uint64_t readThroughCreateAt = 0;

        // Suppress a CRT response that predates an already-issued local read.
        // New replies after this watermark are never hidden by that suppression.
        bool readAcknowledgementPending = false;
        uint64_t readAcknowledgementAt = 0;

        bool isThread() const { return kind == Kind::Thread; }
        bool requiresAttention() const
        {
            return unread || unreadReplies > 0 || unreadMentions > 0;
        }
        bool hasLocalProgress() const { return !readThroughPostId.isEmpty(); }
    };

    static FollowingModel& instance(Backend& backend);

    const QVector<Entry>& entries() const { return entries_; }
    const Entry* findEntry(const QString& channelId,
                           const QString& threadId = QString()) const;

    /** Refresh DM/GM metadata from SidebarService and notify both projections. */
    void refresh();

    /** Request the shared followed-thread snapshot if it is known to be stale. */
    void ensureThreadsFresh();

    /** Force one followed-thread reconciliation, coalescing concurrent callers. */
    void refreshThreads();

    /**
     * Advance the resume high-water mark through a message whose lower edge was
     * actually visible. Random non-Following chats never allocate cursor state:
     * the target entry must already exist in this model.
     */
    void observeReadThrough(const QString& channelId,
                            const QString& threadId,
                            const BackendPost& post,
                            bool sourceAtEnd);

    /** Optimistically acknowledge a followed thread and reconcile with CRT. */
    void markThreadRead(const QString& teamId,
                        const QString& threadId,
                        std::function<void(bool)> callback = {});

    /** Consume a temporary root-mention entry after explicit navigation. */
    void consumeSyntheticThread(const QString& threadId);

signals:
    void changed();

private:
    explicit FollowingModel(Backend& backend);

    Entry* findEntryMutable(const QString& channelId, const QString& threadId);
    Entry* findThreadMutable(const QString& threadId);
    const Entry* findThread(const QString& threadId) const;

    void syncConversations();
    void noteIncomingPost(BackendChannel& channel, const BackendPost& post);
    void noteSyntheticMention(BackendChannel& channel, const BackendPost& post);
    void clearSyntheticMentions(const QString& channelId);
    void scheduleThreadRefresh();
    void applyThreadSnapshot(QVector<ThreadFollowService::ThreadSummary> threads);

    QString nextCachedPostId(const BackendChannel& channel,
                             const QString& threadId,
                             const BackendPost& after) const;
    static bool isAfter(uint64_t lhsCreateAt,
                        const QString& lhsId,
                        uint64_t rhsCreateAt,
                        const QString& rhsId);
    static void noteAttentionTransition(Entry& entry, bool wasAttention);
    static void noteIncomingForResume(Entry& entry, const BackendPost& post);

    Backend& backend_;
    QVector<Entry> entries_;
    QTimer threadRefreshTimer_;
    bool threadRefreshInFlight_ = false;
    bool threadRefreshRequested_ = false;
    bool threadSnapshotDirty_ = true;
};

} // namespace Mattermost
