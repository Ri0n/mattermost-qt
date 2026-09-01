#include "MessageFormatter.h"

#include <algorithm>

#include <QTextBlock>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFormat>

#include "backend/emoji/EmojiInfo.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include <QRegularExpression>
#include <QTextFragment>
#endif

namespace Mattermost {
namespace MessageFormatter {

static void replaceEmojis(QString& text)
{
    int emojiStart = 0;
    int emojiEnd = 0;

    do {
        emojiStart = text.indexOf(':', emojiEnd);
        if (emojiStart == -1) {
            break;
        }

        emojiEnd = text.indexOf(':', emojiStart + 1);
        if (emojiEnd == -1) {
            break;
        }

        if (emojiEnd - emojiStart == 1) {
            ++emojiEnd;
            continue;
        }

        const int emojiNameSize = emojiEnd - emojiStart - 1;
        const QString emojiName = text.mid(emojiStart + 1, emojiNameSize);
        const EmojiID emojiID = EmojiInfo::findByName(emojiName);

        if (!emojiID) {
            ++emojiEnd;
            continue;
        }

        const Emoji emoji = EmojiInfo::getEmoji(emojiID);
        text.replace(emojiStart, emojiNameSize + 2, emoji.unicodeString);

        emojiEnd = emojiStart + emoji.unicodeString.size();
    } while (emojiStart != -1);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
namespace {

struct EmojiReplacement {
    int position = 0;
    int length = 0;
    Emoji emoji;
};

bool customEmojiImageFormat(const Emoji& emoji, QTextImageFormat& imageFormat)
{
    if (!emoji.unicodeString.contains(QStringLiteral("<img"))) {
        return false;
    }

    static const QRegularExpression imageExpression(
        QStringLiteral(R"(<img\s+src=["']([^"']+)["']\s+width=(\d+)\s+height=(\d+)\s*/?>)"),
        QRegularExpression::CaseInsensitiveOption);

    const QRegularExpressionMatch match = imageExpression.match(emoji.unicodeString);
    if (!match.hasMatch()) {
        return false;
    }

    imageFormat.setName(match.captured(1));
    imageFormat.setWidth(match.captured(2).toInt());
    imageFormat.setHeight(match.captured(3).toInt());
    return true;
}

void replaceEmojisInDocument(QTextDocument& document)
{
    static const QRegularExpression emojiExpression(QStringLiteral(R"(:([^:\s]+):)"));
    QList<EmojiReplacement> replacements;

    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        const QString blockText = block.text();
        QRegularExpressionMatchIterator matches = emojiExpression.globalMatch(blockText);
        while (matches.hasNext()) {
            const QRegularExpressionMatch match = matches.next();
            const EmojiID emojiID = EmojiInfo::findByName(match.captured(1));
            if (!emojiID) {
                continue;
            }

            replacements.push_back(EmojiReplacement {
                block.position() + match.capturedStart(0),
                match.capturedLength(0),
                EmojiInfo::getEmoji(emojiID),
            });
        }
    }

    std::sort(replacements.begin(), replacements.end(), [](const EmojiReplacement& left, const EmojiReplacement& right) {
        return left.position > right.position;
    });

    for (const EmojiReplacement& replacement: replacements) {
        QTextCursor cursor(&document);
        cursor.setPosition(replacement.position);
        const QTextCharFormat textFormat = cursor.charFormat();
        cursor.setPosition(replacement.position + replacement.length, QTextCursor::KeepAnchor);

        QTextImageFormat imageFormat;
        if (customEmojiImageFormat(replacement.emoji, imageFormat)) {
            cursor.removeSelectedText();
            cursor.insertImage(imageFormat);
        } else {
            cursor.insertText(replacement.emoji.unicodeString, textFormat);
        }
    }
}

bool hasNonImageText(const QString& text)
{
    for (const QChar character: text) {
        if (!character.isSpace() && character != QChar::ObjectReplacementCharacter) {
            return true;
        }
    }
    return false;
}

bool shouldRenderImageAsBlock(const QTextImageFormat& imageFormat)
{
    constexpr qreal maxInlineImageSize = 64.0;

    const qreal width = imageFormat.width();
    const qreal height = imageFormat.height();
    if (width > maxInlineImageSize || height > maxInlineImageSize) {
        return true;
    }

    // Markdown images generally have no explicit dimensions. They are content
    // images, not emoji, and must not share a QTextLine with message text.
    return width <= 0.0 && height <= 0.0;
}

void clearBlockMargins(const QTextBlock& block)
{
    if (!block.isValid()) {
        return;
    }

    QTextCursor cursor(block);
    QTextBlockFormat format = block.blockFormat();
    format.setTopMargin(0);
    format.setBottomMargin(0);
    cursor.setBlockFormat(format);
}

void separateLargeImages(QTextDocument& document)
{
    // Split one mixed text/image block at a time and restart after each edit,
    // because QTextFragment positions are invalidated by insertBlock().
    for (;;) {
        bool changed = false;

        for (QTextBlock block = document.begin(); block.isValid() && !changed; block = block.next()) {
            const QString blockText = block.text();

            for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
                const QTextFragment fragment = it.fragment();
                if (!fragment.isValid() || !fragment.charFormat().isImageFormat()) {
                    continue;
                }

                const QTextImageFormat imageFormat = fragment.charFormat().toImageFormat();
                if (!shouldRenderImageAsBlock(imageFormat)) {
                    continue;
                }

                const int offset = fragment.position() - block.position();
                const bool hasTextBefore = hasNonImageText(blockText.left(offset));
                const bool hasTextAfter = hasNonImageText(blockText.mid(offset + fragment.length()));

                if (!hasTextBefore && !hasTextAfter) {
                    clearBlockMargins(block);
                    continue;
                }

                if (hasTextAfter) {
                    QTextCursor cursor(&document);
                    cursor.setPosition(fragment.position() + fragment.length());
                    cursor.insertBlock();
                }

                if (hasTextBefore) {
                    QTextCursor cursor(&document);
                    cursor.setPosition(fragment.position());
                    cursor.insertBlock();
                }

                changed = true;
                break;
            }
        }

        if (!changed) {
            break;
        }
    }

    // QLabel parses the generated HTML into another QTextDocument. Explicitly
    // zero margins on every block so the Markdown -> HTML -> RichText roundtrip
    // cannot reintroduce a large gap around image-only paragraphs.
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        clearBlockMargins(block);
    }
}

} // namespace

void buildMarkdownDocument(QTextDocument& document, const QString& text)
{
    document.clear();
    document.setDocumentMargin(0);

    // Parse the original Markdown verbatim. In particular, do not HTML-escape
    // quotes or ampersands before parsing: entities inside code spans are not
    // decoded by CommonMark, which used to turn a literal `"` into &quot;.
    // Raw HTML is disabled at the parser level instead.
    const QTextDocument::MarkdownFeatures features =
        QTextDocument::MarkdownDialectGitHub | QTextDocument::MarkdownNoHTML;
    document.setMarkdown(text, features);

    // Emoji are applied after Markdown parsing. Custom emoji are inserted as
    // QTextImageFormat objects, so enabling raw user HTML is unnecessary.
    replaceEmojisInDocument(document);
    separateLargeImages(document);
}
#endif

QString formatMessageText(const QString& text)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    QTextDocument document;
    buildMarkdownDocument(document, text);
    return document.toHtml();
#else
    QString result(text.toHtmlEscaped());
    result.replace("\n", "<br>");

    int linkStart = 0;
    int linkEnd = 0;

    replaceEmojis(result);

    do {
        QLatin1String lookups[2] = { QLatin1String("http://"), QLatin1String("https://") };
        QLatin1String* useLookup = nullptr;

        for (auto& lookup: lookups) {
            linkStart = result.indexOf(lookup, linkEnd);
            if (linkStart != -1) {
                useLookup = &lookup;
                break;
            }
        }

        if (!useLookup) {
            break;
        }

        for (linkEnd = linkStart + useLookup->size(); linkEnd < result.size(); ++linkEnd) {
            if (result.at(linkEnd) == ' ' || result.at(linkEnd) == '<') {
                break;
            }
        }

        const int size = linkEnd - linkStart;
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
        result.insert(linkEnd, "\">" + QStringRef(&result, linkStart, size) + "</a>");
#else
        const QStringView stringView(result);
        result.insert(linkEnd, "\">" + stringView.sliced(linkStart, size).toString() + "</a>");
#endif
        result.insert(linkStart, "<a href=\"");

        linkEnd += size + 15;
    } while (linkStart != -1);

    return result;
#endif
}

} // namespace MessageFormatter
} // namespace Mattermost
