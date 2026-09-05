#pragma once

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include "HTTPConnector.h"

namespace Mattermost {

class Backend;
class BackendChannel;

/**
 * Post creation path that preserves the existing Backend::addPost behaviour
 * while allowing structured Mattermost post props when the caller needs them.
 */
class PostCreateService final : public QObject
{
    Q_OBJECT
public:
    static PostCreateService& instance(Backend& backend);

    void createPost(BackendChannel& channel,
                    const QString& message,
                    const QList<QString>& attachments,
                    const QString& rootId = QString(),
                    const QJsonObject& props = QJsonObject());

private:
    explicit PostCreateService(Backend& backend);

    Backend& backend;
    HTTPConnector httpConnector;
};

} // namespace Mattermost
