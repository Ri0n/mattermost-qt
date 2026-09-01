#include <QtTest>

#include <QAbstractTextDocumentLayout>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
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
