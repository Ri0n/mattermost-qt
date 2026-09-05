#include "AppNavigationService.h"

#include <functional>
#include <utility>

#include <QApplication>
#include <QDesktopServices>
#include <QMap>
#include <QPointer>

#include "backend/Backend.h"
#include "backend/NetworkRequest.h"
#include "backend/PostRepository.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendUser.h"
#include "mainwindow.h"

namespace Mattermost {

AppNavigationService& AppNavigationService::instance(Backend& backend)
{
    static QMap<Backend*, AppNavigationService*> instances;
    auto it = instances.find(&backend);
    if (it == instances.end()) {
        it = instances.insert(backend, new AppNavigationService(backend));
    }
    return **it;
}

AppNavigationService::AppNavigationService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
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
        if (channel && channel->postIdToPost.contains(postId)) {
            return channel;
        }
    }
    return nullptr;
}

void AppNavigationService::openChannel(const QString& channelId)
{
    if (!channelId.isEmpty() && backend.getStorage().getChannelById(channelId)) {
        emit channelRequested(channelId, QString(), QString(), QStringList(), false, false);
    }
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
            openChannel(channel->id);
            return;
        }
    }

    // Mattermost direct-message deep links use /<team>/messages/@<username>.
    // Existing DMs are already represented in Storage; route them without
    // sending a perfectly local navigation action through the browser.
    if (path.size() >= 3 && path.at(1) == QStringLiteral("messages")) {
        QString username = path.at(2);
        if (username.startsWith(QLatin1Char('@'))) {
            username.remove(0, 1);
        }
        for (const auto& entry : backend.getStorage().getAllUsers()) {
            const BackendUser& user = entry.second;
            if (QString::compare(user.username, username, Qt::CaseInsensitive) != 0) {
                continue;
            }
            if (BackendChannel* channel = backend.getStorage().getDirectChannelByUserId(user.id)) {
                openChannel(channel->id);
                return;
            }
            break;
        }
    }

    // Both normal permalinks (/<team>/pl/<post>) and Mattermost's
    // /_redirect/pl/<post> form have "pl" as the second path component.
    if (path.size() >= 3 && path.at(1) == QStringLiteral("pl")) {
        openPost(path.at(2));
        return;
    }

    const QUrl browserUrl = url.isRelative()
        ? QUrl(NetworkRequest::host()).resolved(url)
        : url;
    QDesktopServices::openUrl(browserUrl);
}

void AppNavigationService::openPost(const QString& postId)
{
    if (BackendChannel* channel = findPostChannel(postId)) {
        openPostInChannel(*channel, postId);
        return;
    }

    QPointer<AppNavigationService> guard(this);
    PostRepository::instance(backend).loadPost(
        postId,
        [guard, postId](const PostRepository::PostResult& result) {
            if (!guard || !result.success) {
                return;
            }
            BackendChannel* channel = guard->backend.getStorage().getChannelById(result.channelId);
            if (channel) {
                guard->openPostInChannel(*channel, postId);
            }
        });
}

void AppNavigationService::openThreadAtLastViewed(const QString& channelId,
                                                  const QString& rootId,
                                                  uint64_t lastViewedAt,
                                                  const QString& fallbackPostId,
                                                  NavigationCallback callback)
{
    BackendChannel* channel = backend.getStorage().getChannelById(channelId);
    if (!channel || rootId.isEmpty()) {
        if (callback) {
            callback(false);
        }
        return;
    }

    QPointer<AppNavigationService> guard(this);
    PostRepository::instance(backend).loadThreadFromTime(
        *channel, rootId, 30, lastViewedAt,
        [guard, channelId, rootId, lastViewedAt, fallbackPostId,
         callback = std::move(callback)](const PostRepository::Page& page) mutable {
            if (!guard) {
                return;
            }

            BackendChannel* currentChannel = guard->backend.getStorage().getChannelById(channelId);
            QString targetPostId;
            if (page.success && currentChannel) {
                for (const QString& postId : page.postIds) {
                    BackendPost* post = currentChannel->postIdToPost.value(postId, nullptr);
                    if (post && post->root_id == rootId && post->create_at > lastViewedAt) {
                        targetPostId = postId;
                        break;
                    }
                }
            }

            if (targetPostId.isEmpty() && !fallbackPostId.isEmpty()) {
                targetPostId = fallbackPostId;
            }

            if (!targetPostId.isEmpty()) {
                guard->openPost(targetPostId);
                if (callback) {
                    callback(true);
                }
                return;
            }

            if (page.success && currentChannel) {
                emit guard->channelRequested(channelId,
                                             rootId,
                                             rootId,
                                             QStringList(),
                                             false,
                                             false);
                if (callback) {
                    callback(true);
                }
                return;
            }

            if (callback) {
                callback(false);
            }
        });
}

void AppNavigationService::openPostInChannel(BackendChannel& channel,
                                             const QString& postId)
{
    // A cached reply carries its exact thread identity, so semantic navigation
    // can route directly to the thread without fetching an unused channel window.
    if (BackendPost* cached = channel.postIdToPost.value(postId, nullptr)) {
        if (!cached->root_id.isEmpty()) {
            emit channelRequested(channel.id,
                                  postId,
                                  cached->root_id,
                                  QStringList(),
                                  false,
                                  false);
            return;
        }
    }

    QPointer<AppNavigationService> guard(this);
    const QString channelId = channel.id;
    PostRepository::instance(backend).loadChannelAround(
        channel, postId,
        [guard, channelId, postId](const PostRepository::Context& context) {
            if (!guard || !context.success) {
                return;
            }

            QString rootId;
            if (BackendChannel* currentChannel =
                    guard->backend.getStorage().getChannelById(channelId)) {
                if (BackendPost* target = currentChannel->postIdToPost.value(postId, nullptr)) {
                    rootId = target->root_id;
                }
            }

            emit guard->channelRequested(channelId,
                                         postId,
                                         rootId,
                                         context.postIds,
                                         context.reachedOldest,
                                         context.reachedNewest);
        },
        true);
}

} // namespace Mattermost
