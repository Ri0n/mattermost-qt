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

#if QT_VERSION < QT_VERSION_CHECK(6, 10, 0)
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
#endif

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
namespace {

struct EmojiReplacement {
    int position = 0;
    int length = 0;
    Emoji emoji;
};

bool isEscaped(const QString& text, int position)
{
    int backslashCount = 0;
    for (int i = position - 1; i >= 0 && text.at(i) == QLatin1Char('\\'); --i) {
        ++backslashCount;
    }
    return (backslashCount % 2) != 0;
}

int backtickRunLength(const QString& text, int position)
{
    int length = 0;
    while (position + length < text.size() && text.at(position + length) == QLatin1Char('`')) {
        ++length;
    }
    return length;
}

int longestBacktickRun(const QString& text)
{
    int longest = 0;
    for (int i = 0; i < text.size();) {
        if (text.at(i) != QLatin1Char('`')) {
            ++i;
            continue;
        }
        const int length = backtickRunLength(text, i);
        longest = std::max(longest, length);
        i += length;
    }
    return longest;
}

int fencedBlockEnd(const QString& text, int lineStart)
{
    if (lineStart != 0 && text.at(lineStart - 1) != QLatin1Char('\n')) {
        return -1;
    }

    int fenceStart = lineStart;
    int leadingSpaces = 0;
    while (fenceStart < text.size() && leadingSpaces < 3 && text.at(fenceStart) == QLatin1Char(' ')) {
        ++fenceStart;
        ++leadingSpaces;
    }
    if (fenceStart >= text.size()) {
        return -1;
    }

    const QChar fenceCharacter = text.at(fenceStart);
    if (fenceCharacter != QLatin1Char('`') && fenceCharacter != QLatin1Char('~')) {
        return -1;
    }

    int fenceLength = 0;
    while (fenceStart + fenceLength < text.size()
           && text.at(fenceStart + fenceLength) == fenceCharacter) {
        ++fenceLength;
    }
    if (fenceLength < 3) {
        return -1;
    }

    int nextLineStart = text.indexOf(QLatin1Char('\n'), fenceStart + fenceLength);
    if (nextLineStart == -1) {
        return text.size();
    }
    ++nextLineStart;

    while (nextLineStart < text.size()) {
        int candidate = nextLineStart;
        int closingLeadingSpaces = 0;
        while (candidate < text.size() && closingLeadingSpaces < 3
               && text.at(candidate) == QLatin1Char(' ')) {
            ++candidate;
            ++closingLeadingSpaces;
        }

        int closingLength = 0;
        while (candidate + closingLength < text.size()
               && text.at(candidate + closingLength) == fenceCharacter) {
            ++closingLength;
        }

        if (closingLength >= fenceLength) {
            const int lineEnd = text.indexOf(QLatin1Char('\n'), candidate + closingLength);
            const int contentEnd = lineEnd == -1 ? text.size() : lineEnd;
            bool onlyWhitespaceAfterFence = true;
            for (int i = candidate + closingLength; i < contentEnd; ++i) {
                if (text.at(i) != QLatin1Char(' ') && text.at(i) != QLatin1Char('\t')) {
                    onlyWhitespaceAfterFence = false;
                    break;
                }
            }
            if (onlyWhitespaceAfterFence) {
                return lineEnd == -1 ? text.size() : lineEnd + 1;
            }
        }

        const int lineEnd = text.indexOf(QLatin1Char('\n'), nextLineStart);
        if (lineEnd == -1) {
            break;
        }
        nextLineStart = lineEnd + 1;
    }

    // An unclosed Markdown fence owns the rest of the document. Keep it
    // untouched instead of trying to reinterpret backticks inside its body.
    return text.size();
}

QString promoteMultilineCodeSpans(const QString& text)
{
    QString result;
    result.reserve(text.size());

    int position = 0;
    while (position < text.size()) {
        if (position == 0 || text.at(position - 1) == QLatin1Char('\n')) {
            const int fenceEnd = fencedBlockEnd(text, position);
            if (fenceEnd != -1) {
                result += text.mid(position, fenceEnd - position);
                position = fenceEnd;
                continue;
            }
        }

        if (text.at(position) != QLatin1Char('`') || isEscaped(text, position)) {
            result += text.at(position);
            ++position;
            continue;
        }

        const int delimiterLength = backtickRunLength(text, position);
        if (delimiterLength > 2) {
            result += text.mid(position, delimiterLength);
            position += delimiterLength;
            continue;
        }

        int closingPosition = position + delimiterLength;
        while (closingPosition < text.size()) {
            if (text.at(closingPosition) != QLatin1Char('`')) {
                ++closingPosition;
                continue;
            }

            const int closingLength = backtickRunLength(text, closingPosition);
            if (closingLength == delimiterLength && !isEscaped(text, closingPosition)) {
                break;
            }
            closingPosition += closingLength;
        }

        if (closingPosition >= text.size()) {
            result += text.mid(position, delimiterLength);
            position += delimiterLength;
            continue;
        }

        const int contentStart = position + delimiterLength;
        const QString content = text.mid(contentStart, closingPosition - contentStart);
        if (!content.contains(QLatin1Char('\n'))) {
            result += text.mid(position, closingPosition + delimiterLength - position);
            position = closingPosition + delimiterLength;
            continue;
        }

        // CommonMark intentionally collapses whitespace inside multiline code
        // spans. Mattermost messages in the wild also contain multiline snippets
        // wrapped in one or two backticks, so promote those spans to a fenced
        // code block before handing the text to QTextDocument's Markdown parser.
        const int fenceLength = std::max(3, longestBacktickRun(content) + 1);
        const QString fence(fenceLength, QLatin1Char('`'));
        const bool startsAtLineStart = position == 0 || text.at(position - 1) == QLatin1Char('\n');
        const int afterClosing = closingPosition + delimiterLength;
        const bool endsAtLineEnd = afterClosing == text.size() || text.at(afterClosing) == QLatin1Char('\n');

        if (!startsAtLineStart) {
            if (!result.endsWith(QLatin1Char('\n'))) {
                result += QLatin1Char('\n');
            }
            result += QLatin1Char('\n');
        }

        result += fence;
        result += QLatin1Char('\n');
        result += content;
        if (!content.endsWith(QLatin1Char('\n'))) {
            result += QLatin1Char('\n');
        }
        result += fence;

        if (!endsAtLineEnd) {
            result += QStringLiteral("\n\n");
        }

        position = afterClosing;
    }

    return result;
}

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
                block.position() + static_cast<int>(match.capturedStart(0)),
                static_cast<int>(match.capturedLength(0)),
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
    QTextDocument::MarkdownFeatures features(QTextDocument::MarkdownDialectGitHub);
    features.setFlag(QTextDocument::MarkdownNoHTML);
    document.setMarkdown(promoteMultilineCodeSpans(text), features);

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
