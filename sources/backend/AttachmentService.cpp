#include "AttachmentService.h"

#include <utility>

#include <QNetworkReply>
#include <QTimer>

#include "Backend.h"
#include "HttpResponseCallback.h"
#include "NetworkRequest.h"

namespace Mattermost {

AttachmentService& AttachmentService::instance(Backend& backend)
{
    static QHash<Backend*, QPointer<AttachmentService>> instances;
    QPointer<AttachmentService>& service = instances[&backend];
    if (!service) {
        service = new AttachmentService(backend);
    }
    return *service;
}

AttachmentService::AttachmentService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);
}

void AttachmentService::retrieveFile(const QString& fileId, Callback callback)
{
    if (fileId.isEmpty()) {
        QTimer::singleShot(0, this, [callback = std::move(callback)] {
            if (callback) {
                callback(QByteArray());
            }
        });
        return;
    }

    auto pending = pendingCallbacks.find(fileId);
    if (pending != pendingCallbacks.end()) {
        pending->push_back(std::move(callback));
        return;
    }

    pendingCallbacks.insert(fileId, QVector<Callback> {std::move(callback)});

    NetworkRequest request(QStringLiteral("files/") + fileId, true);
    request.setPriority(QNetworkRequest::LowPriority);
    request.setAttribute(QNetworkRequest::BackgroundRequestAttribute, true);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                         QNetworkRequest::PreferCache);

    httpConnector.get(request, HttpResponseCallback(
        [this, fileId](QVariant, QByteArray data) {
            const QVector<Callback> callbacks = pendingCallbacks.take(fileId);
            for (const Callback& current : callbacks) {
                if (current) {
                    current(data);
                }
            }
        }));
}

} // namespace Mattermost
