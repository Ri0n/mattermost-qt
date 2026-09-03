#include "AppNavigationService.h"

#include <QApplication>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/PostNavigationService.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "mainwindow.h"

namespace Mattermost {

AppNavigationService& AppNavigationService::instance(Backend& backend)
{
    static QMap<Backend*, AppNavigationService*> instances;
    auto it = instances.find(&backend);
    if (it == instances.end()) {
        it = instances.insert(&backend, new AppNavigationService(backend));
    }
    return **it;
}

AppNavigationService::AppNavigationService(Backend& backend)
    : QObject(&backend)
    , backend(backend)
{
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);

    // The service is first materialized by a PostWidget, i.e. after MainWindow
    // exists. Keep conversation routing centralized without making every post
    // hold another MainWindow connection.
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (auto* mainWindow = qobject_cast<MainWindow*>(widget)) {
            connect(this, &AppNavigationService::channelRequested,
                    mainWindow, &MainWindow::openChannelPost,
                    Qt::UniqueConnection);
        }
    }
}

bool AppNavigationService::isLocalUrl(const QUrl& url) const
{
    if (url.isRelative() || url.host().isEmpty()) {
        return true;
    }

    const QUrl serverUrl(NetworkRequest::host());
    if (!serverUrl.isValid() || serverUrl.host().isEmpty()) {
        return false;
    }

    return QString::compare(url.host(), serverUrl.host(), Qt::CaseInsensitive) == 0
        && url.port(-1) == serverUrl.port(-1);
}

BackendChannel* AppNavigationService::findChannel(const QString& teamName,
                                                   const QString& channelName) const
{
    for (BackendChannel* channel : backend.getStorage().channels) {
        if (!channel || channel->name != channelName) {
            continue;
        }
        if (teamName.isEmpty()
            || (channel->team && channel->team->name == teamName)) {
            return channel;
        }
    }
    return nullptr;
}

BackendChannel* AppNavigationService::findPostChannel(const QString& postId) const
{
    if (postId.isEmpty()) {
        return nullptr;
    }

    for (BackendChannel* channel : backend.getStorage().channels) {
        if (channel && channel->findPostById(postId)) {
            return channel;
        }
    }
    return nullptr;
}

void AppNavigationService::openUrl(const QUrl& url)
{
    if (!url.isValid()) {
        return;
    }

    if (!isLocalUrl(url)) {
        QDesktopServices::openUrl(url);
        return;
    }

    const QStringList path = url.path().split(QLatin1Char('/'), Qt::SkipEmptyParts);
    if (path.size() >= 3 && path.at(1) == QStringLiteral("channels")) {
        if (BackendChannel* channel = findChannel(path.at(0), path.at(2))) {
            emit channelRequested(channel->id, QString());
            return;
        }
    }

    if (path.size() >= 3 && path.at(1) == QStringLiteral("pl")) {
        openPost(path.at(2));
        return;
    }

    QDesktopServices::openUrl(url);
}

void AppNavigationService::openPost(const QString& postId)
{
    if (BackendChannel* channel = findPostChannel(postId)) {
        openPostInChannel(*channel, postId);
        return;
    }

    NetworkRequest request(QStringLiteral("posts/") + postId);
    QPointer<AppNavigationService> guard(this);
    httpConnector.get(request,
        [guard, postId](QVariant, QByteArray data, const QNetworkReply&) {
            if (!guard) {
                return;
            }

            const QJsonDocument document = QJsonDocument::fromJson(data);
            if (!document.isObject()) {
                return;
            }
            const QString channelId = document.object()
                .value(QStringLiteral("channel_id")).toString();
            BackendChannel* channel = guard->backend.getStorage().getChannelById(channelId);
            if (channel) {
                guard->openPostInChannel(*channel, postId);
            }
        });
}

void AppNavigationService::openPostInChannel(BackendChannel& channel,
                                             const QString& postId)
{
    QPointer<AppNavigationService> guard(this);
    const QString channelId = channel.id;
    PostNavigationService::instance(backend).loadAround(
        channel, postId,
        [guard, channelId, postId](bool success) {
            if (guard && success) {
                emit guard->channelRequested(channelId, postId);
            }
        },
        true);
}

} // namespace Mattermost
