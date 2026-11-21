#pragma once
#include <QObject>
#include <QVariantMap>

class Editor;

// Tells the user when generation finishes: a banner in the app, and, as chosen, a macOS
// notification (with or without a sound) and a bounce of the Dock icon. The choices are the
// user's, kept in QSettings under "notify/". Clicking a macOS notification brings the app forward
// and emits activated() with what the job was for.
//
// With CHORUS_NOTIFY_LOG set, nothing reaches macOS: each notification and Dock bounce is
// appended to that file as a JSON line instead (for tests).
class Notifier : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool takes READ takes WRITE setTakes NOTIFY optionsChanged)
    Q_PROPERTY(bool pictures READ pictures WRITE setPictures NOTIFY optionsChanged)
    Q_PROPERTY(bool failures READ failures WRITE setFailures NOTIFY optionsChanged)
    Q_PROPERTY(bool system READ system WRITE setSystem NOTIFY optionsChanged)
    Q_PROPERTY(bool sound READ sound WRITE setSound NOTIFY optionsChanged)
    Q_PROPERTY(bool dock READ dock WRITE setDock NOTIFY optionsChanged)
    Q_PROPERTY(bool onlyInBackground READ onlyInBackground WRITE setOnlyInBackground NOTIFY optionsChanged)
    // macOS's answer for this app: "allowed", "denied", "notAsked" or "unavailable" (not an app bundle).
    Q_PROPERTY(QString permission READ permission NOTIFY permissionChanged)
    // Why the last request for permission got no answer from macOS; empty when it answered.
    Q_PROPERTY(QString refusal READ refusalText NOTIFY permissionChanged)
    // The latest in-app banner: {title, body, owner, ownerId, ok}; empty when dismissed.
    Q_PROPERTY(QVariantMap latest READ latest NOTIFY latestChanged)
public:
    explicit Notifier(Editor* editor);
    bool takes() const { return wantTakes; }
    bool pictures() const { return wantPictures; }
    bool failures() const { return wantFailures; }
    bool system() const { return useSystem; }
    bool sound() const { return useSound; }
    bool dock() const { return useDock; }
    bool onlyInBackground() const { return backgroundOnly; }
    void setTakes(bool on) { set(wantTakes, on, "takes"); }
    void setPictures(bool on) { set(wantPictures, on, "pictures"); }
    void setFailures(bool on) { set(wantFailures, on, "failures"); }
    void setSystem(bool on);
    void setSound(bool on) { set(useSound, on, "sound"); }
    void setDock(bool on) { set(useDock, on, "dock"); }
    void setOnlyInBackground(bool on) { set(backgroundOnly, on, "onlyInBackground"); }
    QString permission() const { return allowed; }
    QString refusalText() const { return refusal; }
    QVariantMap latest() const { return banner; }
    Q_INVOKABLE void dismissLatest();
    // Asks macOS for permission (it asks the user the first time), then refreshes permission.
    Q_INVOKABLE void requestPermission();
    // Opens System Settings at Notifications, where a denied permission is changed.
    Q_INVOKABLE void openSystemSettings();
    // A sample, to try the choices.
    Q_INVOKABLE void test();
    // What a click on a notification does: the same as activated() from macOS.
    Q_INVOKABLE void activate(const QString& owner, int ownerId) { emit activated(owner, ownerId); }
public slots:
    void jobFinished(const QString& owner, int ownerId, const QString& use, bool ok, const QString& text, int count);
signals:
    void optionsChanged();
    void permissionChanged();
    void latestChanged();
    void activated(const QString& owner, int ownerId);
private:
    Editor* editor;
    bool wantTakes = true, wantPictures = false, wantFailures = true;
    bool useSystem = true, useSound = true, useDock = true, backgroundOnly = true;
    QString allowed = "notAsked";
    QString refusal;
    QVariantMap banner;
    QString logPath;
    void set(bool& field, bool on, const char* key);
    void deliver(const QString& title, const QString& body, const QString& owner, int ownerId, bool ok);
    void refreshPermission();
    void log(const QVariantMap& entry) const;
};
