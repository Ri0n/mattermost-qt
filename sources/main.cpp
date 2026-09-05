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

#include <memory>
#include <QApplication>
#include <QEvent>
#include <QMenu>
#include <QPalette>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif

#include "login/LoginDialog.h"
#include "mainwindow.h"
#include "backend/Backend.h"
#include "config/Config.h"

namespace Mattermost {

class MattermostApplication: public QApplication {
public:
	MattermostApplication (int& argc, char *argv[]);

	void openLoginWindow ();
	void showWindow ();
	void toggleShowWindow ();
	void reopen ();

protected:
    bool event(QEvent* event) override;

private:
    void refreshTopLevelPalettes();

	std::unique_ptr<MainWindow>			mainWindow;
	std::unique_ptr<QSystemTrayIcon> 	trayIcon;
	std::unique_ptr<QMenu>				trayIconMenu;
	Backend								backend;
	LoginDialog*						loginDialog;
	QWidget*							currentWindow;
};

inline MattermostApplication::MattermostApplication (int& argc, char *argv[])
:QApplication (argc, argv)
,trayIcon (std::make_unique<QSystemTrayIcon> (QIcon(":/icons/img/icon0.ico"), nullptr))
,trayIconMenu (std::make_unique<QMenu> (nullptr))
,currentWindow (nullptr)
{
    Config::init ();
	trayIcon->setToolTip(tr("Mattermost Qt"));
	trayIcon->setContextMenu (trayIconMenu.get());
	trayIcon->show();

	connect (trayIcon.get(), &QSystemTrayIcon::messageClicked, this, &MattermostApplication::showWindow);

	connect (trayIcon.get(), &QSystemTrayIcon::activated, [this] (QSystemTrayIcon::ActivationReason reason) {
		if (reason == QSystemTrayIcon::Trigger) {
			toggleShowWindow ();
		}
	});

	trayIconMenu->addAction ("Open Mattermost", this, &MattermostApplication::showWindow);
	trayIconMenu->addAction ("Quit", qApp, &QApplication::quit);
	qApp->setQuitOnLastWindowClosed(false);

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    connect(styleHints(), &QStyleHints::colorSchemeChanged, this,
            [this](Qt::ColorScheme) {
        // The signal is emitted while the previous application palette is still
        // active. Defer until Qt has installed the new system palette.
        QTimer::singleShot(0, this, &MattermostApplication::refreshTopLevelPalettes);
    });
#endif
}

bool MattermostApplication::event(QEvent* event)
{
    const bool paletteChanged = event && event->type() == QEvent::ApplicationPaletteChange;
    const bool handled = QApplication::event(event);

    if (paletteChanged) {
        QTimer::singleShot(0, this, &MattermostApplication::refreshTopLevelPalettes);
    }
    return handled;
}

void MattermostApplication::refreshTopLevelPalettes()
{
    const QWidgetList windows = QApplication::topLevelWidgets();
    for (QWidget* window : windows) {
        if (!window) {
            continue;
        }

        // Top-level widgets must inherit the freshly installed application
        // palette. Do not copy QApplication::palette() here: that would resolve
        // all roles explicitly and freeze this theme for the next switch.
        window->setPalette(QPalette());
        window->update();

        // Palette propagation changes the effective roles immediately, but some
        // QWidget/QAbstractScrollArea children keep their old backing-store
        // pixels until another interaction (notably scrolling) exposes them.
        // Explicitly invalidate the existing widget tree; this does not alter
        // layout, materialization or any intentional child palette overrides.
        const auto children = window->findChildren<QWidget*>();
        for (QWidget* child : children) {
            if (child) {
                child->update();
            }
        }
    }
}

void MattermostApplication::openLoginWindow ()
{
	loginDialog = new LoginDialog (nullptr, backend);
	loginDialog->open();
	currentWindow = loginDialog;

	connect (loginDialog, &LoginDialog::accepted, [this] {
		//create Main Window and open it, after successful login
		loginDialog = nullptr;
		mainWindow = std::make_unique<MainWindow> (nullptr, *trayIcon, backend);
		mainWindow->show();
		currentWindow = mainWindow.get();
	});
}

inline void MattermostApplication::showWindow ()
{
	if (currentWindow && !currentWindow->isVisible()) {
		currentWindow->show ();
	}
}

inline void MattermostApplication::toggleShowWindow ()
{
	if (!currentWindow) {
		return;
	}

	if (currentWindow->isVisible()) {
		currentWindow->hide ();
	} else {
		currentWindow->show ();
	}
}

} /* namespace Mattermost */

int main( int argc, char *argv[])
{
	QCoreApplication::setOrganizationName("mattermost-native");
	QCoreApplication::setApplicationName("Mattermost");
	QGuiApplication::setHighDpiScaleFactorRoundingPolicy(Qt::HighDpiScaleFactorRoundingPolicy::Round);

	Mattermost::MattermostApplication app (argc, argv);
	app.openLoginWindow ();
	return app.exec();
}
