/**
 * @file PinnedPostsList.cpp
 * @brief Shows a list of pinned posts for a channel
 * @author Lyubomir Filipov
 * @date Apr 1, 2023
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Mattermost-QT is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with Mattermost-QT. if not, see https://www.gnu.org/licenses/.
 */

#include "PinnedPostsList.h"

#include <QDockWidget>
#include <QHBoxLayout>
#include <QKeySequence>
#include <QLabel>
#include <QPointer>
#include <QShortcut>
#include <QToolButton>
#include <QVBoxLayout>

#include "ChatArea.h"
#include "backend/PostNavigationService.h"
#include "post/PostWidget.h"
#include "ui_PinnedPostsList.h"

namespace Mattermost {

PinnedPostsList::PinnedPostsList(QWidget *parent)
:QWidget(parent)
,ui(new Ui::PinnedPostsList)
,chatArea(qobject_cast<ChatArea*>(parent))
{
    ui->setupUi(this);

    auto* header = new QWidget(this);
    auto* headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(4, 2, 2, 2);
    headerLayout->setSpacing(4);
    headerLayout->addWidget(new QLabel(tr("Pinned messages"), header));
    headerLayout->addStretch();

    auto* closeButton = new QToolButton(header);
    closeButton->setText(QStringLiteral("×"));
    closeButton->setToolTip(tr("Close"));
    closeButton->setAutoRaise(true);
    headerLayout->addWidget(closeButton);
    ui->verticalLayout->insertWidget(0, header);

    connect(closeButton, &QToolButton::clicked, this, &PinnedPostsList::closePanel);

    auto* escapeShortcut = new QShortcut(QKeySequence(Qt::Key_Escape), this);
    escapeShortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escapeShortcut, &QShortcut::activated, this, &PinnedPostsList::closePanel);
}

PinnedPostsList::~PinnedPostsList()
{
    delete ui;
}

void PinnedPostsList::closePanel()
{
    QWidget* widget = parentWidget();
    while (widget) {
        if (auto* dock = qobject_cast<QDockWidget*>(widget)) {
            // ChatArea stores the dock in a QPointer, so deleting the floating
            // panel also clears the owner's handle safely.
            dock->deleteLater();
            return;
        }
        widget = widget->parentWidget();
    }

    hide();
}

void PinnedPostsList::addPost (PostWidget* postWidget)
{
    if (!postWidget) {
        return;
    }

    const QString postId = postWidget->post.id;
    // Replies are not rendered as normal channel rows. For those pins navigate
    // to the root message instead of silently failing to find the hidden reply.
    const QString navigationId = postWidget->post.root_id.isEmpty()
        ? postId : postWidget->post.root_id;

    auto* rowWidget = new QWidget(ui->listWidget);
    auto* rowLayout = new QVBoxLayout(rowWidget);
    rowLayout->setContentsMargins(0, 0, 0, 4);
    rowLayout->setSpacing(2);

    auto* actionsLayout = new QHBoxLayout;
    actionsLayout->setContentsMargins(0, 0, 2, 0);
    actionsLayout->addStretch();

    auto* goToButton = new QToolButton(rowWidget);
    goToButton->setText(tr("Go to message"));
    goToButton->setToolTip(tr("Show this message in the channel"));
    actionsLayout->addWidget(goToButton);

    rowLayout->addLayout(actionsLayout);
    rowLayout->addWidget(postWidget);

    QListWidgetItem* newItem = new QListWidgetItem();
    ui->listWidget->addItem (newItem);
    ui->listWidget->setItemWidget (newItem, rowWidget);

    connect(goToButton, &QToolButton::clicked, this, [this, navigationId] {
        if (!chatArea || navigationId.isEmpty()) {
            closePanel();
            return;
        }

        QPointer<ChatArea> guard(chatArea);
        BackendChannel& channel = chatArea->getChannel();

        // Set pendingPostId first. If the post is old, loadAround() emits the
        // normal channel onNewPosts signal while merging context; ChatArea will
        // then complete the pending navigation as soon as the row is inserted.
        chatArea->goToPost(navigationId);
        if (!channel.postIdToPost.contains(navigationId)) {
            PostNavigationService::instance(chatArea->getBackend()).loadAround(
                channel, navigationId, [guard, navigationId](bool loaded) {
                    if (guard && loaded) {
                        guard->goToPost(navigationId);
                    }
                });
        }

        closePanel();
    });
}

} /* namespace Mattermost */
