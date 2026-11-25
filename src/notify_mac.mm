#include "notify_platform.h"
#include <QDesktopServices>
#include <QUrl>
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
static UNUserNotificationCenter* center() {
    static ChorusNotificationDelegate* delegate = nil;
    UNUserNotificationCenter* c = UNUserNotificationCenter.currentNotificationCenter;
    if (!delegate) { delegate = [ChorusNotificationDelegate new]; c.delegate = delegate; }
    return c;
}

namespace NotifyPlatform {

bool available() { return NSBundle.mainBundle.bundleIdentifier.length > 0; }

void status(std::function<void(QString)> done) {
    [center() getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* s) {
        QString state = s.authorizationStatus == UNAuthorizationStatusNotDetermined ? "notAsked"
                      : s.authorizationStatus == UNAuthorizationStatusDenied ? "denied" : "allowed";
        dispatch_async(dispatch_get_main_queue(), ^{ done(state); });
    }];
}

// done(state, why): why is macOS's refusal, empty when it answered.
void request(std::function<void(QString, QString)> done) {
    [center() requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
                            completionHandler:^(BOOL, NSError* error) {
        QString why;
        if (error) why = error.domain.length && [error.domain isEqualToString:UNErrorDomain] && error.code == UNErrorCodeNotificationsNotAllowed
                ? QStringLiteral("macOS won't let this copy of ChorusCut ask: it isn't signed as an app. Rebuild it (the build signs it), then try again.")
                : QString::fromNSString(error.localizedDescription);
        status([done, why](QString state) { done(state, why); });
    }];
}

// A notification with a tag replaces the last one with that tag, so takes finishing while the user
// is away add up to one notification instead of a pile.
void post(const QString& title, const QString& body, bool sound, const QString& owner, int ownerId, const QString& tag) {
    UNMutableNotificationContent* content = [UNMutableNotificationContent new];
    content.title = title.toNSString();
    content.body = body.toNSString();
    if (sound) content.sound = UNNotificationSound.defaultSound;
    content.userInfo = @{@"owner": owner.toNSString(), @"ownerId": @(ownerId)};
    NSString* identifier = tag.isEmpty() ? NSUUID.UUID.UUIDString : tag.toNSString();
    UNNotificationRequest* request = [UNNotificationRequest requestWithIdentifier:identifier content:content trigger:nil];
    [center() addNotificationRequest:request withCompletionHandler:nil];
}

void openSettings() {
    QDesktopServices::openUrl(QUrl("x-apple.systempreferences:com.apple.Notifications-Settings.extension"));
}

void onActivated(std::function<void(const QString&, int)> handler) { clickHandler = std::move(handler); }

}
