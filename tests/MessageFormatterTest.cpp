#include <QtTest>

#include <QAbstractTextDocumentLayout>
#include <QFontMetrics>
#include <QImage>
#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextLayout>

#include "backend/emoji/EmojiInfo.h"
#include "chat-area/post/MessageFormatter.h"

using namespace Mattermost;

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
namespace {

bool blockHasText(const QTextBlock& block)
{
    for (const QChar character: block.text()) {
        if (!character.isSpace() && character != QChar::ObjectReplacementCharacter) {
            return true;
        }
    }
    return false;
}

bool blockHasImage(const QTextBlock& block)
{
    for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
        const QTextFragment fragment = it.fragment();
        if (fragment.isValid() && fragment.charFormat().isImageFormat()) {
            return true;
        }
    }
    return false;
}

bool hasMixedTextAndImageBlock(const QTextDocument& document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (blockHasText(block) && blockHasImage(block)) {
            return true;
        }
    }
    return false;
}

int imageBlockCount(const QTextDocument& document)
{
    int count = 0;
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (blockHasImage(block)) {
            ++count;
        }
    }
    return count;
}

QString renderedPlainText(const QString& source)
{
    QTextDocument rendered;
    rendered.setHtml(MessageFormatter::formatMessageText(source));
    return rendered.toPlainText();
}

QString firstAnchorHref(const QTextDocument& document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isAnchor()) {
                return fragment.charFormat().anchorHref();
            }
        }
    }
    return {};
}

QTextBlock firstTextBlock(const QTextDocument& document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (blockHasText(block)) {
            return block;
        }
    }
    return {};
}

QTextBlock firstImageBlock(const QTextDocument& document)
{
    for (QTextBlock block = document.begin(); block.isValid(); block = block.next()) {
        if (blockHasImage(block)) {
            return block;
        }
    }
    return {};
}

} // namespace
#endif

class MessageFormatterTest : public QObject
{
    Q_OBJECT

private slots:
    void multilineSingleBacktickCodeBecomesPreformattedBlock()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "`{\n"
            "  \"name\": \"ContextOverflowError\",\n"
            "  \"data\": {\n"
            "    \"message\": \"Compaction exhausted: context still exceeds model limits after 3 attempts\"\n"
            "  }\n"
            "}`");

        const QString html = MessageFormatter::formatMessageText(source);
        QVERIFY2(html.contains(QStringLiteral("<pre"), Qt::CaseInsensitive), qPrintable(html));

        QTextDocument rendered;
        rendered.setHtml(html);
        const QString plain = rendered.toPlainText();
        QVERIFY2(plain.contains(QStringLiteral("{\n  \"name\": \"ContextOverflowError\",\n  \"data\": {")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("\n    \"message\": \"Compaction exhausted")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("&quot;")), qPrintable(plain));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void singleLineInlineCodeStaysInline()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(QStringLiteral("before `foo()` after"));
        QVERIFY2(!html.contains(QStringLiteral("<pre"), Qt::CaseInsensitive), qPrintable(html));
        QCOMPARE(renderedPlainText(QStringLiteral("before `foo()` after")), QStringLiteral("before foo() after"));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void fencedCodePreservesLineBreaksQuotesAndAmpersands()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "```json\n"
            "{\n"
            "  \"quoted\": \"value\",\n"
            "  \"entity\": \"A & B\"\n"
            "}\n"
            "```");

        const QString html = MessageFormatter::formatMessageText(source);
        QVERIFY2(html.contains(QStringLiteral("<pre"), Qt::CaseInsensitive), qPrintable(html));

        QTextDocument rendered;
        rendered.setHtml(html);
        const QString plain = rendered.toPlainText();
        QVERIFY2(plain.contains(QStringLiteral("{\n  \"quoted\": \"value\",\n  \"entity\": \"A & B\"\n}")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("&quot;")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("&amp;")), qPrintable(plain));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void rawHtmlIsNotInterpreted()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString html = MessageFormatter::formatMessageText(
            QStringLiteral("before <b>bold</b> <script>alert('x')</script> after"));

        QVERIFY(!html.contains(QStringLiteral("<script"), Qt::CaseInsensitive));
        QVERIFY(!html.contains(QStringLiteral("<b>bold</b>"), Qt::CaseInsensitive));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void bareUrlIsClickable()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, QStringLiteral("see https://example.com/path?q=1"));
        QCOMPARE(firstAnchorHref(document), QStringLiteral("https://example.com/path?q=1"));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void longPercentEncodedBareUrlIsClickable()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString url = QStringLiteral(
            "https://c.yadro.com/spaces/TELS/pages/1451865554/"
            "CR429767+review+-+YA224+HWitem+FW+slot+"
            "%D0%BD%D0%B5+%D0%BE%D0%B1%D0%BD%D0%BE%D0%B2%D0%B8%D0%BB+"
            "%D0%B8%D0%BD%D1%84%D0%BE%D1%80%D0%BC%D0%B0%D1%86%D0%B8%D1%8E+"
            "%D0%BE+%D0%BD%D0%B5%D0%B2%D0%B0%D0%BB%D0%B8%D0%B4%D0%BD%D0%BE%D1%81%D1%82%D0%B8+"
            "%D0%BF%D0%BE%D1%81%D0%BB%D0%B5+"
            "%D0%BD%D0%B5%D1%83%D1%81%D0%BF%D0%B5%D1%88%D0%BD%D0%BE%D0%B9+"
            "%D0%B8%D0%BD%D1%81%D1%82%D0%B0%D0%BB%D0%BB%D1%8F%D1%86%D0%B8%D0%B8+04.08.2026");

        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, url);
        QCOMPARE(firstAnchorHref(document), url);
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void inlineCodeUrlIsNotLinkified()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(
            document, QStringLiteral("`https://example.com/not-a-link`"));
        QVERIFY(firstAnchorHref(document).isEmpty());
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void markdownLinkIsClickable()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, QStringLiteral("[example](https://example.com/path)"));
        QCOMPARE(firstAnchorHref(document), QStringLiteral("https://example.com/path"));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void unicodeEmojiAliasIsExpanded()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, QStringLiteral("hello :wave: world"));
        QVERIFY(!document.toPlainText().contains(QStringLiteral(":wave:")));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void customEmojiStaysInline()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString name = QStringLiteral("message_formatter_test_custom");
        EmojiInfo::addCustomEmoji(name, QStringLiteral("/tmp/custom-emoji/message-formatter-test.gif"));

        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, QStringLiteral("before :") + name + QStringLiteral(": after"));

        QCOMPARE(document.blockCount(), 1);
        QVERIFY(blockHasText(document.firstBlock()));
        QVERIFY(blockHasImage(document.firstBlock()));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void largeMarkdownImageGetsOwnParagraph()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "before ![large image](https://example.com/large.png) after");

        QTextDocument document;
        MessageFormatter::buildMarkdownDocument(document, source);
        QVERIFY(!hasMixedTextAndImageBlock(document));
        QCOMPARE(imageBlockCount(document), 1);

        // Rich message fragments are serialized to HTML before being shown in
        // the wrapped text child. Verify that image separation survives it.
        QTextDocument rendered;
        rendered.setHtml(MessageFormatter::formatMessageText(source));
        QVERIFY(!hasMixedTextAndImageBlock(rendered));
        QCOMPARE(imageBlockCount(rendered), 1);
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void largeMarkdownImageAfterTextGetsOwnParagraph()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "last line of text ![large image](https://example.com/large.png)");

        QTextDocument rendered;
        rendered.setHtml(MessageFormatter::formatMessageText(source));
        QVERIFY(!hasMixedTextAndImageBlock(rendered));
        QCOMPARE(imageBlockCount(rendered), 1);
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void largeMarkdownImageDoesNotInflateTextLine()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QUrl imageUrl(QStringLiteral("https://example.com/large.png"));
        const QString source = QStringLiteral("last line of text ![large image](https://example.com/large.png)");

        QTextDocument rendered;
        rendered.setHtml(MessageFormatter::formatMessageText(source));
        rendered.addResource(QTextDocument::ImageResource, imageUrl, QImage(320, 240, QImage::Format_ARGB32));
        rendered.setTextWidth(640);
        rendered.documentLayout()->documentSize();

        const QTextBlock textBlock = firstTextBlock(rendered);
        const QTextBlock imageBlock = firstImageBlock(rendered);
        QVERIFY(textBlock.isValid());
        QVERIFY(imageBlock.isValid());
        QVERIFY(textBlock != imageBlock);
        QCOMPARE(textBlock.blockFormat().topMargin(), 0.0);
        QCOMPARE(textBlock.blockFormat().bottomMargin(), 0.0);
        QCOMPARE(imageBlock.blockFormat().topMargin(), 0.0);
        QCOMPARE(imageBlock.blockFormat().bottomMargin(), 0.0);

        const QTextLayout* textLayout = textBlock.layout();
        QVERIFY(textLayout != nullptr);
        QVERIFY(textLayout->lineCount() > 0);

        const qreal normalLineHeight = QFontMetrics(rendered.defaultFont()).height();
        const qreal actualLineHeight = textLayout->lineAt(0).height();
        QVERIFY2(actualLineHeight <= normalLineHeight * 1.5,
                 qPrintable(QStringLiteral("text line height %1, normal %2").arg(actualLineHeight).arg(normalLineHeight)));

        const QRectF textRect = rendered.documentLayout()->blockBoundingRect(textBlock);
        const QRectF imageRect = rendered.documentLayout()->blockBoundingRect(imageBlock);
        const qreal gap = imageRect.top() - textRect.bottom();
        QVERIFY2(gap <= normalLineHeight,
                 qPrintable(QStringLiteral("unexpected text/image gap %1, line height %2").arg(gap).arg(normalLineHeight)));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }
};

QTEST_MAIN(MessageFormatterTest)

#include "MessageFormatterTest.moc"