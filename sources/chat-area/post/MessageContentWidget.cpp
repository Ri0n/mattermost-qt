#include "MessageContentWidget.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <QAbstractTextDocumentLayout>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QPalette>
#include <QPlainTextEdit>
#include <QResizeEvent>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBlockFormat>
#include <QTextBrowser>
#include <QTextCursor>
#include <QTextDocument>
#include <QTextDocumentFragment>
#include <QTextOption>
#include <QTimer>
#include <QVBoxLayout>

#include "MessageFormatter.h"

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
#include "qsourcehighliter.h"
#endif

namespace Mattermost {
namespace {

class WrappedRichText final : public QTextBrowser
{
public:
    explicit WrappedRichText(QWidget* parent = nullptr)
        : QTextBrowser(parent)
    {
        setObjectName(QStringLiteral("messageRichText"));
        setReadOnly(true);
        setOpenExternalLinks(true);
        setFrameShape(QFrame::NoFrame);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMinimumWidth(0);
        setContentsMargins(0, 0, 0, 0);
        setLineWrapMode(QTextEdit::WidgetWidth);
        setTextInteractionFlags(Qt::LinksAccessibleByMouse | Qt::TextSelectableByMouse);
        document()->setDocumentMargin(0);
        applyWrapMode();
    }

    void setContentHtml(const QString& html)
    {
        setHtml(html);
        document()->setDocumentMargin(0);
        applyWrapMode();
        scheduleHeightUpdate();
    }

    QSize sizeHint() const override
    {
        return QSize(0, height());
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, std::max(1, fontMetrics().height()));
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QTextBrowser::resizeEvent(event);
        scheduleHeightUpdate();
    }

private:
    void applyWrapMode()
    {
        QTextOption option = document()->defaultTextOption();
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document()->setDefaultTextOption(option);
    }

    void scheduleHeightUpdate()
    {
        QTimer::singleShot(0, this, [this] { updateDocumentHeight(); });
    }

    void updateDocumentHeight()
    {
        if (viewport()->width() <= 0) {
            return;
        }

        document()->setTextWidth(viewport()->width());
        const int documentHeight = static_cast<int>(std::ceil(document()->size().height()));
        const int wantedHeight = std::max(fontMetrics().height(), documentHeight + 2 * frameWidth());
        if (height() != wantedHeight) {
            setFixedHeight(wantedHeight);
        }
    }
};

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)

using SourceLanguage = QSourceHighlite::QSourceHighliter::Language;

std::optional<SourceLanguage> sourceLanguageForName(QString language)
{
    language = language.trimmed().toLower();
    const int whitespace = [&language] {
        const int space = language.indexOf(QLatin1Char(' '));
        const int tab = language.indexOf(QLatin1Char('\t'));
        if (space < 0) {
            return tab;
        }
        if (tab < 0) {
            return space;
        }
        return std::min(space, tab);
    }();
    if (whitespace >= 0) {
        language.truncate(whitespace);
    }

    if (language == QLatin1String("cpp") || language == QLatin1String("c++")
        || language == QLatin1String("cxx") || language == QLatin1String("cc")) {
        return SourceLanguage::CodeCpp;
    }
    if (language == QLatin1String("c")) {
        return SourceLanguage::CodeC;
    }
    if (language == QLatin1String("js") || language == QLatin1String("javascript")) {
        return SourceLanguage::CodeJs;
    }
    if (language == QLatin1String("bash") || language == QLatin1String("sh")
        || language == QLatin1String("shell")) {
        return SourceLanguage::CodeBash;
    }
    if (language == QLatin1String("php")) {
        return SourceLanguage::CodePHP;
    }
    if (language == QLatin1String("qml")) {
        return SourceLanguage::CodeQML;
    }
    if (language == QLatin1String("py") || language == QLatin1String("python")) {
        return SourceLanguage::CodePython;
    }
    if (language == QLatin1String("rs") || language == QLatin1String("rust")) {
        return SourceLanguage::CodeRust;
    }
    if (language == QLatin1String("java")) {
        return SourceLanguage::CodeJava;
    }
    if (language == QLatin1String("cs") || language == QLatin1String("c#")
        || language == QLatin1String("csharp")) {
        return SourceLanguage::CodeCSharp;
    }
    if (language == QLatin1String("go")) {
        return SourceLanguage::CodeGo;
    }
    if (language == QLatin1String("v")) {
        return SourceLanguage::CodeV;
    }
    if (language == QLatin1String("sql")) {
        return SourceLanguage::CodeSQL;
    }
    if (language == QLatin1String("json")) {
        return SourceLanguage::CodeJSON;
    }
    if (language == QLatin1String("xml") || language == QLatin1String("html")) {
        return SourceLanguage::CodeXML;
    }
    if (language == QLatin1String("css")) {
        return SourceLanguage::CodeCSS;
    }
    if (language == QLatin1String("ts") || language == QLatin1String("typescript")) {
        return SourceLanguage::CodeTypeScript;
    }
    if (language == QLatin1String("yaml") || language == QLatin1String("yml")) {
        return SourceLanguage::CodeYAML;
    }
    if (language == QLatin1String("ini")) {
        return SourceLanguage::CodeINI;
    }
    if (language == QLatin1String("vex")) {
        return SourceLanguage::CodeVex;
    }
    if (language == QLatin1String("cmake")) {
        return SourceLanguage::CodeCMake;
    }
    if (language == QLatin1String("make") || language == QLatin1String("makefile")) {
        return SourceLanguage::CodeMake;
    }
    if (language == QLatin1String("asm") || language == QLatin1String("assembly")) {
        return SourceLanguage::CodeAsm;
    }
    if (language == QLatin1String("lua")) {
        return SourceLanguage::CodeLua;
    }
    if (language == QLatin1String("rhai")) {
        return SourceLanguage::CodeRhai;
    }

    return std::nullopt;
}

class CodeBlockEdit final : public QPlainTextEdit
{
public:
    CodeBlockEdit(const QString& code, const QString& language, QWidget* parent = nullptr)
        : QPlainTextEdit(parent)
    {
        setObjectName(QStringLiteral("messageCodeBlock"));
        setProperty("codeLanguage", language);
        setReadOnly(true);
        setLineWrapMode(QPlainTextEdit::NoWrap);
        setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        setMinimumWidth(0);
        setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));
        document()->setDocumentMargin(6);

        QPalette codePalette = palette();
        codePalette.setColor(QPalette::Base, QColor(39, 40, 34));
        codePalette.setColor(QPalette::Text, QColor(227, 226, 214));
        setPalette(codePalette);

        setPlainText(code);

        if (const auto sourceLanguage = sourceLanguageForName(language)) {
            auto* highlighter = new QSourceHighlite::QSourceHighliter(
                document(), QSourceHighlite::QSourceHighliter::Themes::Monokai);
            highlighter->setCurrentLanguage(*sourceLanguage);
            highlighter->rehighlight();
            setProperty("sourceHighliteLanguage", static_cast<int>(*sourceLanguage));
        }

        scheduleHeightUpdate();
    }

    QSize sizeHint() const override
    {
        return QSize(0, height());
    }

    QSize minimumSizeHint() const override
    {
        return QSize(0, std::max(1, fontMetrics().height()));
    }

protected:
    void resizeEvent(QResizeEvent* event) override
    {
        QPlainTextEdit::resizeEvent(event);
        scheduleHeightUpdate();
    }

private:
    void scheduleHeightUpdate()
    {
        QTimer::singleShot(0, this, [this] { updateDocumentHeight(); });
    }

    void updateDocumentHeight()
    {
        const int documentHeight = static_cast<int>(std::ceil(document()->size().height()));
        const int scrollBarHeight = horizontalScrollBar()->maximum() > 0
            ? horizontalScrollBar()->sizeHint().height()
            : 0;
        const int wantedHeight = std::max(
            fontMetrics().height(), documentHeight + 2 * frameWidth() + scrollBarHeight);
        if (height() != wantedHeight) {
            setFixedHeight(wantedHeight);
        }
    }
};

bool isCodeBlock(const QTextBlock& block)
{
    const QTextBlockFormat format = block.blockFormat();
    return format.nonBreakableLines()
        || format.hasProperty(QTextFormat::BlockCodeFence)
        || format.hasProperty(QTextFormat::BlockCodeLanguage);
}

QString codeLanguage(const QTextBlock& block)
{
    return block.blockFormat().stringProperty(QTextFormat::BlockCodeLanguage);
}

QString fragmentHtml(QTextDocument& document, int start, int end)
{
    if (end <= start) {
        return {};
    }

    QTextCursor cursor(&document);
    cursor.setPosition(start);
    cursor.setPosition(end, QTextCursor::KeepAnchor);
    return QTextDocumentFragment(cursor).toHtml();
}

#endif

} // namespace

MessageContentWidget::MessageContentWidget(QWidget* parent)
    : QWidget(parent)
    , contentLayout(new QVBoxLayout(this))
{
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(2);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
    setMinimumWidth(0);
}

void MessageContentWidget::setMessage(const QString& message)
{
    clearContent();

    if (message.isEmpty()) {
        setVisible(false);
        return;
    }

    setVisible(true);
#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
    addMarkdownContent(message);
#else
    addRichText(MessageFormatter::formatMessageText(message));
#endif
}

void MessageContentWidget::clear()
{
    clearContent();
    setVisible(false);
}

QString MessageContentWidget::selectedText() const
{
    QStringList selections;
    for (int i = 0; i < contentLayout->count(); ++i) {
        QWidget* widget = contentLayout->itemAt(i)->widget();
        if (const auto* browser = qobject_cast<QTextBrowser*>(widget)) {
            if (browser->textCursor().hasSelection()) {
                selections.push_back(browser->textCursor().selectedText());
            }
        } else if (const auto* editor = qobject_cast<QPlainTextEdit*>(widget)) {
            if (editor->textCursor().hasSelection()) {
                selections.push_back(editor->textCursor().selectedText());
            }
        }
    }
    return selections.join(QLatin1Char('\n'));
}

void MessageContentWidget::clearContent()
{
    while (QLayoutItem* item = contentLayout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

void MessageContentWidget::addRichText(const QString& html)
{
    if (html.isEmpty()) {
        return;
    }

    auto* richText = new WrappedRichText(this);
    richText->setContentHtml(html);
    connect(richText, &QTextBrowser::highlighted, this, [this](const QUrl& url) {
        emit linkHovered(url.toString());
    });
    contentLayout->addWidget(richText);
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 10, 0)
void MessageContentWidget::addMarkdownContent(const QString& message)
{
    QTextDocument document;
    MessageFormatter::buildMarkdownDocument(document, message);

    int richStart = 0;
    QTextBlock block = document.begin();
    while (block.isValid()) {
        if (!isCodeBlock(block)) {
            block = block.next();
            continue;
        }

        const int codeStart = block.position();
        addRichText(fragmentHtml(document, richStart, codeStart));

        QString language = codeLanguage(block);
        QStringList codeLines;
        do {
            if (language.isEmpty()) {
                language = codeLanguage(block);
            }
            codeLines.push_back(block.text());
            block = block.next();
        } while (block.isValid() && isCodeBlock(block));

        addCodeBlock(codeLines.join(QLatin1Char('\n')), language);
        richStart = block.isValid() ? block.position() : document.characterCount() - 1;
    }

    addRichText(fragmentHtml(document, richStart, document.characterCount() - 1));
}

void MessageContentWidget::addCodeBlock(const QString& code, const QString& language)
{
    contentLayout->addWidget(new CodeBlockEdit(code, language, this));
}
#endif

} // namespace Mattermost
