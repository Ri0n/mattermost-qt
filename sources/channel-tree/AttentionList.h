/**
 * @file AttentionList.h
 * @brief Personal attention projection of the shared Following model.
 *
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 */

#pragma once

#include <cstdint>
#include <optional>

#include <QTreeWidget>

#include "SidebarItem.h"
#include "backend/FollowingModel.h"

class QMouseEvent;

namespace Mattermost {

class Backend;

class AttentionList : public QTreeWidget
{
    Q_OBJECT
public:
    explicit AttentionList(QWidget* parent = nullptr);

    void initialize(Backend& backend);
    void refresh();
    void refreshThreads();
    void releaseSelectionRetention();

    QString channelIdAt(const QPoint& pos) const
    {
        QTreeWidgetItem* item = itemAt(pos);
        return item ? item->data(0, SidebarItem::ChannelIdRole).toString() : QString();
    }

signals:
    void channelSelected(const QString& channelId);
    void attentionCountChanged(uint32_t count);

protected:
    void mousePressEvent(QMouseEvent* event) override;

private:
    void activateItem(QTreeWidgetItem* item);
    void retainSelection(QTreeWidgetItem* item);
    void openThread(const FollowingModel::Entry& entry);
    QString threadLabel(const FollowingModel::Entry& entry) const;

    Backend* backend_ = nullptr;
    FollowingModel* model_ = nullptr;

    // Presentation-only snapshot for keeping the selected row visible.
    // Navigation always resolves the current cursor from FollowingModel.
    std::optional<FollowingModel::Entry> retainedEntry_;

    bool refreshing_ = false;
    int lastAttentionCount_ = -1;
};

} // namespace Mattermost
