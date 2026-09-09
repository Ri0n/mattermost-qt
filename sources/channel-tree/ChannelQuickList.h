/**
 * @file ChannelQuickList.h
 * @brief Followed conversation queue used by the Following sidebar tab.
 */

#pragma once

#include <cstdint>

#include <QMap>
#include <QSet>
#include <QTreeWidget>

#include "backend/FollowingModel.h"

class QKeyEvent;
class QMouseEvent;
class QShowEvent;

namespace Mattermost {

class Backend;
class BackendChannel;
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
    void mousePressEvent(QMouseEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void activateItem(QTreeWidgetItem* item);
    void openThread(const FollowingModel::Entry& entry);
    QString threadLabel(const FollowingModel::Entry& entry) const;
    void updateDirectUser(const BackendUser& user);
    void ensureDirectUserConnections(BackendChannel& channel);

    Backend* backend_ = nullptr;
    FollowingModel* model_ = nullptr;
    bool refreshing_ = false;
    QMap<QString, QTreeWidgetItem*> channelItems_;
    QSet<QString> connectedUsers_;

    QString retainedKey_;
    uint64_t retainedSortTime_ = 0;
    bool retainedUnreadPosition_ = false;
};

} // namespace Mattermost
