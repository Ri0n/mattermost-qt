#pragma once

#include <QFont>
#include <QString>
#include <QStringList>

#include "ui/EmojiFont.h"

namespace Mattermost::EmojiDialogSupport {

inline QString normalizeSearchTerm(QString term)
{
    term = term.trimmed().toLower();
    while (term.startsWith(QLatin1Char(':'))) {
        term.remove(0, 1);
    }
    while (term.endsWith(QLatin1Char(':'))) {
        term.chop(1);
    }
    term.replace(QLatin1Char('-'), QLatin1Char('_'));
    term.replace(QLatin1Char(' '), QLatin1Char('_'));
    return term;
}

inline bool matchesSearch(const QString& emojiName, const QString& term)
{
    const QString needle = normalizeSearchTerm(term);
    if (needle.isEmpty()) {
        return true;
    }

    const QString normalizedName = normalizeSearchTerm(emojiName);
    int searchFrom = 0;
    const QStringList tokens = needle.split(QLatin1Char('_'));
    for (const QString& token : tokens) {
        if (token.isEmpty()) {
            continue;
        }
        const int foundAt = normalizedName.indexOf(token, searchFrom);
        if (foundAt < 0) {
            return false;
        }
        searchFrom = foundAt + token.size();
    }
    return true;
}

inline QFont emojiButtonFont(QFont font, int pointSize = 16)
{
    font.setPointSize(pointSize);
    return EmojiFont::applySystemEmojiFamily(font);
}

} // namespace Mattermost::EmojiDialogSupport
