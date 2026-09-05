#pragma once

#include <QString>

namespace Mattermost::QuotedReplyFormat {

constexpr int PreviewTextLimit = 160;

QString compactText(const QString& message, bool hasAttachments,
                    int limit = PreviewTextLimit);
QString buildFallback(const QString& postId,
                      const QString& author,
                      const QString& message,
                      bool hasAttachments);
QString fallbackPrefix(const QString& wireMessage);
QString stripFallback(const QString& wireMessage);

} // namespace Mattermost::QuotedReplyFormat
