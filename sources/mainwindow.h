#pragma once

#include <memory>
#include <QMainWindow>
#include <QSet>
#include "choose-emoji-dialog/ChooseEmojiDialogWrapper.h"

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class QSplitter;
class QSystemTrayIcon;
class QTabWidget;
class QTreeWidgetItem;

namespace Mattermost {

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

	void changeEvent (QEvent* event) override;

	/**
	 * Called when the window's close buttons is clicked
	 * @param event event
	 */
	void closeEvent(QCloseEvent *event) override;

	/**
	 * Called when the window is about to be closed, either because of logout or reload action or quit action from the tray menu.
	 * In all cases it's state has to be saved
	 */
	void saveState ();

	/**
	 * Called when a new post is received while the Mattermost client is on
	 * @param channel channel
	 * @param post post
	 */
	void messageNotify (BackendChannel& channel, const BackendPost& post);

	/**
	 * Called on Mattermost client startup, when there were new posts, while the client was not open
	 * @param channel channel
	 */
	void unreadMessagesNotify (const BackendChannel& channel);
	void setNotificationsCountVisualization (uint32_t notificationsCount);

	void moveEvent (QMoveEvent* event) override;
	void dragMoveEvent (QDragMoveEvent* event) override;
private:
	void createMenu ();
	void reload ();
	void activateNotification (const NotificationTarget& target);
	void setupChannelTabs ();
	void refreshChannelQuickLists ();
private:
	std::unique_ptr<Ui::MainWindow>		ui;
	QSystemTrayIcon&					trayIcon;
	std::unique_ptr<NotificationManager>	notificationManager;
	ChooseEmojiDialogWrapper			chooseEmojiDialog;
	QSet<const BackendChannel*>			channelsWithNewPosts;
	Backend&							backend;
	QSplitter*							sidebarSplitter = nullptr;
	QTabWidget*							channelTabs = nullptr;
	ChannelQuickList*					recentChannels = nullptr;
	ChannelQuickList*					unreadChannels = nullptr;
	bool								currentTeamRestoredFromSettings;
	QMenu*								mainMenu;
	SettingsWindow*						settingsWindow;
	bool								doDeinit;
};

} /* namespace Mattermost */
