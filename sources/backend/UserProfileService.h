/**
 * @file UserProfileService.h
 * @brief Lazy, batched Mattermost user profile loading.
 */

#pragma once

#include <functional>

#include <QMap>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVector>

#include "backend/HTTPConnector.h"

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendTeam;
class BackendUser;

struct UserSearchOptions {
    QString term;
    QString teamId;
    QString notInTeamId;
    QString inChannelId;
    QString notInChannelId;
    int limit = 100;
    bool allowInactive = false;
};

class UserProfileService : public QObject {
    Q_OBJECT
public:
    static UserProfileService& instance(Backend& backend);

    void clear();

    void ensureUser(const QString& userId,
                    std::function<void(const BackendUser*)> callback = {});
    void ensureUsers(const QStringList& userIds,
                     std::function<void()> callback = {});
    void ensureAvatar(const BackendUser& user);
    void ensureStatuses(const QStringList& userIds,
                        std::function<void()> callback = {});
    void searchUsers(const UserSearchOptions& options,
                     std::function<void(QVector<const BackendUser*>)> callback);
    void ensureTeamMembers(BackendTeam& team,
                           std::function<void()> callback = {});
    void ensureChannelMembers(BackendChannel& channel,
                              std::function<void()> callback = {});

private:
    explicit UserProfileService(Backend& backend);

    void scheduleFlush();
    void flushProfiles();
    void finishProfile(const QString& userId, const BackendUser* user);
    void resolveReferences(BackendUser& user);
    void finishMemberProfiles(const QStringList& userIds,
                              std::function<void()> callback);

    Backend& backend;
    HTTPConnector httpConnector;

    QSet<QString> queuedUserIds;
    QSet<QString> inFlightUserIds;
    QSet<QString> inFlightAvatarKeys;
    QMap<QString, QVector<std::function<void(const BackendUser*)>>> waiters;
    bool flushScheduled = false;
};

} // namespace Mattermost
