/**
 * @file ChannelQuickList.h
 * @brief Flat channel index used by the Recent sidebar tab.
 */

#pragma once

#include <cstdint>

#include <QMap>
#include <QSet>
#include <QTreeWidget>

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendUser;

class ChannelQuickList : public QTreeWidget
{
    Q_OBJECT
public:
    enum Mode {
        Recent,
    };

    explicit ChannelQuickList(QWidget* parent = nullptr);

    void initialize(Backend& backend, Mode mode = Recent);
    void refresh();

signals:
    void channelSelected(const QString& channelId);
    void channelContextMenuRequested(const QString& channelId, const QPoint& globalPos);

private:
    struct RecentThreadTarget {
        QString rootPostId;
        QString postId;
        uint64_t interactionAt = 0;
    };

    void updateDirectUser(const BackendUser& user);
    void ensureDirectUserConnections(BackendChannel& channel);

    Backend* backend = nullptr;
    bool refreshing = false;
    QMap<QString, QTreeWidgetItem*> channelItems;
    QMap<QString, RecentThreadTarget> recentThreadTargets;
    QSet<QString> connectedUsers;
};

} // namespace Mattermost
