#pragma once

#include <QHBoxLayout>
#include <QString>
#include <QWidget>

namespace Mattermost::ReactionChipStyle {

constexpr int Height = 24;
constexpr int IconExtent = 20;
constexpr int IconPointSize = 14;
constexpr int CountPointSize = 8;

inline QString styleSheet(const QString& objectName)
{
    return QStringLiteral(
        "QWidget#%1 {"
        " border: 1px solid rgba(128, 128, 128, 130);"
        " border-radius: 4px;"
        " background-color: rgba(128, 128, 128, 52);"
        " }"
        "QWidget#%1:hover {"
        " background-color: rgba(128, 128, 128, 72);"
        " }").arg(objectName);
}

inline void apply(QWidget* widget, QHBoxLayout* layout, const QString& objectName)
{
    if (!widget || !layout) {
        return;
    }
    widget->setObjectName(objectName);
    widget->setAttribute(Qt::WA_StyledBackground, true);
    widget->setCursor(Qt::PointingHandCursor);
    widget->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    widget->setMaximumHeight(Height);
    widget->setStyleSheet(styleSheet(objectName));
    layout->setContentsMargins(4, 2, 4, 1);
    layout->setSpacing(2);
}

} // namespace Mattermost::ReactionChipStyle
