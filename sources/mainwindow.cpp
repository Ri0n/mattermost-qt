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

#include "mainwindow.h"

#include <algorithm>

#include <QCloseEvent>
#include <QMessageBox>
#include <QSettings>
#include <QSplitter>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QTreeWidgetItem>
#include <QVBoxLayout>
#include <QWindow>

#include "./ui_mainwindow.h"
#include "SettingsWindow.h"
#include "backend/Backend.h"
#include "backend/SidebarService.h"
#include "build-config.h"
#include "channel-tree/ChannelQuickList.h"
#include "chat-area/ChatArea.h"
#include "log.h"
#include "notifications/NotificationManager.h"

namespace Mattermost {

MainWindow::MainWindow (QWidget *parent, QSystemTrayIcon& trayIcon, Backend& _backend)
:QMainWindow(parent)
,ui (std::make_unique<Ui::MainWindow>())
,trayIcon (trayIcon)
,notificationManager (std::make_unique<NotificationManager>(trayIcon))
,chooseEmojiDialog (this)
,backend (_backend)
,currentTeamRestoredFromSettings (false)
,doDeinit (false)
{
	LOG_DEBUG ("MainWindow create start");

	ui->setupUi(this);
	setupChannelTabs ();
	ui->channelList->setChatAreaStackedWidget (ui->chatAreaStackedWidget);
	ui->channelList->setFocus();

	createMenu ();
	connect (notificationManager.get(), &NotificationManager::activated,
	         this, &MainWindow::activateNotification);

	const BackendUser& currentUser = backend.getLoginUser();

	if (currentUser.id.isEmpty()) {
		ui->usericon_label->clear();
		qCritical() << "Current User's ID is empty string";
		return;
	}

	auto& sidebar = SidebarService::instance(backend);
	connect(&sidebar, &SidebarService::channelActivityChanged, this,
	        [this](const QString&) { refreshChannelQuickLists(); });
	connect(&sidebar, &SidebarService::channelActivityReset,
	        this, &MainWindow::refreshChannelQuickLists);
	connect(&sidebar, &SidebarService::categoriesChanged, this,
	        [this](const QString&) { refreshChannelQuickLists(); });

	// Do not wait for the server's channel_viewed websocket echo to update the
	// local Recent/Unreads indexes. A channel becomes viewed as soon as the user
	// selects it. Defer the update by one event-loop turn so a quick-list refresh
	// cannot delete the item while its selection signal is still being handled.
	connect(ui->channelList, &QTreeWidget::currentItemChanged, this,
	        [this](QTreeWidgetItem* current, QTreeWidgetItem*) {
		if (!current || current->data(0, ChannelTree::ItemKindRole).toInt() != ChannelTree::ChannelItemKind) {
			return;
		}
		const QString channelId = current->data(0, ChannelTree::ItemIdRole).toString();
		if (channelId.isEmpty()) {
			return;
		}
		QTimer::singleShot(0, this, [this, channelId] {
			if (BackendChannel* channel = backend.getStorage().getChannelById(channelId)) {
				SidebarService::instance(backend).markChannelViewedLocally(*channel);
			}
		});
	});

	recentChannels->initialize(backend, ChannelQuickList::Recent);
	unreadChannels->initialize(backend, ChannelQuickList::Unreads);

	sidebar.clear();
	sidebar.retrieveChannelMemberships();

	connect (&currentUser, &BackendUser::onStatusChanged, [this, &currentUser] {
		ui->statusLabel->setText (currentUser.status);
	});

	ui->usernameLabel->setText (currentUser.username);

	connect (&currentUser, &BackendUser::onAvatarChanged, [this, &currentUser] {
		LOG_DEBUG ("Got User Image");
		ui->usericon_label->setPixmap (currentUser.avatar);
	});

	/*
	 * Gets the LoginUser's image for the user icon
	 */
	//backend.retrieveUserAvatar (currentUser.id);

	backend.retrieveTotalUsersCount ([this] (uint32_t) {
		backend.retrieveKnownUsers ([this]() {
				backend.retrieveAllUsers ();
			}
		);
	});

	/*
	 * Register for signals
	 */
	//connect (ui->channelList, &QTreeWidget::currentItemChanged, this, &MainWindow::channelListWidget_itemClicked);

	//getAllUsers is called from onShowEvent()
	connect (&backend, &Backend::onAllUsers, [this]() {
		/*
		 * Adds each team in which the LoginUser participates.
		 * The callback is called once for each team
		 */
		backend.retrieveOwnTeams ([this](BackendTeam& team) {
			ui->channelList->addTeam (backend, team);
		});
	});

	/*
	 * The Mattermost sidebar categories are per-user and per-team. Wait until all
	 * channels (including DM/GM channels duplicated by the server across teams)
	 * are in storage, then build each team's sidebar from the server category list.
	 */
	connect (&backend, &Backend::onAllTeamChannelsPopulated, [this] {
		ui->channelList->populateSidebars (backend);
		initializationComplete ();
	});

	connect (&backend, &Backend::onNewPost, [this] (BackendChannel& channel, const BackendPost& post) {
		this->messageNotify (channel, post);
	});

	connect (&backend, &Backend::onChannelViewed, [this] (const BackendChannel& channel) {
		SidebarService::instance(backend).setChannelMentioned(channel.id, false);
		if (channelsWithNewPosts.remove (&channel)) {
			setNotificationsCountVisualization (channelsWithNewPosts.size());
		}
	});

	/*
	 * onAddedToTeam comes after a WebSocket event, when the user is added to (new) team
	 */
	connect (&backend, &Backend::onAddedToTeam, [this](BackendTeam& team) {
		ui->channelList->addTeam (backend, team);
	});

	/*
	 * On new post - set window and tray notifications
	 */
	connect (&backend, &Backend::onUnreadPostsAtStartup, this, &MainWindow::unreadMessagesNotify);

	LOG_DEBUG ("MainWindow signal register finish");

	//Restore saved window position, dimensions and the user-selected sidebar width.
	QSettings settings;
	restoreGeometry (settings.value("geometry", saveGeometry()).toByteArray());
	const QByteArray splitterState = settings.value("sidebar_splitter_state").toByteArray();
	if (sidebarSplitter && !splitterState.isEmpty()) {
		sidebarSplitter->restoreState(splitterState);
	} else if (sidebarSplitter) {
		sidebarSplitter->setSizes({280, std::max(360, width() - 280)});
	}

	connect (qApp, &QApplication::aboutToQuit, this, &MainWindow::saveState);
	LOG_DEBUG ("MainWindow create finish");
}

MainWindow::~MainWindow()
{
}

static QString infoText (QString ("Version " PROJECT_VER "<br/>"
"An unofficial Mattermost Client, using the QT framework<br/>") +
R"(
<br/>
More information:<br/> 
<a href='https://github.com/nuclear868/mattermost-qt'>https://github.com/nuclear868/mattermost-qt</a>
<br/>
<br/>
Mattermost QT Copyright 2021, 2022 Lyubomir Filipov<br/>
<br/>
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Lesser General Public License for more details.<br/>
<br/>
You should have received a copy of the GNU Lesser General Public License
along with Mattermost-QT. if not, see <a href='https://www.gnu.org/licenses/'>https://www.gnu.org/licenses/</a>.<br/>
)");

void MainWindow::setupChannelTabs ()
{
	ui->gridLayout_2->removeWidget(ui->lefttop_frame);
	ui->gridLayout_2->removeWidget(ui->channelList);
	ui->gridLayout_2->removeWidget(ui->chatAreaStackedWidget);

	channelTabs = new QTabWidget(ui->centralwidget);
	channelTabs->setDocumentMode(true);
	channelTabs->setSizePolicy(ui->channelList->sizePolicy());
	channelTabs->setStyleSheet(QStringLiteral("QTabWidget::pane { border: 0; }"));

	auto* channelsPage = new QWidget(channelTabs);
	auto* channelsLayout = new QVBoxLayout(channelsPage);
	channelsLayout->setContentsMargins(0, 0, 0, 0);
	channelsLayout->setSpacing(0);
	channelsLayout->addWidget(ui->channelList);

	recentChannels = new ChannelQuickList(channelTabs);
	unreadChannels = new ChannelQuickList(channelTabs);

	channelTabs->addTab(channelsPage, QStringLiteral("Channels"));
	channelTabs->addTab(recentChannels, QStringLiteral("Recent"));
	channelTabs->addTab(unreadChannels, QStringLiteral("Unreads"));

	auto* leftSidebar = new QWidget(ui->centralwidget);
	auto* leftLayout = new QVBoxLayout(leftSidebar);
	leftLayout->setContentsMargins(0, 0, 0, 0);
	leftLayout->setSpacing(0);
	leftLayout->addWidget(ui->lefttop_frame);
	leftLayout->addWidget(channelTabs, 1);

	sidebarSplitter = new QSplitter(Qt::Horizontal, ui->centralwidget);
	sidebarSplitter->setChildrenCollapsible(false);
	sidebarSplitter->setHandleWidth(4);
	sidebarSplitter->setOpaqueResize(true);
	sidebarSplitter->addWidget(leftSidebar);
	sidebarSplitter->addWidget(ui->chatAreaStackedWidget);
	sidebarSplitter->setStretchFactor(0, 0);
	sidebarSplitter->setStretchFactor(1, 1);

	ui->gridLayout_2->addWidget(sidebarSplitter, 0, 0, 2, 2);
	ui->gridLayout_2->setRowStretch(0, 1);
	ui->gridLayout_2->setRowStretch(1, 0);
	ui->gridLayout_2->setColumnStretch(0, 1);

	connect(recentChannels, &ChannelQuickList::channelSelected,
	        ui->channelList, &ChannelTree::openChannel);
	connect(unreadChannels, &ChannelQuickList::channelSelected,
	        ui->channelList, &ChannelTree::openChannel);
}

void MainWindow::refreshChannelQuickLists ()
{
	if (recentChannels) {
		recentChannels->refresh();
	}
	if (unreadChannels) {
		unreadChannels->refresh();
	}
}

void MainWindow::createMenu ()
{
	mainMenu = new QMenu (ui->toolButton);

	QMenu* fileMenu = mainMenu->addMenu ("File");
	fileMenu->addAction ("Logout", [this] {

		backend.logout ([this] {
			doDeinit = true;
			QMainWindow::close ();
			LOG_DEBUG ("Logout done");
		});
	});

	mainMenu->addAction ("Settings", [this] {
		settingsWindow = new SettingsWindow (this);

		connect (settingsWindow, &QDialog::accepted, [this] {

			if (QMessageBox::question (this, "Reload?", "In order to apply some settings, Mattermost has to be reloaded.\n"
					" Do you want to reload now? (If no, settings will be applied on the next startup)") == QMessageBox::Yes) {

				settingsWindow->applyNewSettings ();
			}
			reload ();
		});

		settingsWindow->show();
	});


	QMenu* helpMenu = mainMenu->addMenu ("Help");
	helpMenu->addAction ("About Mattermost", [this] {
		QMessageBox *msgBox = new QMessageBox (QMessageBox::Information,
				"About Mattermost", infoText);

		msgBox->setIconPixmap (windowIcon().pixmap (QSize (64, 64)));
		msgBox->setTextFormat(Qt::RichText);
		msgBox->setStandardButtons(QMessageBox::Ok);
		msgBox->setDefaultButton(QMessageBox::Ok);
		msgBox->open();
	});

	helpMenu->addAction ("About QT", [this] {
		QMessageBox::aboutQt (this, "About QT");
	});

	ui->toolButton->setMenu(mainMenu);
}

void MainWindow::moveEvent (QMoveEvent*)
{

	ChatArea* currentPage = ui->channelList->getCurrentPage();

	if (currentPage) {
		currentPage->onMove (pos());
	}
}

void MainWindow::dragMoveEvent (QDragMoveEvent*)
{
	qDebug() << "dragMove " << mapToGlobal(pos());
}

void MainWindow::reload ()
{
	QTimer::singleShot(0, [this] {
		backend.reset();
		doDeinit = true;
		QMainWindow::close ();
	});
}

void MainWindow::changeEvent (QEvent* event)
{
	QWidget::changeEvent(event);

	if (event->type() == QEvent::ActivationChange) {
		if (isActiveWindow()) {
			//qDebug() << "Activated";

			ChatArea* currentPage = ui->channelList->getCurrentPage();

			if (currentPage) {
				auto& sidebar = SidebarService::instance(backend);
				sidebar.setChannelMentioned(currentPage->getChannel().id, false);
				sidebar.markChannelViewedLocally(currentPage->getChannel());
				currentPage->onMainWindowActivate ();

				if (channelsWithNewPosts.remove (&currentPage->getChannel())) {
					setNotificationsCountVisualization (channelsWithNewPosts.size());
				}
			}

		} else {
			//qDebug() << "Deactivated";
		}
	} else {
		qDebug() << event->type();
	}
}

void MainWindow::closeEvent(QCloseEvent *event)
{
	qDebug() << "closeEvent";

	if (doDeinit) {
		qDebug() << "QMainWindow closeEvent";
		saveState ();
		return QMainWindow::closeEvent (event);
	}

	if (trayIcon.isVisible()) {
		hide();
		event->ignore();
	}
}

void MainWindow::initializationComplete ()
{
	LOG_DEBUG ("MainWindow initialization comlete");

	/*
	 * No team was restored from settings. Either there was no team saved, or the saved team
	 * was deleted. In all cases, activate the first team
	 */
	if (!currentTeamRestoredFromSettings) {
	//	ui->teamComboBox->setCurrentIndex (0);
		//channelList.activateTeam (0);
	}
}

void MainWindow::messageNotify (BackendChannel& channel, const BackendPost& post)
{
	//do not receive notifications for your own messages ;)
	if (post.author && post.author->id == backend.getLoginUser().id) {
		return;
	}

	auto& sidebar = SidebarService::instance(backend);
	if (post.currentUserMentioned) {
		sidebar.setChannelMentioned(channel.id, true);
	}

	// Mattermost mute disables all desktop/email/push notifications for the channel.
	if (sidebar.isChannelMuted(channel)) {
		return;
	}

	// Replies in a thread are intentionally quiet unless Mattermost marked this
	// websocket event as a mention for the current user. Following a thread is
	// server-side state, but it must not turn every followed reply into a popup.
	if (!post.root_id.isEmpty() && !post.currentUserMentioned) {
		return;
	}

	if (post.root_id.isEmpty()) {
		/**
		 * If the Mattermost window is active (has focus) and the current channel is active,
		 * do not add notifications. We assume that the user is watching the chat window.
		 */
		if (isActiveWindow() && ui->channelList->isChannelActive (channel)) {
			sidebar.setChannelMentioned(channel.id, false);
			sidebar.markChannelViewedLocally(channel);
			backend.markChannelAsViewed(channel);
			return;
		}
	} else {
		// A thread is a separate top-level window. Do not notify if that exact
		// thread is already visible and focused.
		ChatArea* parentArea = ui->channelList->getCurrentPage();
		if (parentArea && &parentArea->getChannel() == &channel) {
			for (ChatArea* threadArea : parentArea->threadsAreas) {
				if (threadArea && threadArea->root_id == post.root_id && threadArea->isActiveWindow()) {
					sidebar.setChannelMentioned(channel.id, false);
					sidebar.markChannelViewedLocally(channel);
					backend.markChannelAsViewed(channel);
					return;
				}
			}
		}
	}

	//Add a desktop notification
	QString title;

	if (!post.root_id.isEmpty()) {
		title = post.getDisplayAuthorName () + " mentioned you in a thread";
	} else if (channel.type == BackendChannel::directChannel) {
		title = post.getDisplayAuthorName () + " messaged you";
	} else {
		title = post.getDisplayAuthorName () + " posted in '" + channel.display_name + "'";
	}

	notificationManager->show(title, post.message,
	                          NotificationTarget {channel.id, post.id, post.root_id});
	qApp->alert (nullptr, 0);

	//update the count of new channels in the taskbar and tray icon
	channelsWithNewPosts.insert(&channel);
	setNotificationsCountVisualization (channelsWithNewPosts.size());
}

void MainWindow::activateNotification (const NotificationTarget& target)
{
	if (!target.isValid()) {
		return;
	}

	if (isMinimized()) {
		showNormal();
	} else {
		show();
	}
	raise();
	activateWindow();

	BackendChannel* channel = backend.getStorage().getChannelById(target.channelId);
	if (!channel) {
		return;
	}

	ui->channelList->openChannel(target.channelId);
	ChatArea* parentArea = ui->channelList->getCurrentPage();
	if (!parentArea || &parentArea->getChannel() != channel) {
		return;
	}

	if (target.rootId.isEmpty()) {
		parentArea->goToPost(target.postId);
		return;
	}

	ChatArea* threadArea = nullptr;
	for (ChatArea* area : parentArea->threadsAreas) {
		if (area && area->root_id == target.rootId) {
			threadArea = area;
			break;
		}
	}

	if (!threadArea) {
		threadArea = new ChatArea(backend, *channel, target.rootId, parentArea);
		parentArea->threadsAreas.insert(threadArea);
	}

	threadArea->show();
	threadArea->raise();
	threadArea->activateWindow();
	threadArea->goToPost(target.postId);
}

void MainWindow::unreadMessagesNotify (const BackendChannel& channel)
{
	if (SidebarService::instance(backend).isChannelMuted(channel)) {
		return;
	}

	//update the count of new channels in the taskbar and tray icon
	channelsWithNewPosts.insert(&channel);
	setNotificationsCountVisualization (channelsWithNewPosts.size());
}

void MainWindow::setNotificationsCountVisualization (uint32_t notificationsCount)
{
	//set the count in the window's taskbar element
	if (notificationsCount == 0) {
		setWindowTitle (qApp->applicationName());
	} else {
		setWindowTitle ("(" + QString::number (channelsWithNewPosts.size()) + ") " + qApp->applicationName());
	}

	//set the count in the tray icon
	notificationsCount = std::min (notificationsCount, 6u);
	QString iconName (":/icons/img/icon" + QString::number(notificationsCount) + ".ico");
	trayIcon.setIcon(QIcon(iconName));
}

void MainWindow::saveState ()
{
	LOG_DEBUG ("MainWindow saveState");
	QSettings settings;
	settings.setValue ("geometry", saveGeometry());
	if (sidebarSplitter) {
		settings.setValue("sidebar_splitter_state", sidebarSplitter->saveState());
	}
//	settings.setValue ("current_team", channelList.getCurrentTeamId());
//	if (currentPage) {
//		settings.setValue ("current_channel", currentPage->getChannel().id);
//	}
}

} /* namespace Mattermost */

