/**
 * Copyright 2021, 2022 Lyubomir Filipov
 *
 * This file is part of Mattermost-QT.
 *
 * Mattermost-QT is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
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

#pragma once

#include <memory>
#include <QMainWindow>
#include <QStringList>
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class QLineEdit;
class QSplitter;
class QSystemTrayIcon;
class QTabWidget;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace Mattermost {

class AttentionList;
class ChatArea;
class Backend;
class BackendChannel;
class BackendPost;
class BackendTeam;
class ChannelQuickList;
class NotificationManager;
class SettingsWindow;
struct NotificationTarget;

class MainWindow: public QMainWindow {
	Q_OBJECT
public:
	MainWindow (QWidget *parent, QSystemTrayIcon& trayIcon, Backend& backend);
	~MainWindow();
public:
	void initializationComplete ();
	void openChannelPost(const QString& channelId,
	                     const QString& postId = QString(),
	                     const QString& rootId = QString(),
	                     const QStringList& contextPostIds = QStringList(),
	                     bool reachedOldest = false,
	                     bool reachedNewest = false);

	void changeEvent (QEvent* event) override;
	void closeEvent(QCloseEvent *event) override;
	void saveState ();
	void messageNotify (BackendChannel& channel, const BackendPost& post);
	void unreadMessagesNotify (const BackendChannel& channel);
	void setNotificationsCountVisualization (uint32_t notificationsCount);
	void moveEvent (QMoveEvent* event) override;
	void dragMoveEvent (QDragMoveEvent* event) override;
private:
	void createMenu ();
	void reload ();
	void activateNotification (const NotificationTarget& target);
	void setupChannelTabs ();
	void refreshSidebarViews ();
	void refreshChannelUnreadFilter ();
	void applySidebarTextFilter(QTreeWidget* tree) const;
	void openDirectMessageSearch ();
	void openAttentionThread (const QString& channelId, const QString& rootPostId);
private:
	std::unique_ptr<Ui::MainWindow>		ui;
	QSystemTrayIcon&					trayIcon;
	std::unique_ptr<NotificationManager>	notificationManager;
	ChooseEmojiDialogWrapper			chooseEmojiDialog;
	Backend&							backend;
	QSplitter*							sidebarSplitter = nullptr;
	QTabWidget*							channelTabs = nullptr;
	QWidget*							channelsPage = nullptr;
	QLineEdit*							sidebarFilterEdit = nullptr;
	QToolButton*						unreadFilterButton = nullptr;
	ChannelQuickList*					recentChannels = nullptr;
	AttentionList*						attentionList = nullptr;
	QString								retainedUnreadFilterChannelId;
	bool								currentTeamRestoredFromSettings;
	QMenu*								mainMenu;
	SettingsWindow*						settingsWindow;
	bool								doDeinit;
};

} /* namespace Mattermost */
