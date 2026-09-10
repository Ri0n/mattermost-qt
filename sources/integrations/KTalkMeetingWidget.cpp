/**
 * @file KTalkMeetingWidget.cpp
 * @brief Native rendering for KTalk meeting posts.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "KTalkMeetingWidget.h"

#include <QColor>
#include <QDesktopServices>
#include <QEvent>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonObject>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QSizePolicy>
#include <QUrl>
#include <QVBoxLayout>

#include "KTalkIntegration.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

namespace {

constexpr char MeetingPostType[] = "custom_ktalk_meeting";
constexpr int MetadataAccentWidth = 3;
constexpr int IconExtent = 24;

QUrl httpUrl(const QString& value)
{
    const QUrl url(value);
    if (!url.isValid()
        || (url.scheme() != QLatin1String("https")
            && url.scheme() != QLatin1String("http"))) {
        return {};
    }
    return url;
}

} // namespace

bool KTalkMeetingWidget::supports(const BackendPost& post)
{
    return post.type == QLatin1String(MeetingPostType);
}

KTalkMeetingWidget::KTalkMeetingWidget(Backend& backend,
                                       const BackendPost& post,
                                       QWidget* parent)
    : QFrame(parent)
    , backend_(backend)
{
    const QJsonObject props = post.props.toObject();
    meetingLink_ = props.value(QStringLiteral("meeting_link")).toString();
    const QUrl link = httpUrl(meetingLink_);
    if (link.isEmpty()) {
        hide();
        return;
    }
    valid_ = true;

    setFrameShape(QFrame::StyledPanel);
    setFrameShadow(QFrame::Plain);
    setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);

    auto* outerLayout = new QHBoxLayout(this);
    // Match the visual grammar used by structured attachment cards: the
    // vertical accent marks content rendered from server-provided metadata.
    outerLayout->setContentsMargins(5, 4, 6, 4);
    outerLayout->setSpacing(7);

    accentBar_ = new QFrame(this);
    accentBar_->setFixedWidth(MetadataAccentWidth);
    accentBar_->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Expanding);
    outerLayout->addWidget(accentBar_);
    refreshAccent();

    iconLabel_ = new QLabel(this);
    iconLabel_->setFixedSize(IconExtent, IconExtent);
    iconLabel_->setAlignment(Qt::AlignCenter);
    outerLayout->addWidget(iconLabel_, 0, Qt::AlignTop);

    auto* contentLayout = new QVBoxLayout;
    contentLayout->setContentsMargins(0, 0, 0, 0);
    contentLayout->setSpacing(4);
    outerLayout->addLayout(contentLayout, 1);

    QString title = props.value(QStringLiteral("meeting_topic")).toString().trimmed();
    if (title.isEmpty()) {
        title = tr("KTalk Meeting");
    }
    auto* titleLabel = new QLabel(title, this);
    QFont titleFont = titleLabel->font();
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);
    titleLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    contentLayout->addWidget(titleLabel);

    const QString meetingId = props.value(QStringLiteral("meeting_id")).toString().trimmed();
    if (!meetingId.isEmpty()) {
        const bool personal = props.value(QStringLiteral("meeting_personal")).toBool();
        auto* idLabel = new QLabel(
            personal
                ? tr("Personal Meeting ID (PMI): %1").arg(meetingId)
                : tr("Meeting ID: %1").arg(meetingId),
            this);
        idLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
        contentLayout->addWidget(idLabel);
    }

    joinButton_ = new QPushButton(tr("Join meeting"), this);
    joinButton_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    joinButton_->setCursor(Qt::PointingHandCursor);
    connect(joinButton_, &QPushButton::clicked,
            this, &KTalkMeetingWidget::openMeeting);
    contentLayout->addWidget(joinButton_, 0, Qt::AlignLeft);

    KTalkIntegration& integration = KTalkIntegration::instance(backend_);
    connect(&integration, &KTalkIntegration::iconChanged,
            this, &KTalkMeetingWidget::refreshIcon);
    refreshIcon();
}

void KTalkMeetingWidget::changeEvent(QEvent* event)
{
    QFrame::changeEvent(event);
    if (event && (event->type() == QEvent::PaletteChange
                  || event->type() == QEvent::ApplicationPaletteChange
                  || event->type() == QEvent::StyleChange)) {
        refreshAccent();
    }
}

void KTalkMeetingWidget::refreshAccent()
{
    if (!accentBar_) {
        return;
    }

    const QColor accentColor = palette().color(QPalette::Highlight);
    // Keep this widget-local, just like structured attachment cards: some Qt
    // styles ignore an inherited QFrame backgroundRole for a child accent bar.
    accentBar_->setStyleSheet(
        QStringLiteral("border: none; background-color: %1;")
            .arg(accentColor.name(QColor::HexArgb)));
}

void KTalkMeetingWidget::refreshIcon()
{
    const QIcon icon = KTalkIntegration::instance(backend_).icon();
    if (iconLabel_) {
        iconLabel_->setPixmap(icon.pixmap(IconExtent, IconExtent));
    }
    if (joinButton_) {
        joinButton_->setIcon(icon);
    }
}

void KTalkMeetingWidget::openMeeting()
{
    const QUrl url = httpUrl(meetingLink_);
    if (!url.isEmpty()) {
        QDesktopServices::openUrl(url);
    }
}

} // namespace Mattermost
