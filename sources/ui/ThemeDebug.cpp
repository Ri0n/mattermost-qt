/**
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "ThemeDebug.h"

#include <QApplication>
#include <QColor>
#include <QLoggingCategory>
#include <QPalette>
#include <QPointer>
#include <QStyle>
#include <QTextBrowser>
#include <QTimer>
#include <QWidget>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif

Q_LOGGING_CATEGORY(lcMattermostThemeTrace,
                   "mattermost.theme.trace",
                   QtWarningMsg)

namespace Mattermost {
namespace {

QString eventName(QEvent::Type type)
{
    switch (type) {
    case QEvent::PaletteChange:
        return QStringLiteral("PaletteChange");
    case QEvent::ApplicationPaletteChange:
        return QStringLiteral("ApplicationPaletteChange");
    case QEvent::StyleChange:
        return QStringLiteral("StyleChange");
    case QEvent::ParentChange:
        return QStringLiteral("ParentChange");
    case QEvent::Show:
        return QStringLiteral("Show");
    case QEvent::Hide:
        return QStringLiteral("Hide");
    default:
        return QString::number(static_cast<int>(type));
    }
}

QString colorName(const QColor& color)
{
    return color.name(QColor::HexArgb);
}

QString paletteSummary(const QPalette& palette)
{
    return QStringLiteral("W=%1 WT=%2 B=%3 T=%4 Btn=%5 BtnT=%6 Hi=%7 HiT=%8")
        .arg(colorName(palette.color(QPalette::Window)),
             colorName(palette.color(QPalette::WindowText)),
             colorName(palette.color(QPalette::Base)),
             colorName(palette.color(QPalette::Text)),
             colorName(palette.color(QPalette::Button)),
             colorName(palette.color(QPalette::ButtonText)),
             colorName(palette.color(QPalette::Highlight)),
             colorName(palette.color(QPalette::HighlightedText)));
}

QString widgetName(const QWidget* widget)
{
    if (!widget) {
        return QStringLiteral("<null>");
    }

    QString result = QString::fromLatin1(widget->metaObject()->className());
    if (!widget->objectName().isEmpty()) {
        result += QLatin1Char('#');
        result += widget->objectName();
    }
    return result;
}

QString widgetPath(const QWidget* widget)
{
    QStringList path;
    const QWidget* current = widget;
    for (int depth = 0; current && depth < 8; ++depth) {
        path.push_back(widgetName(current));
        current = current->parentWidget();
    }
    return path.join(QStringLiteral(" <- "));
}

bool classIsInteresting(const QWidget* widget)
{
    if (!widget) {
        return false;
    }

    const QString className = QString::fromLatin1(widget->metaObject()->className());
    return className.contains(QStringLiteral("MainWindow"))
        || className.contains(QStringLiteral("ChannelTree"))
        || className.contains(QStringLiteral("ChatArea"))
        || className.contains(QStringLiteral("ChatLogWidget"))
        || className.contains(QStringLiteral("LongListWidget"))
        || className.contains(QStringLiteral("PostWidget"))
        || className.contains(QStringLiteral("MessageContentWidget"))
        || className.contains(QStringLiteral("OutgoingPostPanel"))
        || className == QStringLiteral("QTabWidget")
        || className == QStringLiteral("QToolButton")
        || className == QStringLiteral("QStackedWidget")
        || qobject_cast<const QTextBrowser*>(widget) != nullptr;
}

bool hasInterestingAncestor(const QWidget* widget)
{
    const QWidget* current = widget ? widget->parentWidget() : nullptr;
    for (int depth = 0; current && depth < 8; ++depth) {
        if (classIsInteresting(current)) {
            return true;
        }
        current = current->parentWidget();
    }
    return false;
}

bool isInterestingWidget(const QWidget* widget)
{
    if (!widget) {
        return false;
    }

    if (classIsInteresting(widget)) {
        return true;
    }

    const QString objectName = widget->objectName();
    if (objectName == QStringLiteral("qt_scrollarea_viewport")
        || objectName == QStringLiteral("addEmojiButton")
        || objectName == QStringLiteral("attachButton")
        || objectName == QStringLiteral("sendButton")
        || objectName == QStringLiteral("channelList")) {
        return hasInterestingAncestor(widget);
    }

    return false;
}

bool shouldTraceEvent(const QWidget* widget, QEvent::Type type)
{
    if (!isInterestingWidget(widget)) {
        return false;
    }

    if (type == QEvent::PaletteChange
        || type == QEvent::ApplicationPaletteChange
        || type == QEvent::StyleChange
        || type == QEvent::ParentChange) {
        return true;
    }

    if (type != QEvent::Show && type != QEvent::Hide) {
        return false;
    }

    const QString className = QString::fromLatin1(widget->metaObject()->className());
    const QString objectName = widget->objectName();
    return className.contains(QStringLiteral("ChatArea"))
        || className.contains(QStringLiteral("OutgoingPostPanel"))
        || className == QStringLiteral("QStackedWidget")
        || objectName == QStringLiteral("addEmojiButton")
        || objectName == QStringLiteral("attachButton");
}

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
QString schemeName(Qt::ColorScheme scheme)
{
    switch (scheme) {
    case Qt::ColorScheme::Light:
        return QStringLiteral("Light");
    case Qt::ColorScheme::Dark:
        return QStringLiteral("Dark");
    default:
        return QStringLiteral("Unknown");
    }
}
#endif

} // namespace

namespace ThemeDebug {

void logWidgetState(const char* marker,
                    const QWidget* widget,
                    QEvent::Type eventType)
{
    if (!widget || !lcMattermostThemeTrace().isDebugEnabled()) {
        return;
    }

    const QString styleName = widget->style()
        ? QString::fromLatin1(widget->style()->metaObject()->className())
        : QStringLiteral("<null>");
    const QString appPalette = qApp
        ? paletteSummary(qApp->palette())
        : QStringLiteral("<no-app>");

    const QWidget* parent = widget->parentWidget();
    const QWidget* topLevel = widget->window();
    const QString parentPalette = parent
        ? paletteSummary(parent->palette())
        : QStringLiteral("<none>");
    const QString topLevelPalette = topLevel
        ? paletteSummary(topLevel->palette())
        : QStringLiteral("<none>");

    qCDebug(lcMattermostThemeTrace).noquote()
        << marker
        << QStringLiteral("event=") + eventName(eventType)
        << QStringLiteral("widget=") + widgetPath(widget)
        << QStringLiteral("visible=") + QString::number(widget->isVisible())
        << QStringLiteral("enabled=") + QString::number(widget->isEnabled())
        << QStringLiteral("style=") + styleName
        << QStringLiteral("stylesheet=") + QString::number(!widget->styleSheet().isEmpty())
        << QStringLiteral("autofill=") + QString::number(widget->autoFillBackground())
        << QStringLiteral("widgetPalette={") + paletteSummary(widget->palette()) + QLatin1Char('}')
        << QStringLiteral("parentPalette={") + parentPalette + QLatin1Char('}')
        << QStringLiteral("windowPalette={") + topLevelPalette + QLatin1Char('}')
        << QStringLiteral("appPalette={") + appPalette + QLatin1Char('}');
}

} // namespace ThemeDebug

ThemeDebugProbe::ThemeDebugProbe(QObject* parent)
    : QObject(parent)
{
    if (qApp) {
        qApp->installEventFilter(this);
    }

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    if (QGuiApplication::styleHints()) {
        connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged,
                this, [this](Qt::ColorScheme scheme) {
            if (!lcMattermostThemeTrace().isDebugEnabled()) {
                return;
            }
            qCDebug(lcMattermostThemeTrace).noquote()
                << "THEME_SCHEME_SIGNAL"
                << QStringLiteral("scheme=") + schemeName(scheme)
                << QStringLiteral("appPalette={") + paletteSummary(qApp->palette()) + QLatin1Char('}');
            QTimer::singleShot(0, this, [scheme] {
                qCDebug(lcMattermostThemeTrace).noquote()
                    << "THEME_SCHEME_QUEUED"
                    << QStringLiteral("scheme=") + schemeName(scheme)
                    << QStringLiteral("appPalette={") + paletteSummary(qApp->palette()) + QLatin1Char('}');
            });
        });
    }
#endif
}

bool ThemeDebugProbe::eventFilter(QObject* watched, QEvent* event)
{
    auto* widget = qobject_cast<QWidget*>(watched);
    if (!widget || !event || !shouldTraceEvent(widget, event->type())) {
        return QObject::eventFilter(watched, event);
    }

    ThemeDebug::logWidgetState("THEME_EVENT_BEFORE", widget, event->type());

    if (event->type() == QEvent::PaletteChange
        || event->type() == QEvent::ApplicationPaletteChange
        || event->type() == QEvent::StyleChange
        || event->type() == QEvent::ParentChange) {
        const QEvent::Type type = event->type();
        QPointer<QWidget> guard(widget);
        QTimer::singleShot(0, this, [guard, type] {
            if (guard) {
                ThemeDebug::logWidgetState("THEME_EVENT_AFTER", guard, type);
            }
        });
    }

    return QObject::eventFilter(watched, event);
}

} // namespace Mattermost
