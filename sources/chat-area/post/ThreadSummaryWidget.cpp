#include "ThreadSummaryWidget.h"

#include <algorithm>

#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QSet>

#include "backend/Backend.h"
#include "backend/Storage.h"
#include "backend/UserProfileService.h"
#include "backend/types/BackendChannel.h"
#include "backend/types/BackendPost.h"
#include "backend/types/BackendUser.h"
#include "ui/AvatarUtils.h"

namespace Mattermost {

namespace {

constexpr int ParticipantAvatarSize = 16;
constexpr int MaxParticipantAvatars = 5;

} // namespace

ThreadSummaryWidget::ThreadSummaryWidget(Backend& backend,
                                         BackendChannel& channel,
                                         BackendPost& rootPost,
                                         QWidget* parent)
    : QWidget(parent)
    , backend(backend)
    , channel(channel)
    , rootPost(rootPost)
    , layout(new QHBoxLayout(this))
    , button(new QPushButton(this))
{
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    button->setFlat(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setToolTip(tr("Open thread"));
    button->setAccessibleName(tr("Open thread"));
    button->setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    button->setStyleSheet(QStringLiteral(
        "QPushButton {"
        " border: 1px solid rgba(128, 128, 128, 120);"
        " border-radius: 4px;"
        " background-color: rgba(128, 128, 128, 44);"
        " padding: 1px 4px;"
        " }"
        "QPushButton:hover {"
        " background-color: rgba(128, 128, 128, 70);"
        " }"));
    connect(button, &QPushButton::clicked, this, &ThreadSummaryWidget::clicked);

    connect(&channel, &BackendChannel::onPostEdited, this,
            [this](BackendPost& edited) {
        if (edited.id == rootPost.id) {
            refresh();
        }
    });
    connect(&channel, &BackendChannel::onNewPost, this,
            [this](BackendPost& reply) {
        if (reply.root_id == rootPost.id) {
            refresh();
        }
    });

    refresh();
}

void ThreadSummaryWidget::refresh()
{
    rebuildParticipantAvatars();
    button->setText(rootPost.reply_count > 0
        ? QStringLiteral("💬 %1").arg(rootPost.reply_count)
        : QStringLiteral("💬"));
    updateGeometry();
}

void ThreadSummaryWidget::rebuildParticipantAvatars()
{
    while (layout->count() > 0) {
        QLayoutItem* item = layout->takeAt(0);
        QWidget* widget = item ? item->widget() : nullptr;
        if (widget && widget != button) {
            widget->deleteLater();
        }
        delete item;
    }

    QStringList participantIds;
    QSet<QString> seen;
    for (auto it = channel.posts.rbegin(); it != channel.posts.rend(); ++it) {
        if (it->root_id != rootPost.id || it->user_id.isEmpty() || seen.contains(it->user_id)) {
            continue;
        }
        seen.insert(it->user_id);
        participantIds.push_back(it->user_id);
        if (participantIds.size() >= MaxParticipantAvatars) {
            break;
        }
    }

    for (const QString& userId : participantIds) {
        BackendUser* user = backend.getStorage().getUserById(userId);
        if (!user) {
            QPointer<ThreadSummaryWidget> guard(this);
            UserProfileService::instance(backend).ensureUser(
                userId, [guard](const BackendUser*) {
                    if (guard) {
                        guard->refresh();
                    }
                });
            continue;
        }

        watchUser(user);
        if (user->avatar.isNull()
            || user->avatar_picture_update != user->last_picture_update) {
            UserProfileService::instance(backend).ensureAvatar(*user);
        }

        auto* avatar = new QLabel(this);
        avatar->setFixedSize(ParticipantAvatarSize, ParticipantAvatarSize);
        avatar->setAlignment(Qt::AlignCenter);
        avatar->setToolTip(user->getDisplayName());
        if (!user->avatar.isNull()) {
            avatar->setPixmap(AvatarUtils::circular(user->avatar, ParticipantAvatarSize));
        }
        layout->addWidget(avatar);
    }

    layout->addWidget(button);
}

void ThreadSummaryWidget::watchUser(const BackendUser* user)
{
    if (!user) {
        return;
    }
    connect(user, &BackendUser::onAvatarChanged,
            this, &ThreadSummaryWidget::refresh, Qt::UniqueConnection);
}

} // namespace Mattermost
