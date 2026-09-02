/**
 * @file UserProfileService.cpp
 * @brief Lazy, batched Mattermost user profile loading.
 */

#include "UserProfileService.h"

#include <algorithm>
#include <memory>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/QByteArrayCreator.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendTeamMember.h"
#include "backend/types/BackendUser.h"

namespace Mattermost {

namespace {

constexpr int MaxProfilesPerRequest = 100;
constexpr int MaxStatusesPerRequest = 200;
constexpr int MembersPerPage = 200;

QStringList uniqueNonEmptyIds(const QStringList& userIds)
{
    QSet<QString> unique;
    for (const QString& userId : userIds) {
        if (!userId.isEmpty()) {
            unique.insert(userId);
        }
    }
    return unique.values();
}

} // namespace

UserProfileService& UserProfileService::instance(Backend& backend)
{
    static QMap<Backend*, UserProfileService*> instances;
    auto it = instances.find(&backend);
    if (it == instances.end()) {
        it = instances.insert(&backend, new UserProfileService(backend));
    }
    return **it;
}

UserProfileService::UserProfileService(Backend& backend)
    : QObject(&backend)
    , backend(backend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);
}

void UserProfileService::clear()
{
    httpConnector.reset();
    queuedUserIds.clear();
    inFlightUserIds.clear();
    waiters.clear();
    flushScheduled = false;
}

void UserProfileService::ensureUser(const QString& userId,
                                    std::function<void(const BackendUser*)> callback)
{
    if (userId.isEmpty()) {
        if (callback) {
            callback(nullptr);
        }
        return;
    }

    if (BackendUser* user = backend.getStorage().getUserById(userId)) {
        if (callback) {
            callback(user);
        }
        return;
    }

    if (callback) {
        waiters[userId].push_back(std::move(callback));
    }

    if (!queuedUserIds.contains(userId) && !inFlightUserIds.contains(userId)) {
        queuedUserIds.insert(userId);
        scheduleFlush();
    }
}

void UserProfileService::ensureUsers(const QStringList& userIds,
                                     std::function<void()> callback)
{
    const QStringList ids = uniqueNonEmptyIds(userIds);
    if (ids.isEmpty()) {
        if (callback) {
            callback();
        }
        return;
    }

    const auto remaining = std::make_shared<int>(static_cast<int>(ids.size()));
    const auto completed = std::make_shared<bool>(false);

    for (const QString& userId : ids) {
        ensureUser(userId, [remaining, completed, callback](const BackendUser*) {
            --*remaining;
            if (*remaining == 0 && !*completed) {
                *completed = true;
                if (callback) {
                    callback();
                }
            }
        });
    }
}

void UserProfileService::ensureStatuses(const QStringList& userIds,
                                        std::function<void()> callback)
{
    QStringList ids;
    for (const QString& userId : uniqueNonEmptyIds(userIds)) {
        if (backend.getStorage().getUserById(userId)) {
            ids.push_back(userId);
        }
    }

    if (ids.isEmpty()) {
        if (callback) {
            callback();
        }
        return;
    }

    const auto pending = std::make_shared<QStringList>(std::move(ids));
    const auto fetchNext = std::make_shared<std::function<void()>>();

    *fetchNext = [this, callback, pending, fetchNext] {
        if (pending->isEmpty()) {
            if (callback) {
                callback();
            }
            return;
        }

        QJsonArray payload;
        QStringList batch;
        const int batchSize = std::min(
            MaxStatusesPerRequest, static_cast<int>(pending->size()));
        batch.reserve(batchSize);
        for (int i = 0; i < batchSize; ++i) {
            const QString userId = pending->takeFirst();
            batch.push_back(userId);
            payload.push_back(userId);
        }

        NetworkRequest request(QStringLiteral("users/status/ids"));
        httpConnector.post(request, QByteArrayCreator(payload),
                           HttpResponseCallback([this, fetchNext](const QJsonDocument& doc) {
            for (const auto& value : doc.array()) {
                const QJsonObject object = value.toObject();
                BackendUser* user = backend.getStorage().getUserById(
                    object.value(QStringLiteral("user_id")).toString());
                if (!user) {
                    continue;
                }

                const QString status = object.value(QStringLiteral("status")).toString();
                const uint64_t lastActivity = object.value(QStringLiteral("last_activity_at"))
                    .toVariant().toULongLong();
                const bool statusChanged = user->status != status;
                user->status = status;
                user->lastActivity = lastActivity;
                if (statusChanged) {
                    emit user->onStatusChanged();
                }
            }
            (*fetchNext)();
        }));
    };

    (*fetchNext)();
}

void UserProfileService::searchUsers(
    const UserSearchOptions& options,
    std::function<void(QVector<const BackendUser*>)> callback)
{
    QJsonObject payload {
        {QStringLiteral("term"), options.term},
        {QStringLiteral("allow_inactive"), options.allowInactive},
        {QStringLiteral("limit"), std::max(1, options.limit)},
    };

    if (!options.teamId.isEmpty()) {
        payload.insert(QStringLiteral("team_id"), options.teamId);
    }
    if (!options.notInTeamId.isEmpty()) {
        payload.insert(QStringLiteral("not_in_team_id"), options.notInTeamId);
    }
    if (!options.inChannelId.isEmpty()) {
        payload.insert(QStringLiteral("in_channel_id"), options.inChannelId);
    }
    if (!options.notInChannelId.isEmpty()) {
        payload.insert(QStringLiteral("not_in_channel_id"), options.notInChannelId);
    }

    NetworkRequest request(QStringLiteral("users/search"));
    httpConnector.post(request, QByteArrayCreator(payload),
                       HttpResponseCallback([this, callback](const QJsonDocument& doc) {
        QVector<const BackendUser*> users;
        users.reserve(doc.array().size());

        for (const auto& value : doc.array()) {
            BackendUser* user = backend.getStorage().addUser(value.toObject());
            if (!user) {
                continue;
            }
            resolveReferences(*user);
            users.push_back(user);
        }

        if (callback) {
            callback(std::move(users));
        }
    }));
}

void UserProfileService::ensureTeamMembers(BackendTeam& team,
                                           std::function<void()> callback)
{
    const auto userIds = std::make_shared<QStringList>();
    const auto fetchPage = std::make_shared<std::function<void(int)>>();

    *fetchPage = [this, &team, callback, userIds, fetchPage](int page) {
        NetworkRequest request(
            QStringLiteral("teams/") + team.id + QStringLiteral("/members?page=")
            + QString::number(page) + QStringLiteral("&per_page=")
            + QString::number(MembersPerPage));

        httpConnector.get(request, HttpResponseCallback(
            [this, &team, callback, userIds, fetchPage, page](const QJsonDocument& doc) {
                const QJsonArray members = doc.array();
                for (const auto& value : members) {
                    const QJsonObject object = value.toObject();
                    team.addMember(backend.getStorage(), object);
                    userIds->push_back(object.value(QStringLiteral("user_id")).toString());
                }

                if (members.size() == MembersPerPage) {
                    (*fetchPage)(page + 1);
                    return;
                }

                finishMemberProfiles(*userIds, callback);
            }));
    };

    (*fetchPage)(0);
}

void UserProfileService::ensureChannelMembers(BackendChannel& channel,
                                              std::function<void()> callback)
{
    const auto userIds = std::make_shared<QStringList>();
    const auto fetchPage = std::make_shared<std::function<void(int)>>();

    *fetchPage = [this, &channel, callback, userIds, fetchPage](int page) {
        NetworkRequest request(
            QStringLiteral("channels/") + channel.id + QStringLiteral("/members?page=")
            + QString::number(page) + QStringLiteral("&per_page=")
            + QString::number(MembersPerPage));

        httpConnector.get(request, HttpResponseCallback(
            [this, &channel, callback, userIds, fetchPage, page](const QJsonDocument& doc) {
                const QJsonArray members = doc.array();
                for (const auto& value : members) {
                    const QJsonObject object = value.toObject();
                    channel.addMember(backend.getStorage(), object);
                    userIds->push_back(object.value(QStringLiteral("user_id")).toString());
                }

                if (members.size() == MembersPerPage) {
                    (*fetchPage)(page + 1);
                    return;
                }

                finishMemberProfiles(*userIds, callback);
            }));
    };

    (*fetchPage)(0);
}

void UserProfileService::finishMemberProfiles(const QStringList& userIds,
                                              std::function<void()> callback)
{
    const QStringList ids = uniqueNonEmptyIds(userIds);
    ensureUsers(ids, [this, ids, callback] {
        ensureStatuses(ids, callback);
    });
}

void UserProfileService::scheduleFlush()
{
    if (flushScheduled) {
        return;
    }

    flushScheduled = true;
    QTimer::singleShot(0, this, [this] {
        flushScheduled = false;
        flushProfiles();
    });
}

void UserProfileService::flushProfiles()
{
    if (queuedUserIds.isEmpty()) {
        return;
    }

    QStringList batch;
    batch.reserve(std::min(
        MaxProfilesPerRequest, static_cast<int>(queuedUserIds.size())));
    auto it = queuedUserIds.begin();
    while (it != queuedUserIds.end() && static_cast<int>(batch.size()) < MaxProfilesPerRequest) {
        const QString userId = *it;
        it = queuedUserIds.erase(it);
        inFlightUserIds.insert(userId);
        batch.push_back(userId);
    }

    QJsonArray payload;
    for (const QString& userId : batch) {
        payload.push_back(userId);
    }

    NetworkRequest request(QStringLiteral("users/ids"));
    httpConnector.post(request, QByteArrayCreator(payload),
                       HttpResponseCallback([this, batch](const QJsonDocument& doc) {
        QSet<QString> returnedIds;
        for (const auto& value : doc.array()) {
            BackendUser* user = backend.getStorage().addUser(value.toObject());
            if (!user) {
                continue;
            }
            returnedIds.insert(user->id);
            resolveReferences(*user);
            finishProfile(user->id, user);
        }

        for (const QString& userId : batch) {
            if (!returnedIds.contains(userId)) {
                finishProfile(userId, nullptr);
            }
        }

        if (!queuedUserIds.isEmpty()) {
            scheduleFlush();
        }
    }));
}

void UserProfileService::finishProfile(const QString& userId, const BackendUser* user)
{
    inFlightUserIds.remove(userId);
    const auto callbacks = waiters.take(userId);
    for (const auto& callback : callbacks) {
        if (callback) {
            callback(user);
        }
    }
}

void UserProfileService::resolveReferences(BackendUser& user)
{
    Storage& storage = backend.getStorage();

    if (BackendChannel* directChannel = storage.getDirectChannelByUserId(user.id)) {
        directChannel->display_name = user.getDisplayName();
        if (!storage.directChannels.members.contains(&user)) {
            storage.directChannels.members.push_back(&user);
        }
        emit directChannel->onUpdated();
    }

    for (auto teamIt = storage.teams.begin(); teamIt != storage.teams.end(); ++teamIt) {
        auto memberIt = teamIt->second.members.find(user.id);
        if (memberIt != teamIt->second.members.end()) {
            memberIt->user = &user;
        }
    }

    for (auto channelIt = storage.channels.begin(); channelIt != storage.channels.end(); ++channelIt) {
        BackendChannel* channel = channelIt.value();
        if (!channel) {
            continue;
        }

        auto memberIt = channel->members.find(user.id);
        if (memberIt != channel->members.end()) {
            memberIt->user = &user;
        }

        for (BackendPost& post : channel->posts) {
            if (!post.author && post.user_id == user.id) {
                post.author = &user;
            }
        }
        for (BackendPost& post : channel->pinnedPosts) {
            if (!post.author && post.user_id == user.id) {
                post.author = &user;
            }
        }
    }
}

} // namespace Mattermost
