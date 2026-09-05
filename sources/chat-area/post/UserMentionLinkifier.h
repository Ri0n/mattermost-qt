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

#include <algorithm>

#include <QApplication>
#include <QHash>
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

inline QString userMentionHref(const QString& username)
{
    return QStringLiteral("mattermost-user:///") + username;
}

inline QString groupMentionHref(const QString& groupId)
{
    return QStringLiteral("mattermost-group:///") + groupId;
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

inline QTextCharFormat clickableMentionFormat(const QPalette& palette, const QString& href)
{
    QTextCharFormat format;
    format.setAnchor(true);
    format.setAnchorHref(href);
    format.setForeground(palette.color(QPalette::Link));
    format.setFontUnderline(false);
    return format;
}

inline QTextCharFormat specialMentionFormat(const QPalette& palette)
{
    QTextCharFormat format;
    QColor background = palette.color(QPalette::Highlight);
    background.setAlphaF(std::min<qreal>(background.alphaF(), 0.45));
    format.setBackground(background);
    format.setForeground(palette.color(QPalette::Text));
    format.setFontUnderline(false);
    format.setAnchor(false);
    return format;
}

inline void linkify(QTextDocument& document,
                    const QHash<QString, QString>& groupMentionIds = {})
{
    enum class Kind { User, Group, Special };
    struct Replacement {
        int position = 0;
        int length = 0;
        QString value;
        Kind kind = Kind::User;
    };

    QList<Replacement> replacements;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        QRegularExpressionMatchIterator matches = mentionExpression().globalMatch(block.text());
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const QString name = match.captured(1);
            const int position = block.position() + static_cast<int>(match.capturedStart(0));
            const int length = static_cast<int>(match.capturedLength(0));
            if (rangeAlreadyLinkedOrCode(document, position, length)) {
                continue;
            }

            if (isSpecialMention(name)) {
                replacements.push_back({position, length, {}, Kind::Special});
                continue;
            }
            const auto groupIt = groupMentionIds.constFind(name.toLower());
            if (groupIt != groupMentionIds.cend()) {
                replacements.push_back({position, length, groupIt.value(), Kind::Group});
                continue;
            }
            replacements.push_back({position, length, name, Kind::User});
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
        switch (replacement.kind) {
        case Kind::Special:
            cursor.mergeCharFormat(specialMentionFormat(palette));
            break;
        case Kind::Group:
            cursor.mergeCharFormat(clickableMentionFormat(
                palette, groupMentionHref(replacement.value)));
            break;
        case Kind::User:
            cursor.mergeCharFormat(clickableMentionFormat(
                palette, userMentionHref(replacement.value)));
            break;
        }
    }
}

inline QString linkifyHtmlTextSegment(
    const QString& text,
    const QHash<QString, QString>& groupMentionIds = {})
{
    QString result;
    int position = 0;
    QRegularExpressionMatchIterator matches = mentionExpression().globalMatch(text);
    while (matches.hasNext()) {
        const QRegularExpressionMatch match = matches.next();
        const QString name = match.captured(1);
        if (isSpecialMention(name)) {
            continue;
        }

        result += text.mid(position, static_cast<int>(match.capturedStart(0)) - position);
        const QString mention = match.captured(0);
        const auto groupIt = groupMentionIds.constFind(name.toLower());
        const QString href = groupIt == groupMentionIds.cend()
            ? userMentionHref(name) : groupMentionHref(groupIt.value());
        result += QStringLiteral("<a href=\"") + href
            + QStringLiteral("\" style=\"text-decoration:none\">") + mention
            + QStringLiteral("</a>");
        position = static_cast<int>(match.capturedEnd(0));
    }
    result += text.mid(position);
    return result;
}

inline QString linkifyHtml(const QString& html,
                           const QHash<QString, QString>& groupMentionIds = {})
{
    QString result;
    result.reserve(html.size() + 32);

    int position = 0;
    int anchorDepth = 0;
    while (position < html.size()) {
        const int tagStart = html.indexOf(QLatin1Char('<'), position);
        const int textEnd = tagStart < 0 ? html.size() : tagStart;
        const QString text = html.mid(position, textEnd - position);
        result += anchorDepth == 0 ? linkifyHtmlTextSegment(text, groupMentionIds) : text;

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
        if (lower.startsWith(QStringLiteral("<a ")) || lower == QStringLiteral("<a>")) {
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
