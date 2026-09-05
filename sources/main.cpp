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

#include <QAbstractScrollArea>
#include <QApplication>
#include <QEvent>
#include <QLoggingCategory>
#include <QMenu>
#include <QPalette>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <QWidget>

#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
#include <QStyleHints>
#endif

#include "backend/Backend.h"
#include "config/Config.h"
#include "login/LoginDialog.h"
#include "mainwindow.h"

namespace Mattermost {

namespace {

Q_LOGGING_CATEGORY(lcThemeTrace, "mattermost.theme.trace", QtWarningMsg)

QString paletteSummary(const QPalette& palette)
{
    return QStringLiteral("Window=%1 WindowText=%2 Base=%3 Text=%4 Button=%5 ButtonText=%6")
        .arg(palette.color(QPalette::Window).name(),
             palette.color(QPalette::WindowText).name(),
             palette.color(QPalette::Base).name(),
             palette.color(QPalette::Text).name(),
             palette.color(QPalette::Button).name(),
             palette.color(QPalette::ButtonText).name());
}

} // namespace

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
    void traceThemeState(const char* stage) const;

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
        // Qt documents that colorSchemeChanged is emitted while the old palette
        // is still active. Observe the immediate and deferred states instead of
        // modifying them here; this lets us distinguish a stale application
        // palette from stale per-widget palette resolution.
        traceThemeState("color-scheme-signal");
        QTimer::singleShot(0, this, [this] { traceThemeState("color-scheme+0ms"); });
        QTimer::singleShot(100, this, [this] { traceThemeState("color-scheme+100ms"); });
    });
#endif
}

bool MattermostApplication::event(QEvent* event)
{
    const bool paletteChanged = event && event->type() == QEvent::ApplicationPaletteChange;
    const bool handled = QApplication::event(event);

    if (paletteChanged) {
        traceThemeState("application-palette-change");
        QTimer::singleShot(0, this, [this] { traceThemeState("application-palette+0ms"); });
    }
    return handled;
}

void MattermostApplication::traceThemeState(const char* stage) const
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
    const int scheme = static_cast<int>(styleHints()->colorScheme());
#else
    const int scheme = -1;
#endif

    qCDebug(lcThemeTrace).noquote()
        << "THEME" << stage
        << "scheme=" << scheme
        << "style=" << (style() ? style()->objectName() : QStringLiteral("<none>"))
        << "app" << paletteSummary(QApplication::palette());

    const QWidgetList windows = QApplication::topLevelWidgets();
    for (QWidget* window : windows) {
        if (!window) {
            continue;
        }
        qCDebug(lcThemeTrace).noquote()
            << "THEME_WINDOW" << stage
            << window->metaObject()->className()
            << "object=" << window->objectName()
            << paletteSummary(window->palette());

        const auto scrollAreas = window->findChildren<QAbstractScrollArea*>();
        for (QAbstractScrollArea* area : scrollAreas) {
            if (!area || !area->viewport()) {
                continue;
            }
            qCDebug(lcThemeTrace).noquote()
                << "THEME_VIEWPORT" << stage
                << area->metaObject()->className()
                << "object=" << area->objectName()
                << "area" << paletteSummary(area->palette())
                << "viewport" << paletteSummary(area->viewport()->palette());
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
