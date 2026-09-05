/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
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

#include <algorithm>

#include <QApplication>
#include <QPalette>
#include <QRegularExpression>
#include <QTextBlock>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QTextDocument>
#include <QWidget>

namespace Mattermost {

namespace UserMentionLinkifier {

inline const QRegularExpression& mentionExpression()
{
    // Mattermost usernames may contain letters, digits, '.', '_' and '-'. The
    // negative left context avoids turning the domain half of an e-mail address
    // into a mention.
    static const QRegularExpression expression(
        QStringLiteral(R"((?<![A-Za-z0-9._-])@([A-Za-z0-9][A-Za-z0-9._-]*))"));
    return expression;
}

inline bool isSpecialMention(const QString& name)
{
    return name.compare(QStringLiteral("all"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("channel"), Qt::CaseInsensitive) == 0
        || name.compare(QStringLiteral("here"), Qt::CaseInsensitive) == 0;
}

inline QString mentionHref(const QString& username)
{
    return QStringLiteral("mattermost-user:///") + username;
}

inline bool rangeAlreadyLinkedOrCode(QTextDocument& document, int position, int length)
{
    QTextCursor cursor(&document);
    for (int i = 0; i < length; ++i) {
        cursor.setPosition(position + i);
        const QTextCharFormat format = cursor.charFormat();
        if (format.isAnchor() || format.fontFixedPitch()) {
            return true;
        }
    }
    return false;
}

inline QPalette documentPalette(const QTextDocument& document)
{
    if (const auto* widget = qobject_cast<const QWidget*>(document.parent())) {
        return widget->palette();
    }
    return QApplication::palette();
}

inline QTextCharFormat userMentionFormat(const QPalette& palette, const QString& username)
{
    QTextCharFormat format;
    format.setAnchor(true);
    format.setAnchorHref(mentionHref(username));
    format.setForeground(palette.color(QPalette::Link));
    format.setFontUnderline(false);
    return format;
}

inline QTextCharFormat specialMentionFormat(const QPalette& palette)
{
    QTextCharFormat format;
    QColor background = palette.color(QPalette::Highlight);
    // Keep special mentions visibly selection-like without making a normal
    // message look as if the user currently selected the text.
    background.setAlphaF(std::min<qreal>(background.alphaF(), 0.45));
    format.setBackground(background);
    format.setForeground(palette.color(QPalette::Text));
    format.setFontUnderline(false);
    format.setAnchor(false);
    return format;
}

inline void linkify(QTextDocument& document)
{
    struct Replacement {
        int position = 0;
        int length = 0;
        QString username;
        bool special = false;
    };

    QList<Replacement> replacements;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QRegularExpressionMatchIterator matches = mentionExpression().globalMatch(block.text());
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const QString username = match.captured(1);
            const int position = block.position() + static_cast<int>(match.capturedStart(0));
            const int length = static_cast<int>(match.capturedLength(0));
            if (rangeAlreadyLinkedOrCode(document, position, length)) {
                continue;
            }
            replacements.push_back({position, length, username, isSpecialMention(username)});
        }
    }

    std::sort(replacements.begin(), replacements.end(),
              [](const Replacement& lhs, const Replacement& rhs) {
        return lhs.position > rhs.position;
    });

    const QPalette palette = documentPalette(document);
    for (const Replacement& replacement : replacements) {
        QTextCursor cursor(&document);
        cursor.setPosition(replacement.position);
        cursor.setPosition(replacement.position + replacement.length,
                           QTextCursor::KeepAnchor);
        cursor.mergeCharFormat(replacement.special
                                   ? specialMentionFormat(palette)
                                   : userMentionFormat(palette, replacement.username));
    }
}

inline QString linkifyHtmlTextSegment(const QString& text)
{
    QString result;
    int position = 0;
    QRegularExpressionMatchIterator matches = mentionExpression().globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString username = match.captured(1);
        if (isSpecialMention(username)) {
            continue;
        }

        result += text.mid(position, static_cast<int>(match.capturedStart(0)) - position);
        const QString mention = match.captured(0);
        result += QStringLiteral("<a href=\"") + mentionHref(username)
            + QStringLiteral("\" style=\"text-decoration:none\">") + mention
            + QStringLiteral("</a>");
        position = static_cast<int>(match.capturedEnd(0));
    }
    result += text.mid(position);
    return result;
}

inline QString linkifyHtml(const QString& html)
{
    // Qt 5's compatibility formatter produces a very small HTML subset. Walk
    // text nodes rather than regexing the whole document so usernames inside an
    // existing URL anchor/href are never reinterpreted as mentions.
    QString result;
    result.reserve(html.size() + 32);

    int position = 0;
    int anchorDepth = 0;
    while (position < html.size()) {
        const int tagStart = html.indexOf(QLatin1Char('<'), position);
        const int textEnd = tagStart < 0 ? html.size() : tagStart;
        const QString text = html.mid(position, textEnd - position);
        result += anchorDepth == 0 ? linkifyHtmlTextSegment(text) : text;

        if (tagStart < 0) {
            break;
        }
        const int tagEnd = html.indexOf(QLatin1Char('>'), tagStart + 1);
        if (tagEnd < 0) {
            result += html.mid(tagStart);
            break;
        }

        const QString tag = html.mid(tagStart, tagEnd - tagStart + 1);
        const QString lower = tag.toLower();
        if (lower.startsWith(QStringLiteral("<a "))
            || lower == QStringLiteral("<a>")) {
            ++anchorDepth;
        } else if (lower.startsWith(QStringLiteral("</a")) && anchorDepth > 0) {
            --anchorDepth;
        }
        result += tag;
        position = tagEnd + 1;
    }
    return result;
}

} // namespace UserMentionLinkifier

} // namespace Mattermost
