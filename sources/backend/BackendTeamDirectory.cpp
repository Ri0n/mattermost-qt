#include "Backend.h"
#include "NetworkRequest.h"
#include "QByteArrayCreator.h"

#include <utility>

#include <QJsonDocument>
#include <QJsonObject>

namespace Mattermost {

void Backend::retrieveJoinableTeams(std::function<void(QJsonArray)> callback)
{
    NetworkRequest request(QStringLiteral("teams?per_page=200&page=0"));
    httpConnector.get(request, HttpResponseCallback(
        [callback = std::move(callback)](const QJsonDocument& doc) mutable {
            if (callback) {
                callback(doc.array());
            }
        }));
}

void Backend::joinTeam(const QString& teamId)
{
    if (teamId.isEmpty() || storage.getTeamById(teamId)) {
        return;
    }

    QJsonObject payload {
        {QStringLiteral("user_id"), getLoginUser().id},
        {QStringLiteral("team_id"), teamId},
    };
    NetworkRequest request(QStringLiteral("teams/") + teamId
                           + QStringLiteral("/members"));
    httpConnector.post(request, QByteArrayCreator(payload), HttpResponseCallback(
        [this, teamId](QVariant, QByteArray) {
            // The websocket user-added event normally resolves the new team.
            // Also retrieve it from the successful HTTP path so the sidebar is
            // not dependent on realtime delivery to expose a team we just joined.
            if (!storage.getTeamById(teamId)) {
                retrieveTeam(teamId);
            }
        }));
}

} // namespace Mattermost
