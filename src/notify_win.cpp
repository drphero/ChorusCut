#include "notify_platform.h"
#include <QApplication>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QGuiApplication>
#include <QIcon>
#include <QSystemTrayIcon>
#include <QUrl>
#include <QWindow>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// ---------------------------------------------------------------- Windows side
// A notification-area icon stands in for Notification Center: balloons carry the title/body,
// MessageBeep carries the sound, and clicking a balloon reports the last notification's owner
// like clicking a macOS notification. Balloons can't be tagged; a second balloon while one is
// up replaces it, which is the roll-up the tag would have done anyway.
namespace {
QSystemTrayIcon* tray = nullptr;
std::function<void(const QString&, int)> clickHandler;
QString lastOwner;
int lastOwnerId = 0;

void raiseWindows() {
    for (QWindow* w : QGuiApplication::topLevelWindows()) { w->showNormal(); w->raise(); w->requestActivate(); }
}

QSystemTrayIcon* trayIcon() {
    if (!tray) {
        tray = new QSystemTrayIcon(QIcon(":/resources/icon/ChorusCut.ico"), qApp);
        QObject::connect(tray, &QSystemTrayIcon::messageClicked, qApp, [] {
            raiseWindows();
            if (clickHandler) clickHandler(lastOwner, lastOwnerId);
        });
        QObject::connect(tray, &QSystemTrayIcon::activated, qApp, [](QSystemTrayIcon::ActivationReason reason) {
            if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) raiseWindows();
        });
        tray->show();
    }
    return tray;
}
}

namespace NotifyPlatform {

// Tray icons need a QApplication (Widgets); under QGuiApplication test binaries we report
// unavailable the same way unbundled macOS binaries do.
bool available() {
    return qobject_cast<QApplication*>(QCoreApplication::instance())
        && QSystemTrayIcon::isSystemTrayAvailable() && QSystemTrayIcon::supportsMessages();
}

void status(std::function<void(QString)> done) {
    // Windows gives tray balloons no per-app permission to query.
    done(available() ? "allowed" : "unavailable");
}

void request(std::function<void(QString, QString)> done) {
    status([done](QString state) { done(state, {}); });
}

void post(const QString& title, const QString& body, bool sound, const QString& owner, int ownerId, const QString&) {
    if (!available()) return;
    lastOwner = owner;
    lastOwnerId = ownerId;
    if (sound) MessageBeep(MB_ICONASTERISK);
    trayIcon()->showMessage(title, body, QSystemTrayIcon::Information, 10000);
}

void openSettings() {
    QDesktopServices::openUrl(QUrl("ms-settings:notifications"));
}

void onActivated(std::function<void(const QString&, int)> handler) { clickHandler = std::move(handler); }

}
