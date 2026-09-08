#include "AppNavigationService.h"

#include <functional>
#include <utility>

#include <QApplication>
#include <QDesktopServices>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QMap>
#include <QNetworkReply>
#include <QPointer>

#include "backend/Backend.h"
#include "backend/HTTPConnector.h"
#include "backend/HttpResponseCallback.h"
#include "backend/NetworkRequest.h"
#include "backend/PostRepository.h"
#include "backend/Storage.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendTeam.h"
#include "backend/types/BackendUser.h"
#include "mainwindow.h"

namespace Mattermost {
namespace {

Q_LOGGING_CATEGORY(lcNavigationResolve, "mattermost.navigation.resolve", QtWarningMsg)

/**
 * Resolve a cold permalink without assuming that its channel is already in
 * Storage. PostRepository deliberately cannot materialize a post until its
 * BackendChannel exists, so navigation first resolves the post identity, then
 * lazily fetches the missing channel, and finally hands the authoritative post
 * snapshot back to PostRepository for normal resident/cache ingestion.
 */
class NavigationPostResolver final : public QObject
{
public:
    using Callback = std::function<void(BackendChannel*)>;

    NavigationPostResolver(Backend& sourceBackend,
                           QString targetPostId,
                           Callback completed,
                           QObject* parent)
        : QObject(parent)
        , backend(sourceBackend)
        , postId(std::move(targetPostId))
        , callback(std::move(completed))
    {
        connect(&httpConnector, &HTTPConnector::onNetworkError,
                &backend, &Backend::onNetworkError);
        connect(&httpConnector, &HTTPConnector::onHttpError,
                &backend, &Backend::onHttpError);
    }

    void start()
    {
        if (postId.isEmpty()) {
            qCWarning(lcNavigationResolve) << "Cannot resolve an empty post id";
            finish(nullptr);
            return;
        }

        qCDebug(lcNavigationResolve) << "Resolving post" << postId;
        QPointer<NavigationPostResolver> guard(this);
        NetworkRequest request(QStringLiteral("posts/") + postId);
        httpConnector.get(request, HttpResponseCallback(
            [guard](QVariant status, const QJsonDocument& doc) {
                if (guard) {
                    guard->handlePost(status, doc);
                }
            }));
    }

private:
    void handlePost(QVariant status, const QJsonDocument& doc)
    {
        if (status.toInt() != QNetworkReply::NoError || !doc.isObject()) {
            qCWarning(lcNavigationResolve)
                << "Failed to resolve post" << postId
                << "network status" << status.toInt();
            finish(nullptr);
            return;
        }

        postObject = doc.object();
        channelId = postObject.value(QStringLiteral("channel_id")).toString();
        if (channelId.isEmpty()) {
            qCWarning(lcNavigationResolve)
                << "Resolved post has no channel id" << postId;
            finish(nullptr);
            return;
        }

        if (BackendChannel* channel = backend.getStorage().getChannelById(channelId)) {
            finishWithChannel(channel);
            return;
        }

        qCDebug(lcNavigationResolve)
            << "Post" << postId << "requires missing channel" << channelId;
        requestChannel();
    }

    void requestChannel()
    {
        QPointer<NavigationPostResolver> guard(this);
        NetworkRequest request(QStringLiteral("channels/") + channelId);
        httpConnector.get(request, HttpResponseCallback(
            [guard](QVariant status, const QJsonDocument& doc) {
                if (guard) {
                    guard->handleChannel(status, doc);
                }
            }));
    }

    void handleChannel(QVariant status, const QJsonDocument& doc)
    {
        if (status.toInt() != QNetworkReply::NoError || !doc.isObject()) {
            qCWarning(lcNavigationResolve)
                << "Failed to load channel" << channelId
                << "for post" << postId
                << "network status" << status.toInt();
            finish(nullptr);
            return;
        }

        channelObject = doc.object();
        const QString returnedId = channelObject.value(QStringLiteral("id")).toString();
        if (returnedId != channelId) {
            qCWarning(lcNavigationResolve)
                << "Channel lookup returned unexpected id" << returnedId
                << "while resolving" << channelId;
            finish(nullptr);
            return;
        }

        if (BackendChannel* existing = backend.getStorage().getChannelById(channelId)) {
            finishWithChannel(existing);
            return;
        }

        const QString type = channelObject.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("D") || type == QLatin1String("G")) {
            finishWithChannel(materializeChannel());
            return;
        }

        const QString teamId = channelObject.value(QStringLiteral("team_id")).toString();
        if (teamId.isEmpty()) {
            qCWarning(lcNavigationResolve)
                << "Channel" << channelId << "has no team id";
            finish(nullptr);
            return;
        }

        if (backend.getStorage().getTeamById(teamId)) {
            finishWithChannel(materializeChannel());
            return;
        }

        requestTeam(teamId);
    }

    void requestTeam(const QString& teamId)
    {
        qCDebug(lcNavigationResolve)
            << "Channel" << channelId << "requires missing team" << teamId;
        QPointer<NavigationPostResolver> guard(this);
        NetworkRequest request(QStringLiteral("teams/") + teamId);
        httpConnector.get(request, HttpResponseCallback(
            [guard, teamId](QVariant status, const QJsonDocument& doc) {
                if (guard) {
                    guard->handleTeam(teamId, status, doc);
                }
            }));
    }

    void handleTeam(const QString& teamId, QVariant status, const QJsonDocument& doc)
    {
        if (status.toInt() != QNetworkReply::NoError || !doc.isObject()) {
            qCWarning(lcNavigationResolve)
                << "Failed to load team" << teamId
                << "for channel" << channelId
                << "network status" << status.toInt();
            finish(nullptr);
            return;
        }

        Storage& storage = backend.getStorage();
        BackendTeam* team = storage.getTeamById(teamId);
        if (!team) {
            storage.addTeam(doc.object());
            team = storage.getTeamById(teamId);
        }
        if (!team) {
            qCWarning(lcNavigationResolve)
                << "Could not materialize team" << teamId
                << "for channel" << channelId;
            finish(nullptr);
            return;
        }

        finishWithChannel(materializeChannel());
    }

    BackendChannel* materializeChannel()
    {
        Storage& storage = backend.getStorage();
        if (BackendChannel* existing = storage.getChannelById(channelId)) {
            return existing;
        }

        const QString type = channelObject.value(QStringLiteral("type")).toString();
        if (type == QLatin1String("D")) {
            return storage.addDirectChannel(channelObject);
        }
        if (type == QLatin1String("G")) {
            return storage.addGroupChannel(channelObject);
        }

        const QString teamId = channelObject.value(QStringLiteral("team_id")).toString();
        BackendTeam* team = storage.getTeamById(teamId);
        if (!team) {
            return nullptr;
        }
        return storage.addTeamChannel(*team, channelObject);
    }

    void finishWithChannel(BackendChannel* channel)
    {
        if (!channel) {
            qCWarning(lcNavigationResolve)
                << "Could not materialize channel" << channelId
                << "for post" << postId;
            finish(nullptr);
            return;
        }

        if (!PostRepository::instance(backend).ingestFetchedPost(postObject)) {
            qCWarning(lcNavigationResolve)
                << "Could not ingest resolved post" << postId
                << "into channel" << channel->id;
        }

        qCDebug(lcNavigationResolve)
            << "Resolved post" << postId << "to channel" << channel->id;
        finish(channel);
    }

    void finish(BackendChannel* channel)
    {
        Callback completed = std::move(callback);
        deleteLater();
        if (completed) {
            completed(channel);
        }
    }

    Backend& backend;
    QString postId;
    QString channelId;
    QJsonObject postObject;
    QJsonObject channelObject;
    Callback callback;
    HTTPConnector httpConnector;
};

} // namespace

AppNavigationService& AppNavigationService::instance(Backend& backend)
{
    static QMap<Backend*, AppNavigationService*> instances;
    auto it = instances.find(&backend);
    if (it == instances.end()) {
        it = instances.insert(&backend, new AppNavigationService(backend));
    }
    return **it;
}

AppNavigationService::AppNavigationService(Backend& sourceBackend)
    : QObject(&sourceBackend)
    , backend(sourceBackend)
{
    ensureMainWindowConnection();
}

void AppNavigationService::ensureMainWindowConnection()
{
    // The singleton can be instantiated by cached-link/sidebar code before the
    // MainWindow exists. Re-discover it at the navigation boundary; the unique
    // connection makes this cheap and idempotent and removes startup-order
    // dependence from semantic navigation.
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
        ensureMainWindowConnection();
        emit channelRequested(channelId, QString(), QString(), QStringList(),
                              false, false, false);
    }
}

void AppNavigationService::openThread(const QString& channelId, const QString& rootId)
{
    if (channelId.isEmpty() || rootId.isEmpty()
        || !backend.getStorage().getChannelById(channelId)) {
        return;
    }

    // A root-message click means "present this thread", not "reset its
    // viewport". MainWindow/NavigationUiController decide whether that means
    // creating a docked thread, revealing an existing docked one, or raising a
    // detached window. New threads still start at the newest edge.
    ensureMainWindowConnection();
    emit channelRequested(channelId, QString(), rootId, QStringList(),
                          false, true, true);
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
    if (postId.isEmpty()) {
        qCWarning(lcNavigationResolve) << "Ignoring navigation to an empty post id";
        return;
    }

    if (BackendChannel* channel = findPostChannel(postId)) {
        openPostInChannel(*channel, postId);
        return;
    }

    QPointer<AppNavigationService> guard(this);
    auto* resolver = new NavigationPostResolver(
        backend,
        postId,
        [guard, postId](BackendChannel* channel) {
            if (!guard) {
                return;
            }
            if (!channel) {
                qCWarning(lcNavigationResolve)
                    << "Navigation target could not be resolved" << postId;
                return;
            }
            guard->openPostInChannel(*channel, postId);
        },
        this);
    resolver->start();
}

void AppNavigationService::openThreadAtLastViewed(const QString& channelId,
                                                  const QString& rootId,
                                                  uint64_t lastViewedAt,
                                                  const QString& fallbackPostId,
                                                  NavigationCallback callback,
                                                  bool preserveIfOpen)
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
        [guard, channelId, rootId, lastViewedAt, fallbackPostId, preserveIfOpen,
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

            guard->ensureMainWindowConnection();
            if (!targetPostId.isEmpty()) {
                // Callers choose whether an already-open thread should preserve
                // its viewport (Following) or jump/highlight again (Attention).
                emit guard->channelRequested(channelId,
                                             targetPostId,
                                             rootId,
                                             QStringList(),
                                             false,
                                             false,
                                             preserveIfOpen);
                if (callback) {
                    callback(true);
                }
                return;
            }

            if (page.success && currentChannel) {
                // Nothing exists after last_viewed_at. This is the normal path
                // for a previously read followed thread, and can also happen if
                // server unread metadata races the thread page. Open the thread
                // without an explicit post target; callers still decide whether
                // an existing viewport is preserved or repositioned.
                emit guard->channelRequested(channelId,
                                             QString(),
                                             rootId,
                                             QStringList(),
                                             false,
                                             true,
                                             preserveIfOpen);
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
    if (BackendPost* cached = channel.postIdToPost.value(postId, nullptr)) {
        if (!cached->root_id.isEmpty()) {
            const QString channelId = channel.id;
            const QString rootId = cached->root_id;
            const auto presentReply = [this, channelId, postId, rootId] {
                ensureMainWindowConnection();
                emit channelRequested(channelId,
                                      postId,
                                      rootId,
                                      QStringList(),
                                      false,
                                      false,
                                      false);
            };

            // ThreadPostSource derives its logical length and time anchors from
            // the root's reply_count/last_reply_at metadata. A direct permalink
            // can make the reply resident before its root is present, in which
            // case creating the thread immediately yields a zero-row source.
            if (channel.postIdToPost.contains(rootId)) {
                presentReply();
                return;
            }

            QPointer<AppNavigationService> guard(this);
            PostRepository::instance(backend).loadPost(
                rootId,
                [guard, channelId, postId, rootId](const PostRepository::PostResult& result) {
                    if (!guard || !result.success || result.channelId != channelId) {
                        return;
                    }

                    BackendChannel* currentChannel =
                        guard->backend.getStorage().getChannelById(channelId);
                    BackendPost* root = currentChannel
                        ? currentChannel->postIdToPost.value(rootId, nullptr) : nullptr;
                    BackendPost* target = currentChannel
                        ? currentChannel->postIdToPost.value(postId, nullptr) : nullptr;
                    if (!root || !target || target->root_id != rootId) {
                        return;
                    }

                    guard->ensureMainWindowConnection();
                    emit guard->channelRequested(channelId,
                                                 postId,
                                                 rootId,
                                                 QStringList(),
                                                 false,
                                                 false,
                                                 false);
                });
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

            guard->ensureMainWindowConnection();
            emit guard->channelRequested(channelId,
                                         postId,
                                         rootId,
                                         context.postIds,
                                         context.reachedOldest,
                                         context.reachedNewest,
                                         false);
        },
        true);
}

} // namespace Mattermost
