#include "PostCreateService.h"

#include <QHash>
#include <QJsonArray>
#include <QPointer>

#include "Backend.h"
#include "NetworkRequest.h"
#include "backend/types/BackendChannel.h"

namespace Mattermost {

PostCreateService& PostCreateService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<PostCreateService>> instances;
    QPointer<PostCreateService>& service = instances[&backend];
    if (!service) {
        service = new PostCreateService(backend);
    }
    return *service;
}

PostCreateService::PostCreateService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
    // Keep errors from the metadata-capable path consistent with Backend's
    // existing post creation path.
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);
}

void PostCreateService::createPost(BackendChannel& channel,
                                   const QString& message,
                                   const QList<QString>& attachments,
                                   const QString& rootId,
                                   const QJsonObject& props)
{
    // Avoid replacing the long-standing Backend path for ordinary posts.
    if (props.isEmpty()) {
        backend.addPost(channel, message, attachments, rootId);
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
    if (!rootId.isEmpty()) {
        json.insert(QStringLiteral("root_id"), rootId);
    }

    NetworkRequest request(QStringLiteral("posts"));
    httpConnector.post(request, json, HttpResponseCallback([](QVariant, QByteArray) {
        // The authoritative post is delivered through the normal websocket path.
    }));
}

} // namespace Mattermost
