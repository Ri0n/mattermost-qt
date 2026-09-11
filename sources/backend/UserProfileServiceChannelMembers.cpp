/**
 * @file UserProfileServiceChannelMembers.cpp
 * @brief Paged channel-member primitives for virtualized member lists.
 */

#include "UserProfileService.h"

#include <algorithm>

#include <QJsonDocument>
#include <QJsonObject>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

void UserProfileService::queryChannelMemberCount(
    BackendChannel& channel,
    std::function<void(int)> callback)
{
    if (channel.member_count >= 0) {
        if (callback) {
            callback(channel.member_count);
        }
        return;
    }

    NetworkRequest request(
        QStringLiteral("channels/") + channel.id + QStringLiteral("/stats"));
    httpConnector.get(request, HttpResponseCallback(
        [&channel, callback = std::move(callback)](const QJsonDocument& doc) mutable {
            channel.member_count = std::max(0,
                doc.object().value(QStringLiteral("member_count")).toInt());
            if (callback) {
                callback(channel.member_count);
            }
        }));
}

void UserProfileService::loadChannelMembersPage(
    BackendChannel& channel,
    int page,
    int perPage,
    std::function<void(QStringList)> callback)
{
    if (page < 0 || perPage <= 0) {
        if (callback) {
            callback({});
        }
        return;
    }

    NetworkRequest request(
        QStringLiteral("channels/") + channel.id
        + QStringLiteral("/members?page=") + QString::number(page)
        + QStringLiteral("&per_page=") + QString::number(perPage));

    httpConnector.get(request, HttpResponseCallback(
        [this, &channel, callback = std::move(callback)](
            const QJsonDocument& doc) mutable {
            QStringList userIds;
            userIds.reserve(doc.array().size());
            for (const auto& value : doc.array()) {
                const QJsonObject memberObject = value.toObject();
                const QString userId = memberObject.value(
                    QStringLiteral("user_id")).toString();
                if (userId.isEmpty()) {
                    continue;
                }
                channel.addMember(backend.getStorage(), memberObject);
                userIds.push_back(userId);
            }

            if (userIds.isEmpty()) {
                if (callback) {
                    callback({});
                }
                return;
            }

            ensureUsers(userIds,
                [this, userIds, callback = std::move(callback)]() mutable {
                    ensureStatuses(userIds,
                        [userIds, callback = std::move(callback)]() mutable {
                            if (callback) {
                                callback(userIds);
                            }
                        });
                });
        }));
}

} // namespace Mattermost
