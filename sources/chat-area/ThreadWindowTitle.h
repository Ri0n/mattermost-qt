/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. If not, see https://www.gnu.org/licenses/.
 */

#pragma once

#include <QDateTime>
#include <QString>

#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

inline QString threadRootSummary(const BackendPost& post)
{
    QString firstLine;
    const QString message = post.message.trimmed();
    if (!message.isEmpty()) {
        firstLine = message.section(QLatin1Char('\n'), 0, 0).simplified();

        // A markdown image-only root is less useful as a title than the actual
        // attachment filename available on the post.
        const bool looksLikeImageOnlyMarkdown = firstLine.startsWith(QStringLiteral("!["))
            && firstLine.contains(QStringLiteral("]("))
            && firstLine.endsWith(QLatin1Char(')'));
        if (looksLikeImageOnlyMarkdown) {
            firstLine.clear();
        }
    }

    if (!firstLine.isEmpty()) {
        // Prefer a complete first sentence when it is reasonably short. Dots
        // inside account names and URLs do not terminate a sentence unless
        // followed by whitespace/end-of-line.
        for (int i = 0; i < firstLine.size(); ++i) {
            const QChar ch = firstLine.at(i);
            if ((ch == QLatin1Char('.') || ch == QLatin1Char('!') || ch == QLatin1Char('?'))
                && (i + 1 == firstLine.size() || firstLine.at(i + 1).isSpace())) {
                firstLine.truncate(i + 1);
                break;
            }
        }

        constexpr int MaxSummaryLength = 96;
        if (firstLine.size() > MaxSummaryLength) {
            firstLine = firstLine.left(MaxSummaryLength - 1) + QChar(0x2026);
        }
        return firstLine;
    }

    for (const BackendFile& file : post.files) {
        if (!file.name.trimmed().isEmpty()) {
            return file.name.trimmed();
        }
    }

    QStringList fallback;
    const QString author = post.getDisplayAuthorName().trimmed();
    if (!author.isEmpty()) {
        fallback.push_back(author);
    }
    if (post.create_at != 0) {
        fallback.push_back(QDateTime::fromMSecsSinceEpoch(post.create_at)
                               .toString(QStringLiteral("dd MMM yyyy")));
    }
    return fallback.join(QStringLiteral(" · "));
}

inline QString threadWindowTitle(const BackendChannel& channel, const BackendPost& rootPost)
{
    QString channelName = channel.display_name.trimmed();
    if (channelName.isEmpty()) {
        channelName = QStringLiteral("Mattermost");
    }

    const QString summary = threadRootSummary(rootPost);
    if (summary.isEmpty()) {
        return channelName + QStringLiteral(" — Thread");
    }
    return channelName + QStringLiteral(" — ") + summary;
}

} // namespace Mattermost
