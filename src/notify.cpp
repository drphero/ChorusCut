#include "notify.h"
#include "notify_platform.h"
#include "editor.h"
#include <QFile>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSettings>
#include <QWindow>

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
    if (logPath.isEmpty() && NotifyPlatform::available())
        NotifyPlatform::onActivated([self](const QString& owner, int id) { if (self) self->activate(owner, id); });
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
    if (!NotifyPlatform::available()) { allowed = "unavailable"; emit permissionChanged(); return; }
    QPointer<Notifier> self(this);
    NotifyPlatform::status([self](QString state) { if (self && self->allowed != state) { self->allowed = state; emit self->permissionChanged(); } });
}

void Notifier::requestPermission() {
    if (!logPath.isEmpty() || !NotifyPlatform::available()) { refreshPermission(); return; }
    QPointer<Notifier> self(this);
    NotifyPlatform::request([self](QString state, QString why) {
        if (!self || (self->allowed == state && self->refusal == why)) return;
        self->allowed = state;
        self->refusal = why;
        emit self->permissionChanged();
    });
}

void Notifier::openSystemSettings() {
    NotifyPlatform::openSettings();
}

void Notifier::dismissLatest() {
    upscales = 0;
    upscaleShots.clear();
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
    QString title, body, tag, about = owner;
    int aboutId = ownerId;
    if (ok && use == "take-upscale") {
        // Upscales add up until the notice is dismissed: one notice, one OS notification.
        ++upscales;
        upscaleShots.insert(ownerId);
        tag = "chorus-upscale";
        title = upscales > 1 ? QString("%1 takes upscaled").arg(upscales) : what + ": take upscaled";
        body = upscales > 1 ? QString("In %1 shots. Render uses them; switch the viewer to Upscaled to watch them.").arg(upscaleShots.size())
                            : "Render uses it; switch the viewer to Upscaled to watch it.";
        if (upscaleShots.size() > 1) { about.clear(); aboutId = 0; }
    } else if (ok && take) {
        // Several takes waiting (several shots generating while the user was away): one notice
        // that counts them all, and Show goes through them one at a time.
        const auto waiting = editor->newTakes();
        tag = "chorus-takes";
        if (waiting.size() > 1) {
            QList<int> others;
            for (const auto& v : waiting) {
                const int n = v.toMap()["shotNumber"].toInt();
                if (v.toMap()["shot"].toInt() != ownerId && !others.contains(n)) others << n;
            }
            QStringList names;
            for (int n : others) names << QString::number(n);
            const QString list = names.size() > 1 ? names.mid(0, names.size() - 1).join(", ") + " and " + names.last() : names.value(0);
            title = QString("%1 new takes to watch").arg(waiting.size());
            body = what + " just finished" + (others.isEmpty() ? QString() : (others.size() == 1 ? "; also shot " : "; also shots ") + list)
                   + ". Show plays them one at a time.";
            about = "takes"; aboutId = 0;
        } else {
            title = what + (use == "take-face" ? ": face refined" : ": take ready");
            body = (use == "take-face" ? "A take with a face redrawn, in the same place as the one it came from."
                    : use == "take-sync" ? "A new take, singing to the song." : "A new take from the reference pictures.") + QString(" Show plays it.");
        }
    } else if (ok) {
        title = what + (count == 1 ? ": picture ready" : QString(": %1 pictures ready").arg(count));
        body = "Generated pictures arrived, waiting for you to choose.";
    } else {
        title = what + (take ? ": take failed" : ": picture failed");
        body = text;
    }
    deliver(title, body, about, aboutId, ok, tag);
}

void Notifier::test() {
    deliver("ChorusCut notifications", "This is how a finished take will tell you.", {}, 0, true);
}

void Notifier::deliver(const QString& title, const QString& body, const QString& owner, int ownerId, bool ok, const QString& tag) {
    banner = QVariantMap{{"title", title}, {"body", body}, {"owner", owner}, {"ownerId", ownerId}, {"ok", ok}};
    emit latestChanged();
    const bool away = QGuiApplication::applicationState() != Qt::ApplicationActive;
    if (backgroundOnly && !away) return;
    if (useSystem) {
        if (!logPath.isEmpty()) log({{"kind", "notification"}, {"title", title}, {"body", body}, {"sound", useSound}, {"owner", owner}, {"ownerId", ownerId}, {"tag", tag}});
        else if (NotifyPlatform::available() && allowed != "denied") {
            if (allowed == "notAsked") requestPermission();
            NotifyPlatform::post(title, body, useSound, owner, ownerId, tag);
        }
    }
    if (useDock) {
        if (!logPath.isEmpty()) log({{"kind", "dock"}});
        else for (QWindow* w : QGuiApplication::topLevelWindows()) if (w->isVisible()) { w->alert(0); break; }
    }
}
