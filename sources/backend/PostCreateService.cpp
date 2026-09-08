#include "PostCreateService.h"

#include <QDebug>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>
#include <QStringList>

#include "Backend.h"
#include "NetworkRequest.h"
#include "PostRepository.h"
#include "QByteArrayCreator.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendNewPollData.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {
namespace {

QString quoteMatterpollArgument(QString value)
{
    value.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    return QLatin1Char('"') + value + QLatin1Char('"');
}

QString buildMatterpollCommand(const QString& trigger, const BackendNewPollData& pollData)
{
    QStringList parts;
    parts.push_back(QLatin1Char('/') + trigger);
    parts.push_back(quoteMatterpollArgument(pollData.question));
    for (const QString& option : pollData.options) {
        parts.push_back(quoteMatterpollArgument(option));
    }

    if (pollData.isAnonymous) {
        parts.push_back(QStringLiteral("--anonymous"));
    }
    if (pollData.isAnonymousCreator) {
        parts.push_back(QStringLiteral("--anonymous-creator"));
    }
    if (pollData.showProgress) {
        parts.push_back(QStringLiteral("--progress"));
    }
    if (pollData.allowAddOptions) {
        parts.push_back(QStringLiteral("--public-add-option"));
    }
    parts.push_back(QStringLiteral("--votes=%1").arg(pollData.maxVotes));
    return parts.join(QLatin1Char(' '));
}

} // namespace

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
    connect(&httpConnector, &HTTPConnector::onNetworkError,
            &backend, &Backend::onNetworkError);
    connect(&httpConnector, &HTTPConnector::onHttpError,
            &backend, &Backend::onHttpError);
}

void PostCreateService::createPost(BackendChannel& channel,
                                   const QString& message,
                                   const QList<QString>& attachments,
                                   const QString& rootId,
                                   const QJsonObject& props,
                                   const QString& pendingPostId,
                                   PostCallback callback)
{
    QJsonArray files;
    for (const QString& id : attachments) {
        files.push_back(id);
    }

    QJsonObject json;
    json.insert(QStringLiteral("channel_id"), channel.id);
    json.insert(QStringLiteral("message"), message);
    if (!props.isEmpty()) {
        json.insert(QStringLiteral("props"), props);
    }
    if (!files.isEmpty()) {
        json.insert(QStringLiteral("file_ids"), files);
    }
    if (!rootId.isEmpty()) {
        json.insert(QStringLiteral("root_id"), rootId);
    }
    if (!pendingPostId.isEmpty()) {
        // Mattermost uses pending_post_id as the idempotency key for duplicate
        // create requests. The same value must survive an ambiguous/manual retry.
        json.insert(QStringLiteral("pending_post_id"), pendingPostId);
    }

    QPointer<PostCreateService> guard(this);
    NetworkRequest request(QStringLiteral("posts"));
    const QByteArrayCreator payload(json);
    httpConnector.post(request, payload, HttpResponseCallback(
        [guard, callback = std::move(callback)](QVariant status,
                                                const QJsonDocument& doc) mutable {
            BackendPost* post = nullptr;
            if (guard && status.toInt() == QNetworkReply::NoError && doc.isObject()) {
                post = guard->ingestCreatedPost(doc.object());
            }
            if (callback) {
                callback(post);
            }
        }));
}

void PostCreateService::editPost(const QString& postId,
                                 const QString& message,
                                 const QList<QString>& attachments,
                                 PostCallback callback)
{
    QJsonArray files;
    for (const QString& id : attachments) {
        files.push_back(id);
    }

    QJsonObject json;
    json.insert(QStringLiteral("message"), message);
    if (!files.isEmpty()) {
        json.insert(QStringLiteral("file_ids"), files);
    }

    QPointer<PostCreateService> guard(this);
    NetworkRequest request(QStringLiteral("posts/") + postId + QStringLiteral("/patch"));
    const QByteArrayCreator payload(json);
    httpConnector.put(request, payload, HttpResponseCallback(
        [guard, callback = std::move(callback)](QVariant status,
                                                const QJsonDocument& doc) mutable {
            BackendPost* post = nullptr;
            if (guard && status.toInt() == QNetworkReply::NoError && doc.isObject()) {
                post = guard->ingestEditedPost(doc.object());
            }
            if (callback) {
                callback(post);
            }
        }));
}

void PostCreateService::submitPoll(BackendChannel& channel,
                                   const BackendNewPollData& pollData,
                                   ResultCallback callback)
{
    // Matterpoll's interactive-dialog API is intentionally not used here: its
    // create handler is hard-coded to option1..option3. The slash-command path
    // accepts an arbitrary option list and supports the same poll settings.
    const QString channelId = channel.id;
    const QString teamId = channel.team ? channel.team->id : QString();
    const BackendNewPollData commandPollData = pollData;
    QPointer<PostCreateService> guard(this);

    NetworkRequest configurationRequest(NetworkRequest::matterpoll,
                                        QStringLiteral("configuration"));
    httpConnector.get(configurationRequest, HttpResponseCallback(
        [guard, channelId, teamId, commandPollData,
         callback = std::move(callback)](QVariant configurationStatus,
                                         const QJsonDocument& configurationDocument) mutable {
            if (!guard) {
                return;
            }

            if (configurationStatus.toInt() != QNetworkReply::NoError
                || !configurationDocument.isObject()) {
                qWarning().noquote()
                    << "Matterpoll configuration request failed: networkError="
                    << configurationStatus.toInt();
                if (callback) {
                    callback(false);
                }
                return;
            }

            const QString trigger = configurationDocument.object()
                .value(QStringLiteral("trigger")).toString().trimmed();
            if (trigger.isEmpty()) {
                qWarning() << "Matterpoll configuration returned an empty trigger";
                if (callback) {
                    callback(false);
                }
                return;
            }

            NetworkRequest request(QStringLiteral("commands/execute"));
            QJsonObject json {
                {QStringLiteral("channel_id"), channelId},
                {QStringLiteral("command"), buildMatterpollCommand(trigger, commandPollData)},
                {QStringLiteral("root_id"), commandPollData.rootId},
            };
            if (!teamId.isEmpty()) {
                json.insert(QStringLiteral("team_id"), teamId);
            }

            qInfo().noquote() << "Poll command submit: channel=" << channelId
                              << "root=" << commandPollData.rootId
                              << "trigger=/" << trigger
                              << "options=" << commandPollData.options.size();

            const QByteArrayCreator payload(json);
            guard->httpConnector.post(request, payload, HttpResponseCallback(
                [callback = std::move(callback)](QVariant status,
                                                 QByteArray response) mutable {
                    const bool transportSuccess = status.toInt() == QNetworkReply::NoError;
                    qInfo().noquote() << "Poll command submit finished: networkError="
                                      << status.toInt()
                                      << "bytes=" << response.size()
                                      << "transportSuccess=" << transportSuccess;
                    if (!transportSuccess && !response.trimmed().isEmpty()) {
                        qWarning().noquote() << "Poll command submit response:"
                                             << QString::fromUtf8(response.left(1024));
                    }

                    // A successful command HTTP response is not a reliable poll
                    // acknowledgement: plugins may return HTTP 200 while posting
                    // an ephemeral error. Successful creation is confirmed by the
                    // correlated Matterpoll bot post in OutgoingPostCreator.
                    if (!transportSuccess && callback) {
                        callback(false);
                    }
                }));
        }));
}

BackendPost* PostCreateService::ingestCreatedPost(const QJsonObject& postObject)
{
    const QString postId = postObject.value(QStringLiteral("id")).toString();
    const QString channelId = postObject.value(QStringLiteral("channel_id")).toString();
    if (postId.isEmpty() || channelId.isEmpty()) {
        return nullptr;
    }

    PostRepository::instance(backend).cachePostObject(postObject);

    BackendChannel* channel = backend.getStorage().getChannelById(channelId);
    if (!channel) {
        return nullptr;
    }

    const bool alreadyKnown = channel->postIdToPost.contains(postId);
    BackendPost* post = channel->addPost(postObject);
    if (post && !alreadyKnown) {
        // This is the sender-side local delivery corresponding to the HTTP
        // acknowledgement. Do not emit Backend::onNewPost here: notification
        // semantics remain owned by the realtime/fallback incoming path.
        emit channel->onNewPost(*post);
    }
    return post;
}

BackendPost* PostCreateService::ingestEditedPost(const QJsonObject& postObject)
{
    const QString postId = postObject.value(QStringLiteral("id")).toString();
    const QString channelId = postObject.value(QStringLiteral("channel_id")).toString();
    if (postId.isEmpty() || channelId.isEmpty()) {
        return nullptr;
    }

    PostRepository::instance(backend).cachePostObject(postObject);

    BackendChannel* channel = backend.getStorage().getChannelById(channelId);
    if (!channel) {
        return nullptr;
    }

    BackendPost* existing = channel->postIdToPost.value(postId, nullptr);
    if (!existing) {
        existing = channel->addPost(postObject);
        if (existing) {
            emit channel->onNewPost(*existing);
        }
        return existing;
    }

    BackendPost updated(postObject, backend.getStorage());
    channel->editPost(updated);
    return channel->postIdToPost.value(postId, nullptr);
}

} // namespace Mattermost
