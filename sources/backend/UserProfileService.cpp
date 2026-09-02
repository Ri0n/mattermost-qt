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
