#pragma once
#include <QString>
#include <functional>

// Per-OS notification plumbing, one translation unit each (notify_mac.mm, notify_win.cpp,
// notify_stub.cpp). Called from the GUI thread only; the shared Notifier in notify.cpp does
// everything above this seam.
namespace NotifyPlatform {
    // Can this running build post OS notifications at all? (Not an app bundle on macOS, no
    // QApplication on Windows.)
    bool available();
    // Reports the app's notification permission: "allowed", "denied" or "notAsked".
    void status(std::function<void(QString)> done);
    // Asks for permission if needed, then reports (state, why); why is the refusal reason,
    // empty when the OS answered normally.
    void request(std::function<void(QString state, QString why)> done);
    // Posts one notification. tag rolls up: a later post with the same tag replaces the
    // earlier one where the OS supports that.
    void post(const QString& title, const QString& body, bool sound,
              const QString& owner, int ownerId, const QString& tag);
    // Opens the OS's notification settings for this app.
    void openSettings();
    // Sets what a notification click reports: (owner, ownerId).
    void onActivated(std::function<void(const QString&, int)> handler);
}
