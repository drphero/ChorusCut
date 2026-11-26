#include "notify_platform.h"

// No OS notification backend on this platform yet (Linux); the in-app banner still works.
namespace NotifyPlatform {
    bool available() { return false; }
    void status(std::function<void(QString)> done) { done("unavailable"); }
    void request(std::function<void(QString, QString)> done) {
        done("unavailable", "Notifications are not implemented on this platform yet.");
    }
    void post(const QString&, const QString&, bool, const QString&, int, const QString&) {}
    void openSettings() {}
    void onActivated(std::function<void(const QString&, int)>) {}
}
