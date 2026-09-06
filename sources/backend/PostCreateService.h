#pragma once

#include <functional>

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;
class BackendChannel;
class BackendPost;
struct BackendNewPollData;

/**
 * HTTP owner for composer post mutations.
 *
 * A successful REST response is the authoritative acknowledgement. WebSocket
 * events remain a realtime synchronization path and may arrive before or after
 * the corresponding HTTP response.
 */
class PostCreateService final : public QObject
{
public:
    using PostCallback = std::function<void(BackendPost*)>;
    using ResultCallback = std::function<void(bool)>;

    static PostCreateService& instance(Backend& backend);

    void createPost(BackendChannel& channel,
                    const QString& message,
                    const QList<QString>& attachments,
                    const QString& rootId = QString(),
                    const QJsonObject& props = QJsonObject(),
                    const QString& pendingPostId = QString(),
                    PostCallback callback = {});

    void editPost(const QString& postId,
                  const QString& message,
                  const QList<QString>& attachments,
                  PostCallback callback = {});

    void submitPoll(BackendChannel& channel,
                    const BackendNewPollData& pollData,
                    ResultCallback callback = {});

private:
    explicit PostCreateService(Backend& backend);

    BackendPost* ingestCreatedPost(const QJsonObject& postObject);
    BackendPost* ingestEditedPost(const QJsonObject& postObject);

    Backend& backend;
    HTTPConnector httpConnector;
};

} // namespace Mattermost
