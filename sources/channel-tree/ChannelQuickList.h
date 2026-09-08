/**
 * @file ChannelQuickList.h
 * @brief Followed conversation queue used by the Following sidebar tab.
 */

#pragma once

#include <cstdint>

#include <QHash>
#include <QMap>
#include <QSet>
#include <QTimer>
#include <QTreeWidget>
#include <QVector>

#include "backend/ThreadFollowService.h"

class QKeyEvent;
class QShowEvent;

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;
class BackendUser;

class ChannelQuickList : public QTreeWidget
{
    Q_OBJECT
public:
    enum Mode {
        Following,
    };

    explicit ChannelQuickList(QWidget* parent = nullptr);

    void initialize(Backend& backend, Mode mode = Following);
    void refresh();
    void refreshThreads();
    void releaseSelectionRetention();

signals:
    void channelSelected(const QString& channelId);
    void channelContextMenuRequested(const QString& channelId, const QPoint& globalPos);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    using ThreadSummary = ThreadFollowService::ThreadSummary;

    void activateItem(QTreeWidgetItem* item);
    void notePost(BackendChannel& channel, const BackendPost& post);
    void clearSyntheticMentions(const QString& channelId);
    void scheduleThreadRefresh();
    void openThread(const ThreadSummary& thread);
    QString threadLabel(const ThreadSummary& thread) const;
    void updateDirectUser(const BackendUser& user);
    void ensureDirectUserConnections(BackendChannel& channel);

    Backend* backend = nullptr;
    bool refreshing = false;
    bool threadRefreshInFlight = false;
    bool threadRefreshRequested = false;
    bool _threadSnapshotDirty = true;
    QTimer threadRefreshTimer;
    QVector<ThreadSummary> serverThreads;
    QHash<QString, ThreadSummary> syntheticMentions;
    QHash<QString, uint64_t> pendingSince;
    QMap<QString, QTreeWidgetItem*> channelItems;
    QSet<QString> connectedUsers;

    QString retainedKey;
    uint64_t retainedSortTime = 0;
    bool retainedUnreadPosition = false;
};

} // namespace Mattermost
