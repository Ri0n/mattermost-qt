#include <QtTest>

#include <QAbstractTextDocumentLayout>
#include <QFontMetrics>
#include <QImage>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTemporaryDir>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextFragment>
#include <QTextImageFormat>
#include <QTextLayout>
#include <QTextOption>

#include "backend/emoji/EmojiInfo.h"
#include "chat-area/post/MessageContentWidget.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include "qsourcehighliter.h"
#endif

using namespace Mattermost;

namespace {

int renderedLineCount(QTextBrowser& browser, int width)
{
    browser.resize(width, 100);
    browser.document()->setTextWidth(width);
    browser.document()->documentLayout()->documentSize();

    int count = 0;
    for (QTextBlock block = browser.document()->begin(); block.isValid(); block = block.next()) {
        if (const QTextLayout* layout = block.layout()) {
            count += layout->lineCount();
        }
    }
    return count;
}

void showAndSettle(QWidget& widget, const QSize& size = QSize(240, 200))
{
    widget.resize(size);
    widget.show();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
    QCoreApplication::processEvents();
}

QTextImageFormat firstImageFormat(const QTextBrowser& browser)
{
    for (QTextBlock block = browser.document()->begin(); block.isValid(); block = block.next()) {
        for (QTextBlock::iterator it = block.begin(); !it.atEnd(); ++it) {
            const QTextFragment fragment = it.fragment();
            if (fragment.isValid() && fragment.charFormat().isImageFormat()) {
                return fragment.charFormat().toImageFormat();
            }
        }
    }
    return {};
}

qreal firstImageWidth(const QTextBrowser& browser)
{
    return firstImageFormat(browser).width();
}

qreal emojiPointSize(const QTextBrowser& browser, const QString& emoji)
{
    const int position = browser.document()->toPlainText().indexOf(emoji);
    if (position < 0) {
        return -1.0;
    }

    QTextCursor cursor(browser.document());
    cursor.setPosition(position + emoji.size());
    qreal size = cursor.charFormat().fontPointSize();
    if (size <= 0.0) {
        size = browser.document()->defaultFont().pointSizeF();
    }
    return size;
}

} // namespace

class MessageContentWidgetTest : public QObject
{
    Q_OBJECT

private slots:
    void longPlainTokenWrapsAnywhere()
    {
        MessageContentWidget widget;
        widget.setMessage(QString(1000, QLatin1Char('x')));
        showAndSettle(widget);

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        QCOMPARE(richText->document()->defaultTextOption().wrapMode(),
                 QTextOption::WrapAtWordBoundaryOrAnywhere);
        QVERIFY2(renderedLineCount(*richText, 120) > 1,
                 "A long unbroken normal-text token must wrap inside the message");
    }

    void wrappedTextReportsSettledHeight()
    {
        MessageContentWidget widget;
        QSignalSpy geometrySpy(&widget, &MessageContentWidget::dimensionsChanged);
        widget.setMessage(QString(1000, QLatin1Char('x')));
        showAndSettle(widget, QSize(120, 200));

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        QVERIFY2(richText->height() > 2 * richText->fontMetrics().height(),
                 "Wrapped text must expand the real message widget height");
        QVERIFY2(geometrySpy.count() > 0,
                 "A settled text reflow must notify the containing post about its new geometry");
    }

    void richTextBackgroundLetsPostHoverShowThrough()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("hover me"));
        showAndSettle(widget);

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        QVERIFY2(!richText->viewport()->autoFillBackground(),
                 "The rich-text viewport must not cover the containing post hover background");
        QVERIFY2(richText->styleSheet().isEmpty(),
                 "Rich text transparency must not rely on a per-widget style sheet");
    }

    void inlineUnicodeEmojiUsesLargerFont()
    {
        const QString fire = QString::fromUtf8("\xF0\x9F\x94\xA5");

        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("A ") + fire + QStringLiteral(" B"));
        showAndSettle(widget);

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);

        const QString plainText = richText->document()->toPlainText();
        const int emojiPosition = plainText.indexOf(fire);
        QVERIFY(emojiPosition >= 0);

        QTextCursor textCursor(richText->document());
        textCursor.setPosition(1);
        qreal textSize = textCursor.charFormat().fontPointSize();
        if (textSize <= 0.0) {
            textSize = richText->document()->defaultFont().pointSizeF();
        }

        const qreal emojiSize = emojiPointSize(*richText, fire);

        QVERIFY(textSize > 0.0);
        QVERIFY2(emojiSize > textSize * 1.25 && emojiSize < textSize * 1.35,
                 "Inline Unicode emoji should render at approximately 1.3x the surrounding text size");
    }

    void inlineCustomEmojiUsesFontRelativeSizeAndMiddleAlignment()
    {
        const QString name = QStringLiteral("mattermost_qt_inline_custom_test");
        EmojiInfo::addCustomEmoji(name, QStringLiteral("/nonexistent/mattermost-qt-inline-test.png"));

        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("A :") + name + QStringLiteral(": B"));
        showAndSettle(widget);

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);

        const QTextImageFormat imageFormat = firstImageFormat(*richText);
        QVERIFY(imageFormat.isValid());
        const int expected = qRound(
            QFontMetrics(richText->document()->defaultFont()).ascent() * 1.3);
        QVERIFY2(std::abs(imageFormat.height() - expected) <= 1.0,
                 qPrintable(QStringLiteral("inline custom emoji height %1, expected %2")
                     .arg(imageFormat.height()).arg(expected)));
        QCOMPARE(imageFormat.verticalAlignment(), QTextCharFormat::AlignMiddle);
    }

    void emojiOnlyUnicodeUsesJumboFont()
    {
        const QString fire = QString::fromUtf8("\xF0\x9F\x94\xA5");

        MessageContentWidget inlineWidget;
        inlineWidget.setMessage(QStringLiteral("A ") + fire + QStringLiteral(" B"));
        showAndSettle(inlineWidget);
        auto* inlineText = inlineWidget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(inlineText != nullptr);

        MessageContentWidget jumboWidget;
        jumboWidget.setMessage(QStringLiteral("  ") + fire + QStringLiteral("  "));
        showAndSettle(jumboWidget);
        auto* jumboText = jumboWidget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(jumboText != nullptr);

        const qreal inlineSize = emojiPointSize(*inlineText, fire);
        const qreal jumboSize = emojiPointSize(*jumboText, fire);
        QVERIFY(inlineSize > 0.0);
        QVERIFY2(jumboSize > inlineSize * 2.8,
                 "An emoji-only message should render its Unicode emoji at roughly 4em");
    }

    void emojiOnlyCustomEmojiUsesJumboImage()
    {
        const QString name = QStringLiteral("mattermost_qt_jumbo_test");
        EmojiInfo::addCustomEmoji(name, QStringLiteral("/nonexistent/mattermost-qt-jumbo-test.png"));

        MessageContentWidget inlineWidget;
        inlineWidget.setMessage(QStringLiteral("A :") + name + QStringLiteral(": B"));
        showAndSettle(inlineWidget);
        auto* inlineText = inlineWidget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(inlineText != nullptr);

        MessageContentWidget jumboWidget;
        jumboWidget.setMessage(QStringLiteral("  :") + name + QStringLiteral(":  "));
        showAndSettle(jumboWidget);
        auto* jumboText = jumboWidget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(jumboText != nullptr);

        const qreal inlineWidth = firstImageWidth(*inlineText);
        const QTextImageFormat jumboFormat = firstImageFormat(*jumboText);
        QVERIFY(inlineWidth > 0.0);
        QVERIFY2(jumboFormat.width() > inlineWidth * 2.8,
                 "An emoji-only custom emoji should render at roughly 4em too");
        QCOMPARE(jumboFormat.verticalAlignment(), QTextCharFormat::AlignMiddle);
    }

    void jumboCustomEmojiDoesNotUpscalePastNativeBitmap()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("small-custom.png"));
        QImage image(18, 12, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QVERIFY(image.save(path));

        const QString name = QStringLiteral("mattermost_qt_native_cap_test");
        EmojiInfo::addCustomEmoji(name, path);

        MessageContentWidget widget;
        widget.setMessage(QLatin1Char(':') + name + QLatin1Char(':'));
        showAndSettle(widget);

        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        const QTextImageFormat imageFormat = firstImageFormat(*richText);
        QCOMPARE(qRound(imageFormat.width()), 18);
        QCOMPARE(qRound(imageFormat.height()), 12);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    void longInlineCodeWrapsAnywhere()
    {
        MessageContentWidget widget;
        const QString token(1000, QLatin1Char('a'));
        widget.setMessage(QLatin1Char('`') + token + QLatin1Char('`'));
        showAndSettle(widget);

        QVERIFY(widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock")) == nullptr);
        auto* richText = widget.findChild<QTextBrowser*>(QStringLiteral("messageRichText"));
        QVERIFY(richText != nullptr);
        QCOMPARE(richText->document()->defaultTextOption().wrapMode(),
                 QTextOption::WrapAtWordBoundaryOrAnywhere);
        QVERIFY2(renderedLineCount(*richText, 120) > 1,
                 "Inline code must wrap instead of widening the whole chat window");
    }

    void fencedCodeGetsOwnHorizontalScrollArea()
    {
        MessageContentWidget widget;
        const QString longLine = QStringLiteral("const char *value = \"")
            + QString(1000, QLatin1Char('x')) + QStringLiteral("\";");
        widget.setMessage(QStringLiteral("```cpp\n") + longLine + QStringLiteral("\n```"));
        showAndSettle(widget, QSize(220, 200));

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        QCOMPARE(codeBlock->lineWrapMode(), QPlainTextEdit::NoWrap);
        QCOMPARE(codeBlock->horizontalScrollBarPolicy(), Qt::ScrollBarAsNeeded);
        QCOMPARE(codeBlock->property("codeLanguage").toString(), QStringLiteral("cpp"));
        QCOMPARE(codeBlock->property("sourceHighliteLanguage").toInt(),
                 static_cast<int>(QSourceHighlite::QSourceHighliter::CodeCpp));

        codeBlock->resize(180, codeBlock->height());
        QCoreApplication::processEvents();
        QVERIFY2(codeBlock->horizontalScrollBar()->maximum() > 0,
                 "A long code line must scroll inside its own code block");
        QVERIFY2(widget.minimumSizeHint().width() < 180,
                 "Code content must not impose its unwrapped width on the parent message");
    }

    void multilineCodeKeepsAllLinesVisibleAboveScrollbar()
    {
        MessageContentWidget widget;
        const QString longLine(800, QLatin1Char('x'));
        widget.setMessage(QStringLiteral("```cpp\nline one\n") + longLine
                          + QStringLiteral("\nline three\nline four\nline five\n```"));
        showAndSettle(widget, QSize(220, 300));

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        codeBlock->resize(180, codeBlock->height());
        QCoreApplication::processEvents();
        QCoreApplication::processEvents();

        QCOMPARE(codeBlock->document()->blockCount(), 5);
        QVERIFY2(codeBlock->horizontalScrollBar()->maximum() > 0,
                 "The long code line must produce a local horizontal scrollbar");
        const int textHeight = codeBlock->document()->blockCount()
            * codeBlock->fontMetrics().lineSpacing();
        QVERIFY2(codeBlock->viewport()->height() >= textHeight,
                 "The code viewport must be tall enough to show every code line, not only the scrollbar");
    }

    void fencedJsonSelectsJsonHighlighter()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("```json\n{\"answer\": 42}\n```"));
        showAndSettle(widget);

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        QCOMPARE(codeBlock->property("sourceHighliteLanguage").toInt(),
                 static_cast<int>(QSourceHighlite::QSourceHighliter::CodeJSON));
    }

    void unknownLanguageStillGetsCodeWidget()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("```made-up-language\nabcdef\n```"));
        showAndSettle(widget);

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        QCOMPARE(codeBlock->property("codeLanguage").toString(), QStringLiteral("made-up-language"));
        QVERIFY(!codeBlock->property("sourceHighliteLanguage").isValid());
    }

    void promotedMultilineBackticksUseCodeWidget()
    {
        MessageContentWidget widget;
        widget.setMessage(QStringLiteral("`first line\nsecond line\nthird line`"));
        showAndSettle(widget);

        auto* codeBlock = widget.findChild<QPlainTextEdit*>(QStringLiteral("messageCodeBlock"));
        QVERIFY(codeBlock != nullptr);
        QCOMPARE(codeBlock->toPlainText(), QStringLiteral("first line\nsecond line\nthird line"));
    }
#endif
};

QTEST_MAIN(MessageContentWidgetTest)

#include "MessageContentWidgetTest.moc"
