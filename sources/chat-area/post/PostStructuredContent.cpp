#include <algorithm>

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPalette>
#include <QPointer>
#include <QSet>
#include <QTextBrowser>
#include <QVBoxLayout>

#include "MessageContentWidget.h"
#include "PostWidget.h"
#include "navigation/AppNavigationService.h"

namespace Mattermost {
namespace {

constexpr auto structuredObjectName = "mattermostStructuredPostContent";
constexpr auto structuredSignatureProperty = "mattermostStructuredContentSignature";

QString escapeMarkdownLabel(QString text)
{
    text.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    text.replace(QLatin1Char('['), QStringLiteral("\\["));
    text.replace(QLatin1Char(']'), QStringLiteral("\\]"));
    return text;
}

QString markdownLink(const QString& label, const QString& url)
{
    if (label.isEmpty()) {
        return url;
    }
    if (url.isEmpty()) {
        return label;
    }
    return QStringLiteral("[%1](<%2>)").arg(escapeMarkdownLabel(label), url);
}

void appendParagraph(QStringList& paragraphs, const QString& value)
{
    const QString text = value.trimmed();
    if (!text.isEmpty()) {
        paragraphs.push_back(text);
    }
}

qreal lightnessDistance(const QColor& first, const QColor& second)
{
    const qreal delta = first.lightnessF() - second.lightnessF();
    return delta < 0.0 ? -delta : delta;
}

QColor visibleAccent(QColor accent, const QPalette& palette)
{
    if (!accent.isValid()) {
        accent = palette.color(QPalette::Highlight);
    }

    const QColor background = palette.color(QPalette::Window);
    constexpr qreal minimumLightnessDistance = 0.28;
    if (lightnessDistance(accent, background) >= minimumLightnessDistance) {
        return accent;
    }

    // Keep a bot-provided accent whenever possible, but do not allow white on
    // a light card (or black on a dark one) to become effectively invisible.
    // Blend toward the current text colour only as far as necessary, which is
    // naturally theme-aware and preserves most of the original hue.
    const QColor textColor = palette.color(QPalette::Text);
    for (int step = 1; step <= 4; ++step) {
        const qreal amount = static_cast<qreal>(step) / 4.0;
        const qreal keep = 1.0 - amount;
        const QColor candidate = QColor::fromRgbF(
            accent.redF() * keep + textColor.redF() * amount,
            accent.greenF() * keep + textColor.greenF() * amount,
            accent.blueF() * keep + textColor.blueF() * amount,
            accent.alphaF());
        if (lightnessDistance(candidate, background) >= minimumLightnessDistance) {
            return candidate;
        }
    }

    return textColor;
}

QString attachmentCardMarkdown(const QJsonObject& attachment)
{
    QStringList paragraphs;

    const QString authorName = attachment.value(QStringLiteral("author_name")).toString();
    const QString authorLink = attachment.value(QStringLiteral("author_link")).toString();
    appendParagraph(paragraphs, markdownLink(authorName, authorLink));

    const QString title = attachment.value(QStringLiteral("title")).toString();
    const QString titleLink = attachment.value(QStringLiteral("title_link")).toString();
    if (!title.isEmpty()) {
        appendParagraph(paragraphs,
                        QStringLiteral("**%1**").arg(markdownLink(title, titleLink)));
    }

    appendParagraph(paragraphs, attachment.value(QStringLiteral("text")).toString());

    for (const QJsonValue& fieldValue : attachment.value(QStringLiteral("fields")).toArray()) {
        const QJsonObject field = fieldValue.toObject();
        const QString fieldTitle = field.value(QStringLiteral("title")).toString().trimmed();
        const QString fieldText = field.value(QStringLiteral("value")).toString().trimmed();
        if (fieldTitle.isEmpty()) {
            appendParagraph(paragraphs, fieldText);
        } else if (fieldText.isEmpty()) {
            appendParagraph(paragraphs, QStringLiteral("**%1**").arg(fieldTitle));
        } else {
            appendParagraph(paragraphs,
                            QStringLiteral("**%1**\n%2").arg(fieldTitle, fieldText));
        }
    }

    QStringList actionNames;
    for (const QJsonValue& actionValue : attachment.value(QStringLiteral("actions")).toArray()) {
        const QString name = actionValue.toObject().value(QStringLiteral("name")).toString().trimmed();
        if (!name.isEmpty()) {
            actionNames.push_back(name);
        }
    }
    if (!actionNames.isEmpty()) {
        appendParagraph(paragraphs,
                        QObject::tr("**Actions:** %1").arg(actionNames.join(QStringLiteral(" · "))));
    }

    const QString imageUrl = attachment.value(QStringLiteral("image_url")).toString();
    const QString thumbUrl = attachment.value(QStringLiteral("thumb_url")).toString();
    if (!imageUrl.isEmpty()) {
        appendParagraph(paragraphs, markdownLink(QObject::tr("Image"), imageUrl));
    } else if (!thumbUrl.isEmpty()) {
        appendParagraph(paragraphs, markdownLink(QObject::tr("Image"), thumbUrl));
    }

    appendParagraph(paragraphs, attachment.value(QStringLiteral("footer")).toString());

    if (paragraphs.isEmpty()) {
        appendParagraph(paragraphs, attachment.value(QStringLiteral("fallback")).toString());
    }

    return paragraphs.join(QStringLiteral("\n\n"));
}

void wireLinks(MessageContentWidget& content, PostWidget& postWidget)
{
    QPointer<PostWidget> guard(&postWidget);
    for (QTextBrowser* browser : content.findChildren<QTextBrowser*>()) {
        if (!browser) {
            continue;
        }

        browser->setOpenLinks(false);
        browser->setOpenExternalLinks(false);
        QObject::connect(browser, &QTextBrowser::anchorClicked,
                         browser, [guard](const QUrl& url) {
            if (guard) {
                AppNavigationService::instance(guard->getBackend()).openUrl(url);
            }
        });
    }
}

QWidget* createAttachmentWidget(PostWidget& postWidget, const QJsonObject& attachment)
{
    auto* container = new QWidget(&postWidget);
    container->setObjectName(QString::fromLatin1(structuredObjectName));
    container->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    auto* outerLayout = new QVBoxLayout(container);
    outerLayout->setContentsMargins(0, 2, 0, 2);
    outerLayout->setSpacing(3);

    const QString pretext = attachment.value(QStringLiteral("pretext")).toString().trimmed();
    if (!pretext.isEmpty()) {
        auto* pretextWidget = new MessageContentWidget(container);
        pretextWidget->setMessage(pretext);
        wireLinks(*pretextWidget, postWidget);
        outerLayout->addWidget(pretextWidget);
    }

    const QString cardMarkdown = attachmentCardMarkdown(attachment);
    if (cardMarkdown.isEmpty()) {
        return container;
    }

    auto* card = new QFrame(container);
    card->setFrameShape(QFrame::StyledPanel);
    card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    auto* cardLayout = new QHBoxLayout(card);
    cardLayout->setContentsMargins(5, 4, 6, 4);
    cardLayout->setSpacing(7);

    auto* bar = new QFrame(card);
    bar->setFixedWidth(3);
    bar->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    const QColor requestedColor(attachment.value(QStringLiteral("color")).toString());
    const QColor accentColor = visibleAccent(requestedColor, card->palette());
    // Do not rely on QFrame's inherited backgroundRole here. Child frames can
    // keep NoRole/another role under some styles, making autoFillBackground
    // ignore the Window colour we placed in a custom palette. A widget-local
    // stylesheet makes the 3 px attachment accent deterministic on every Qt
    // style while still using the theme-aware colour calculated above.
    bar->setStyleSheet(QStringLiteral("border: none; background-color: %1;")
                           .arg(accentColor.name(QColor::HexArgb)));
    cardLayout->addWidget(bar);

    auto* content = new MessageContentWidget(card);
    content->setMessage(cardMarkdown);
    wireLinks(*content, postWidget);
    cardLayout->addWidget(content, 1);

    outerLayout->addWidget(card);
    return container;
}

void collectBlockText(const QJsonValue& value, QStringList& output, QSet<QString>& seen)
{
    if (value.isArray()) {
        for (const QJsonValue& child : value.toArray()) {
            collectBlockText(child, output, seen);
        }
        return;
    }
    if (!value.isObject()) {
        return;
    }

    const QJsonObject object = value.toObject();
    static const QStringList textKeys {
        QStringLiteral("text"),
        QStringLiteral("title"),
        QStringLiteral("label"),
        QStringLiteral("value"),
        QStringLiteral("name"),
        QStringLiteral("description"),
    };
    for (const QString& key : textKeys) {
        const QString text = object.value(key).toString().trimmed();
        if (!text.isEmpty() && !seen.contains(text)) {
            seen.insert(text);
            output.push_back(text);
        }
    }

    for (auto it = object.constBegin(); it != object.constEnd(); ++it) {
        if (it.value().isArray() || it.value().isObject()) {
            collectBlockText(it.value(), output, seen);
        }
    }
}

QWidget* createBlocksFallback(PostWidget& postWidget, const QJsonValue& blocks)
{
    QStringList texts;
    QSet<QString> seen;
    collectBlockText(blocks, texts, seen);

    auto* frame = new QFrame(&postWidget);
    frame->setObjectName(QString::fromLatin1(structuredObjectName));
    frame->setFrameShape(QFrame::StyledPanel);
    frame->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    auto* layout = new QVBoxLayout(frame);
    layout->setContentsMargins(6, 4, 6, 4);
    layout->setSpacing(2);

    auto* content = new MessageContentWidget(frame);
    content->setMessage(texts.isEmpty()
        ? QObject::tr("[Unsupported structured Mattermost message]")
        : texts.join(QStringLiteral("\n\n")));
    wireLinks(*content, postWidget);
    layout->addWidget(content);
    return frame;
}

void clearStructuredWidgets(PostWidget& postWidget, QVBoxLayout& layout)
{
    const auto existing = postWidget.findChildren<QWidget*>(
        QString::fromLatin1(structuredObjectName), Qt::FindDirectChildrenOnly);
    for (QWidget* widget : existing) {
        layout.removeWidget(widget);
        delete widget;
    }
}

void refreshStructuredContent(PostWidget& postWidget)
{
    const QJsonObject props = postWidget.post.props.toObject();
    const QJsonArray attachments = props.value(QStringLiteral("attachments")).toArray();
    const QJsonValue blocks = props.value(QStringLiteral("mm_blocks"));
    const bool hasBlocks = (blocks.isArray() && !blocks.toArray().isEmpty())
        || (blocks.isObject() && !blocks.toObject().isEmpty());

    QJsonObject signatureObject;
    signatureObject.insert(QStringLiteral("attachments"), attachments);
    signatureObject.insert(QStringLiteral("mm_blocks"), blocks);
    const QByteArray signature = QJsonDocument(signatureObject).toJson(QJsonDocument::Compact);
    if (postWidget.property(structuredSignatureProperty).toByteArray() == signature) {
        return;
    }
    postWidget.setProperty(structuredSignatureProperty, signature);

    auto* layout = postWidget.findChild<QVBoxLayout*>(QStringLiteral("verticalLayout"));
    if (!layout) {
        return;
    }
    clearStructuredWidgets(postWidget, *layout);

    // Matterpoll also stores its definition in props.attachments. A real poll
    // has a poll_id; those attachments belong to PostPoll, not this renderer.
    if (!props.value(QStringLiteral("poll_id")).toString().isEmpty()) {
        return;
    }
    if (attachments.isEmpty() && !hasBlocks) {
        return;
    }

    MessageContentWidget* mainContent = nullptr;
    const auto directContents = postWidget.findChildren<MessageContentWidget*>(
        QString(), Qt::FindDirectChildrenOnly);
    if (!directContents.isEmpty()) {
        mainContent = directContents.front();
    }
    int insertIndex = mainContent ? layout->indexOf(mainContent) + 1 : layout->count();

    for (const QJsonValue& attachmentValue : attachments) {
        const QJsonObject attachment = attachmentValue.toObject();
        if (attachment.isEmpty()) {
            continue;
        }
        layout->insertWidget(insertIndex++, createAttachmentWidget(postWidget, attachment));
    }

    if (attachments.isEmpty() && hasBlocks) {
        layout->insertWidget(insertIndex, createBlocksFallback(postWidget, blocks));
    }

    postWidget.updateGeometry();
    QMetaObject::invokeMethod(&postWidget, "dimensionsChanged", Qt::QueuedConnection);
}

class StructuredContentInstaller final : public QObject
{
public:
    using QObject::QObject;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override
    {
        if (!event) {
            return QObject::eventFilter(watched, event);
        }

        if (auto* postWidget = qobject_cast<PostWidget*>(watched)) {
            if (event->type() == QEvent::PaletteChange
                || event->type() == QEvent::ApplicationPaletteChange) {
                // Accent visibility depends on the current palette; force a
                // rebuild even though the structured JSON itself is unchanged.
                postWidget->setProperty(structuredSignatureProperty, QByteArray());
                refreshStructuredContent(*postWidget);
            } else if (event->type() == QEvent::Show
                       || event->type() == QEvent::LayoutRequest) {
                refreshStructuredContent(*postWidget);
            }
        }
        return QObject::eventFilter(watched, event);
    }
};

void installStructuredContentRenderer()
{
    if (!qApp) {
        return;
    }
    auto* installer = new StructuredContentInstaller(qApp);
    qApp->installEventFilter(installer);
}

Q_COREAPP_STARTUP_FUNCTION(installStructuredContentRenderer)

} // namespace
} // namespace Mattermost
