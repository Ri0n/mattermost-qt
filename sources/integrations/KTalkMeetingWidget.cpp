/**
 * @file KTalkMeetingWidget.cpp
 * @brief Native rendering for KTalk meeting posts.
 *
 * Copyright 2026 Sergei Ilinykh
 *
 * This file is part of Mattermost-QT.
 */

#include "KTalkMeetingWidget.h"

#include <QDesktopServices>
#include <QFont>
#include <QHBoxLayout>
#include <QIcon>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QSizePolicy>
#include <QUrl>
#include <QVBoxLayout>

#include "KTalkIntegration.h"
#include "backend/types/BackendPost.h"

namespace Mattermost {

namespace {

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
    outerLayout->setContentsMargins(10, 8, 10, 8);
    outerLayout->setSpacing(8);

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
