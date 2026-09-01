#include <QtTest>

#include <QTextBlock>
#include <QTextDocument>
#include <QTextFragment>

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

} // namespace
#endif

class MessageFormatterTest : public QObject
{
    Q_OBJECT

private slots:
    void inlineCodePreservesQuotes()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "`{\n"
            "  \"name\": \"ContextOverflowError\",\n"
            "  \"data\": {\n"
            "    \"message\": \"Compaction exhausted: context still exceeds model limits after 3 attempts\"\n"
            "  }\n"
            "}`");

        const QString plain = renderedPlainText(source);
        QVERIFY2(plain.contains(QStringLiteral("\"name\": \"ContextOverflowError\"")), qPrintable(plain));
        QVERIFY2(plain.contains(QStringLiteral("\"message\": \"Compaction exhausted")), qPrintable(plain));
        QVERIFY2(!plain.contains(QStringLiteral("&quot;")), qPrintable(plain));
#else
        QSKIP("Qt Markdown renderer is enabled starting with Qt 6.10");
#endif
    }

    void fencedCodePreservesQuotesAndAmpersands()
    {
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
        const QString source = QStringLiteral(
            "```json\n"
            "{\"quoted\": \"value\", \"entity\": \"A & B\"}\n"
            "```");

        const QString plain = renderedPlainText(source);
        QVERIFY2(plain.contains(QStringLiteral("{\"quoted\": \"value\", \"entity\": \"A & B\"}")), qPrintable(plain));
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

        // PostWidget passes HTML to QLabel, which parses it into another
        // QTextDocument. Verify that the separation survives that roundtrip.
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
};

QTEST_APPLESS_MAIN(MessageFormatterTest)

#include "MessageFormatterTest.moc"
