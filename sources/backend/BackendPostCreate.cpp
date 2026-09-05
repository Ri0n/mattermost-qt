#include "Backend.h"

#include <QJsonArray>
#include <QJsonObject>

#include "NetworkRequest.h"
#include "QByteArrayCreator.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

void Backend::addPost(BackendChannel& channel,
                      const QString& message,
                      const QList<QString>& attachments,
                      const QString& rootID,
                      const QJsonObject& props)
{
    if (props.isEmpty()) {
        addPost(channel, message, attachments, rootID);
        return;
    }

    QJsonArray files;
    for (const QString& id : attachments) {
        files.push_back(id);
    }

    QJsonObject json;
    json.insert(QStringLiteral("channel_id"), channel.id);
    json.insert(QStringLiteral("message"), message);
    json.insert(QStringLiteral("props"), props);

    if (!files.isEmpty()) {
        json.insert(QStringLiteral("file_ids"), files);
    }
    if (!rootID.isEmpty()) {
        json.insert(QStringLiteral("root_id"), rootID);
    }

    NetworkRequest request(QStringLiteral("posts"));
    httpConnector.post(request, json, HttpResponseCallback([](QVariant, QByteArray) {
        // The authoritative post is delivered through the normal websocket path.
    }));
}

} // namespace Mattermost
