#include "notify.h"
#include "editor.h"
#include <QDesktopServices>
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSettings>
#include <QUrl>
#include <QWindow>
#include <functional>
#import <Foundation/Foundation.h>
#import <UserNotifications/UserNotifications.h>

// ---------------------------------------------------------------- macOS side
static std::function<void(const QString&, int)> clickHandler;

@interface ChorusNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end
@implementation ChorusNotificationDelegate
// The app only posts while the user wants it to, so show it even while the app is in front.
- (void)userNotificationCenter:(UNUserNotificationCenter*)center willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions))completionHandler {
    completionHandler(UNNotificationPresentationOptionBanner | UNNotificationPresentationOptionList | UNNotificationPresentationOptionSound);
}
- (void)userNotificationCenter:(UNUserNotificationCenter*)center didReceiveNotificationResponse:(UNNotificationResponse*)response
         withCompletionHandler:(void (^)(void))completionHandler {
    NSDictionary* info = response.notification.request.content.userInfo;
    const QString owner = QString::fromNSString(info[@"owner"] ?: @"");
    const int ownerId = [info[@"ownerId"] intValue];
    dispatch_async(dispatch_get_main_queue(), ^{ if (clickHandler) clickHandler(owner, ownerId); });
    completionHandler();
}
@end

// UNUserNotificationCenter needs an app bundle with an identifier; anything else (the test
// binaries) must never touch it.
static bool macAvailable() { return NSBundle.mainBundle.bundleIdentifier.length > 0; }
static UNUserNotificationCenter* center() {
    static ChorusNotificationDelegate* delegate = nil;
    UNUserNotificationCenter* c = UNUserNotificationCenter.currentNotificationCenter;
    if (!delegate) { delegate = [ChorusNotificationDelegate new]; c.delegate = delegate; }
    return c;
}
static void macStatus(std::function<void(QString)> done) {
    [center() getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* s) {
        QString state = s.authorizationStatus == UNAuthorizationStatusNotDetermined ? "notAsked"
                      : s.authorizationStatus == UNAuthorizationStatusDenied ? "denied" : "allowed";
        dispatch_async(dispatch_get_main_queue(), ^{ done(state); });
    }];
}
// done(state, why): why is macOS's refusal, empty when it answered.
static void macRequest(std::function<void(QString, QString)> done) {
    [center() requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                            completionHandler:^(BOOL, NSError* error) {
        QString why;
        if (error) why = error.domain.length && [error.domain isEqualToString:UNErrorDomain] && error.code == UNErrorCodeNotificationsNotAllowed
                ? QStringLiteral("macOS won't let this copy of Chorus Cut ask: it isn't signed as an app. Rebuild it (the build signs it), then try again.")
                : QString::fromNSString(error.localizedDescription);
        macStatus([done, why](QString state) { done(state, why); });
    }];
}
static void macPost(const QString& title, const QString& body, bool sound, const QString& owner, int ownerId) {
    UNMutableNotificationContent* content = [UNMutableNotificationContent new];
    content.title = title.toNSString();
    content.body = body.toNSString();
    if (sound) content.sound = UNNotificationSound.defaultSound;
    content.userInfo = @{@"owner": owner.toNSString(), @"ownerId": @(ownerId)};
    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:NSUUID.UUID.UUIDString content:content trigger:nil];
    [center() addNotificationRequest:request withCompletionHandler:nil];
}

// ---------------------------------------------------------------- Notifier
Notifier::Notifier(Editor* e) : QObject(e), editor(e) {
    logPath = qEnvironmentVariable("CHORUS_NOTIFY_LOG");
    QSettings s;
    wantTakes = s.value("notify/takes", true).toBool();
    wantPictures = s.value("notify/pictures", false).toBool();
    wantFailures = s.value("notify/failures", true).toBool();
    useSystem = s.value("notify/system", true).toBool();
    useSound = s.value("notify/sound", true).toBool();
    useDock = s.value("notify/dock", true).toBool();
    backgroundOnly = s.value("notify/onlyInBackground", true).toBool();
    QPointer<Notifier> self(this);
    if (logPath.isEmpty() && macAvailable()) clickHandler = [self](const QString& owner, int id) { if (self) self->activate(owner, id); };
    refreshPermission();
}

void Notifier::set(bool& field, bool on, const char* key) {
    if (field == on) return;
    field = on;
    QSettings().setValue(QString("notify/") + key, on);
    emit optionsChanged();
}

void Notifier::setSystem(bool on) {
    set(useSystem, on, "system");
    if (on && allowed == "notAsked") requestPermission();
}

void Notifier::refreshPermission() {
    if (!logPath.isEmpty()) { allowed = "allowed"; emit permissionChanged(); return; }
    if (!macAvailable()) { allowed = "unavailable"; emit permissionChanged(); return; }
    QPointer<Notifier> self(this);
    macStatus([self](QString state) { if (self && self->allowed != state) { self->allowed = state; emit self->permissionChanged(); } });
}

void Notifier::requestPermission() {
    if (!logPath.isEmpty() || !macAvailable()) { refreshPermission(); return; }
    QPointer<Notifier> self(this);
    macRequest([self](QString state, QString why) {
        if (!self || (self->allowed == state && self->refusal == why)) return;
        self->allowed = state;
        self->refusal = why;
        emit self->permissionChanged();
    });
}

void Notifier::openSystemSettings() {
    QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.Notifications-Settings.extension"));
}

void Notifier::dismissLatest() {
    if (banner.isEmpty()) return;
    banner.clear();
    emit latestChanged();
}

void Notifier::log(const QVariantMap& entry) const {
    QFile f(logPath);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    f.write(QJsonDocument(QJsonObject::fromVariantMap(entry)).toJson(QJsonDocument::Compact) + "\n");
}

void Notifier::jobFinished(const QString& owner, int ownerId, const QString& use, bool ok, const QString& text, int count) {
    const bool take = owner == "shot";
    if (ok ? !(take ? wantTakes : wantPictures) : !wantFailures) return;
    auto plan = editor->plan();
    QString what;
    if (take) {
        const auto s = plan->shot(ownerId);
        what = s.isEmpty() ? "A shot" : "Shot " + s["number"].toString();
    } else {
        const auto item = plan->item(owner, ownerId);
        what = item["fields"].toMap()["name"].toString();
        if (what.isEmpty()) what = owner == "location" ? "A location" : owner == "look" ? "A look" : "A character";
    }
    QString title, body;
    if (ok) {
        title = take ? what + ": take ready" : what + (count == 1 ? ": picture ready" : QString(": %1 pictures ready").arg(count));
        body = take ? (use == "take-sync" ? "A new take, singing to the song." : "A new take from the reference pictures.") + QString(" Click to see it.")
                    : "Generated pictures arrived, waiting for you to choose.";
    } else {
        title = what + (take ? ": take failed" : ": picture failed");
        body = text;
    }
    deliver(title, body, owner, ownerId, ok);
}

void Notifier::test() {
    deliver("Chorus Cut notifications", "This is how a finished take will tell you.", {}, 0, true);
}

void Notifier::deliver(const QString& title, const QString& body, const QString& owner, int ownerId, bool ok) {
    banner = QVariantMap{{"title", title}, {"body", body}, {"owner", owner}, {"ownerId", ownerId}, {"ok", ok}};
    emit latestChanged();
    const bool away = QGuiApplication::applicationState() != Qt::ApplicationActive;
    if (backgroundOnly && !away) return;
    if (useSystem) {
        if (!logPath.isEmpty()) log({{"kind", "notification"}, {"title", title}, {"body", body}, {"sound", useSound}, {"owner", owner}, {"ownerId", ownerId}});
        else if (macAvailable() && allowed != "denied") {
            if (allowed == "notAsked") requestPermission();
            macPost(title, body, useSound, owner, ownerId);
        }
    }
    if (useDock) {
        if (!logPath.isEmpty()) log({{"kind", "dock"}});
        else for (QWindow* w : QGuiApplication::topLevelWindows()) if (w->isVisible()) { w->alert(0); break; }
    }
}
