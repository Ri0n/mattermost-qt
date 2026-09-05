#include "QuotedReplyFormat.h"

#include <algorithm>

namespace Mattermost::QuotedReplyFormat {
namespace {

QString escapeLinkLabel(QString text)
{
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('['), QStringLiteral("\\["));
    text.replace(QLatin1Char(']'), QStringLiteral("\\]"));
    return text;
}

} // namespace

QString compactText(const QString& message, bool hasAttachments, int limit)
{
    QString text = message;
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text = text.simplified();

    if (text.isEmpty() && hasAttachments) {
        text = QStringLiteral("[attachment]");
    } else if (text.isEmpty()) {
        text = QStringLiteral("[empty message]");
    }

    limit = std::max(2, limit);
    if (text.size() > limit) {
        text.truncate(limit - 1);
        text += QChar(0x2026);
    }
    return text;
}

QString buildFallback(const QString& postId,
                      const QString& author,
                      const QString& message,
                      bool hasAttachments)
{
    if (postId.isEmpty()) {
        return QString();
    }

    const QString safeAuthor = escapeLinkLabel(
        author.isEmpty() ? QStringLiteral("deleted user") : author);
    const QString quote = compactText(message, hasAttachments);

    return QStringLiteral("> [Replying to %1](/_redirect/pl/%2)\n> %3\n>\n\n")
        .arg(safeAuthor, postId, quote);
}

QString fallbackPrefix(const QString& wireMessage)
{
    if (!wireMessage.startsWith(QStringLiteral("> [Replying to "))) {
        return QString();
    }

    static const QString terminator = QStringLiteral("\n>\n\n");
    const int end = wireMessage.indexOf(terminator);
    if (end < 0) {
        return QString();
    }
    return wireMessage.left(end + terminator.size());
}

QString stripFallback(const QString& wireMessage)
{
    const QString prefix = fallbackPrefix(wireMessage);
    return prefix.isEmpty() ? wireMessage : wireMessage.mid(prefix.size());
}

} // namespace Mattermost::QuotedReplyFormat
