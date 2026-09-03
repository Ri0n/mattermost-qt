#include <QtTest>

#include <QAbstractTextDocumentLayout>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextLayout>
#include <QTextOption>

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
        QVERIFY2(richText->styleSheet().contains(QStringLiteral("background: transparent")),
                 "The rich-text control itself must stay transparent over the post hover background");
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

        QTextCursor emojiCursor(richText->document());
        emojiCursor.setPosition(emojiPosition + fire.size());
        const qreal emojiSize = emojiCursor.charFormat().fontPointSize();

        QVERIFY(textSize > 0.0);
        QVERIFY2(emojiSize > textSize * 1.25 && emojiSize < textSize * 1.35,
                 "Inline Unicode emoji should render at approximately 1.3x the surrounding text size");
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
