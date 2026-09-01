/**
 * @file SidebarService.h
 * @brief Mattermost sidebar categories and per-channel notification state.
 */

#pragma once

#include <functional>

#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>

#include "backend/ChannelActivityTracker.h"
#include "backend/HTTPConnector.h"

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;
class BackendTeam;

struct SidebarCategory {
    QString id;
    QString userId;
    QString teamId;
    qint64 sortOrder = 0;
    QString sorting;
    QString type;
    QString displayName;
    bool muted = false;
    bool collapsed = false;
    QStringList channelIds;

    static SidebarCategory fromJson(const QJsonObject& object);
    QJsonObject toJson() const;

    bool isCustom() const { return type == QStringLiteral("custom"); }
};

struct SidebarTeamState {
    QMap<QString, SidebarCategory> categories;
    QStringList order;

    SidebarCategory* category(const QString& categoryId);
    const SidebarCategory* category(const QString& categoryId) const;
    SidebarCategory* categoryByType(const QString& type);
    const SidebarCategory* categoryByType(const QString& type) const;
};

class SidebarService : public QObject {
    Q_OBJECT
public:
    static SidebarService& instance(Backend& backend);

    void clear();

    bool isChannelMuted(const BackendChannel& channel) const;
    bool isChannelMuted(const QString& channelId) const;
    bool hasUnreadMention(const QString& channelId) const;
    void setChannelMentioned(const QString& channelId, bool mentioned);

    bool isChannelTracked(const QString& channelId) const;
    bool isChannelUnread(const BackendChannel& channel) const;
    uint64_t channelActivityTime(const BackendChannel& channel) const;
    uint64_t channelLastViewedTime(const BackendChannel& channel) const;
    void markChannelViewedLocally(const BackendChannel& channel);
    void synchronizeChannelActivity();

    void retrieveChannelMemberships(std::function<void()> callback = {});
    void setChannelMuted(BackendChannel& channel, bool muted,
                         std::function<void(bool)> callback = {});

    void retrieveCategories(BackendTeam& team,
                            std::function<void(const SidebarTeamState&)> callback = {});
    const SidebarTeamState* teamState(const QString& teamId) const;
    SidebarTeamState* teamState(const QString& teamId);

    void updateCategory(const SidebarCategory& category,
                        std::function<void(const SidebarCategory&)> callback = {});
    void updateCategories(const QString& teamId, const QVector<SidebarCategory>& categories,
                          std::function<void(const SidebarTeamState&)> callback = {});
    void updateCategoryOrder(const QString& teamId, const QStringList& order,
                             std::function<void()> callback = {});

signals:
    void channelMutedChanged(const QString& channelId, bool muted);
    void channelMentionedChanged(const QString& channelId, bool mentioned);
    void channelActivityChanged(const QString& channelId);
    void channelActivityReset();
    void categoriesChanged(const QString& teamId);

private:
    explicit SidebarService(Backend& backend);

    QString currentUserId() const;
    QString categoriesPath(const QString& teamId) const;
    void recordChannelPost(BackendChannel& channel, const BackendPost& post);
    void recordChannelViewed(const BackendChannel& channel);

    Backend& backend;
    HTTPConnector httpConnector;
    QSet<QString> mutedChannelIds;
    QSet<QString> mentionedChannelIds;
    QMap<QString, SidebarTeamState> sidebarByTeam;
    ChannelActivityTracker activityTracker;
};

} // namespace Mattermost
