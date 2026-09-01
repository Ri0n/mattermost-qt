/**
 * @file ChannelQuickList.h
 * @brief Flat channel index used by the Recent and Unreads sidebar tabs.
 */

#pragma once

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
        Unreads,
    };

    explicit ChannelQuickList(QWidget* parent = nullptr);

    void initialize(Backend& backend, Mode mode);
    void refresh();

signals:
    void channelSelected(const QString& channelId);

private:
    void updateDirectUser(const BackendUser& user);
    void ensureDirectUserConnections(BackendChannel& channel);

    Backend* backend = nullptr;
    Mode mode = Recent;
    bool refreshing = false;
    QMap<QString, QTreeWidgetItem*> channelItems;
    QSet<QString> connectedUsers;
};

} // namespace Mattermost
