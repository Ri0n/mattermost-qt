#include "PostCreateService.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QPointer>

#include "Backend.h"
#include "NetworkRequest.h"
#include "PostRepository.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendNewPollData.h"
#include "backend/types/BackendPost.h"

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
    httpConnector.post(request, json, HttpResponseCallback(
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
    httpConnector.put(request, json, HttpResponseCallback(
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
    if (!channel.team) {
        if (callback) {
            callback(false);
        }
        return;
    }

    NetworkRequest request(QStringLiteral("actions/dialogs/submit"));
    QJsonObject json {
        {QStringLiteral("callback_id"), QString()},
        {QStringLiteral("channel_id"), channel.id},
        {QStringLiteral("state"), QString()},
        {QStringLiteral("url"), QStringLiteral("/plugins/com.github.matterpoll.matterpoll/api/v1/polls/create")},
        {QStringLiteral("team_id"), channel.team->id},
    };

    QJsonObject submission {
        {QStringLiteral("question"), pollData.question},
    };
    for (int i = 0; i < pollData.options.size(); ++i) {
        submission.insert(QStringLiteral("option") + QString::number(i + 1),
                          pollData.options.at(i));
    }
    if (pollData.isAnonymous) {
        submission.insert(QStringLiteral("setting-anonymous"), true);
    }
    if (pollData.showProgress) {
        submission.insert(QStringLiteral("setting-progress"), true);
    }
    if (pollData.allowAddOptions) {
        submission.insert(QStringLiteral("setting-public-add-option"), true);
    }
    json.insert(QStringLiteral("submission"), submission);

    httpConnector.post(request, json, HttpResponseCallback(
        [callback = std::move(callback)](QVariant status, QByteArray) mutable {
            if (callback) {
                callback(status.toInt() == QNetworkReply::NoError);
            }
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
