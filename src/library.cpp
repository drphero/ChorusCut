#include "library.h"
#include "editor.h"
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocale>
#include <QProcess>
#include <QSet>
#include <QSettings>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUuid>
#include <QtConcurrentRun>
#include <algorithm>
extern "C" {
#include <libavformat/avformat.h>
}

namespace {
QString defaultLibrary() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::MoviesLocation)).filePath("Chorus Cut");
}

qint64 durationMs(const QString& file) {
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, QFile::encodeName(file), nullptr, nullptr) < 0) return 0;
    qint64 ms = 0;
    if (avformat_find_stream_info(raw, nullptr) >= 0 && raw->duration > 0) ms = raw->duration / 1000;
    avformat_close_input(&raw);
    return ms;
}

QString when(const QDateTime& time) {
    const QLocale locale;
    const QDate day = time.date(), today = QDate::currentDate();
    const QString clock = locale.toString(time.time(), QLocale::ShortFormat);
    if (day == today) return "Today " + clock;
    if (day == today.addDays(-1)) return "Yesterday " + clock;
    return locale.toString(day, day.year() == today.year() ? "d MMM" : "d MMM yyyy");
}

// Reads a project's card details without opening it for editing.
QVariantMap summary(const QString& path) {
    QVariantMap card{{"name", QFileInfo(path).fileName()}, {"path", path}};
    const QFileInfo db(QDir(path).filePath("project.sqlite"));
    card["modified"] = db.lastModified().toMSecsSinceEpoch();
    card["modifiedText"] = when(db.lastModified());
    QString master = "Media/Master.wav";
    bool picture = QFileInfo::exists(QDir(path).filePath("Media/Performance.mp4"));
    int words = 0, lines = 0, flagged = 0;
    bool hasText = false;
    {
        const QString name = QUuid::createUuid().toString();
        auto sql = QSqlDatabase::addDatabase("QSQLITE", name);
        sql.setConnectOptions("QSQLITE_OPEN_READONLY");
        sql.setDatabaseName(db.absoluteFilePath());
        if (sql.open()) {
            QSqlQuery q(sql);
            if (q.exec("SELECT role,path FROM media")) {
                picture = false;
                while (q.next()) {
                    if (q.value(0).toString() == "master") master = q.value(1).toString();
                    if (q.value(0).toString() == "performance") picture = true;
                }
            }
            if (q.exec("SELECT draft,aligned,words FROM lyrics WHERE id=1") && q.next()) {
                hasText = !q.value(0).toString().trimmed().isEmpty();
                if (q.value(0).toString() == q.value(1).toString()) {
                    const auto spans = QJsonDocument::fromJson(q.value(2).toByteArray()).array();
                    words = spans.size();
                    QSet<int> seen, marked;
                    for (const auto& span : spans) {
                        const auto w = span.toObject();
                        seen.insert(w["line"].toInt());
                        if (w["flag"].toBool()) marked.insert(w["line"].toInt());
                    }
                    lines = seen.size();
                    flagged = marked.size();
                }
            }
            q.finish();
            sql.close();
        }
        sql = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
    card["durationMs"] = durationMs(QDir(path).filePath(master));
    card["picture"] = picture;
    card["words"] = words;
    card["lines"] = lines;
    card["flagged"] = flagged;
    card["lyrics"] = !hasText ? QString("No lyrics yet")
                   : !words ? QString("Lyrics not aligned")
                   : flagged ? QString("%1 lines · %2 to check").arg(lines).arg(flagged)
                             : QString("%1 lines aligned").arg(lines);
    const QString thumb = ProjectLibrary::thumbnailPath(path);
    card["thumbnail"] = QFileInfo::exists(thumb) ? QUrl::fromLocalFile(thumb) : QUrl();
    card["thumbnailStamp"] = QFileInfo(thumb).lastModified().toMSecsSinceEpoch();
    return card;
}

bool copyTree(const QString& from, const QString& to) {
    if (!QDir().mkpath(to)) return false;
    QDirIterator it(from, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    const QDir source(from);
    while (it.hasNext()) {
        const QString file = it.next();
        const QFileInfo info = it.fileInfo();
        const QString target = QDir(to).filePath(source.relativeFilePath(file));
        if (info.isSymLink()) continue;  // Projects hold copies of their media, never links.
        if (info.isDir() ? !QDir().mkpath(target) : !QFile::copy(file, target)) return false;
    }
    return true;
}
}

QString ProjectLibrary::thumbnailPath(const QString& project) { return QDir(project).filePath(".thumbnail.jpg"); }

ProjectLibrary::ProjectLibrary(Editor* editor, QObject* parent) : QObject(parent), editor(editor) {
    QSettings settings;
    folders = settings.value("projects/libraries").toStringList();
    if (folders.isEmpty()) folders = {defaultLibrary()};
    index = qBound(0, settings.value("projects/current", 0).toInt(), int(folders.size()) - 1);
    connect(&watcher, &QFileSystemWatcher::directoryChanged, this, [this] { if (!working) refresh(); });
    connect(editor, &Editor::projectChanged, this, &ProjectLibrary::refresh);
    refresh();
}

QVariantList ProjectLibrary::libraries() const {
    QVariantList out;
    for (const auto& f : folders) {
        const QFileInfo info(f);
        QString shown = f;
        const QString home = QDir::homePath();
        if (shown.startsWith(home + "/")) shown = "~" + shown.mid(home.size());
        out.append(QVariantMap{{"name", info.fileName()}, {"path", f}, {"shown", shown}, {"exists", info.isDir()}});
    }
    return out;
}

QUrl ProjectLibrary::folder() const { return QUrl::fromLocalFile(folders.value(index)); }

void ProjectLibrary::setCurrent(int value) {
    value = qBound(0, value, int(folders.size()) - 1);
    if (value == index) return;
    index = value;
    save();
    emit librariesChanged();
    refresh();
}

void ProjectLibrary::say(const QString& text) {
    if (note == text) return;
    note = text;
    emit messageChanged();
}

void ProjectLibrary::save() {
    QSettings settings;
    settings.setValue("projects/libraries", folders);
    settings.setValue("projects/current", index);
}

void ProjectLibrary::watch() {
    if (!watcher.directories().isEmpty()) watcher.removePaths(watcher.directories());
    if (QFileInfo(folders.value(index)).isDir()) watcher.addPath(folders.value(index));
}

bool ProjectLibrary::isOpen(const QString& path) const {
    return editor->ready() && QFileInfo(editor->projectPath()).canonicalFilePath() == QFileInfo(path).canonicalFilePath();
}

void ProjectLibrary::refresh() {
    QVariantList next;
    const QDir dir(folders.value(index));
    for (const auto& entry : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::IgnoreCase)) {
        if (!QFileInfo::exists(QDir(entry.absoluteFilePath()).filePath("project.sqlite"))) continue;
        auto card = summary(entry.absoluteFilePath());
        card["open"] = isOpen(entry.absoluteFilePath());
        next.append(card);
    }
    list = next;
    watch();
    emit projectsChanged();
}

void ProjectLibrary::addLibrary(const QUrl& url) {
    if (!url.isLocalFile()) return;
    const QString path = QDir::cleanPath(url.toLocalFile());
    int at = folders.indexOf(path);
    if (at < 0) { folders.append(path); at = folders.size() - 1; }
    index = at;
    save();
    emit librariesChanged();
    refresh();
}

void ProjectLibrary::removeLibrary(int which) {
    if (which < 0 || which >= folders.size() || folders.size() == 1) return;
    folders.removeAt(which);
    if (index >= which && index > 0) --index;
    save();
    emit librariesChanged();
    refresh();
}

QString ProjectLibrary::checkName(const QString& raw, const QString& except) const {
    const QString name = raw.trimmed();
    if (name.isEmpty()) return "Enter a project name";
    if (name.startsWith('.') || name.contains('/') || name.contains(':')) return "Use a name without “/”, “:” or a leading dot";
    if (name.size() > 120) return "Use a shorter name";
    const QDir dir(folders.value(index));
    if (name.compare(except, Qt::CaseInsensitive) != 0 && QFileInfo::exists(dir.filePath(name)))
        return "A project or folder with that name is already in this library";
    return {};
}

bool ProjectLibrary::open(const QString& path) {
    if (isOpen(path)) return true;
    if (!editor->openProject(QUrl::fromLocalFile(path))) { say(editor->status()); return false; }
    say({});
    return true;
}

bool ProjectLibrary::rename(const QString& path, const QString& raw) {
    const QString name = raw.trimmed();
    const QString from = QFileInfo(path).fileName();
    if (name == from) return true;
    if (auto problem = checkName(name, from); !problem.isEmpty()) { say(problem); return false; }
    const bool wasOpen = isOpen(path);
    if (wasOpen) editor->closeProject();
    QDir parent = QFileInfo(path).absoluteDir();
    const QString target = parent.filePath(name);
    // A change of case alone needs an intermediate name on a case-insensitive volume.
    bool ok;
    if (name.compare(from, Qt::CaseInsensitive) == 0) {
        const QString step = parent.filePath(".chorus-rename-" + QUuid::createUuid().toString(QUuid::Id128));
        ok = parent.rename(path, step) && parent.rename(step, target);
    } else ok = parent.rename(path, target);
    if (wasOpen) editor->openProject(QUrl::fromLocalFile(ok ? target : path));
    say(ok ? QString("Renamed “%1” to “%2”.").arg(from, name) : QString("Could not rename “%1”.").arg(from));
    refresh();
    return ok;
}

void ProjectLibrary::duplicate(const QString& path) {
    if (working) return;
    const QString from = QFileInfo(path).fileName();
    const QDir parent = QFileInfo(path).absoluteDir();
    QString name = from + " copy";
    for (int n = 2; QFileInfo::exists(parent.filePath(name)); ++n) name = QString("%1 copy %2").arg(from).arg(n);
    if (isOpen(path)) editor->saveThumbnail();
    working = true;
    emit busyChanged();
    say(QString("Duplicating “%1”…").arg(from));
    const QString target = parent.filePath(name);
    auto done = new QFutureWatcher<bool>(this);
    connect(done, &QFutureWatcher<bool>::finished, this, [this, done, name, from] {
        const bool ok = done->result();
        done->deleteLater();
        working = false;
        emit busyChanged();
        say(ok ? QString("Duplicated “%1” as “%2”.").arg(from, name) : QString("Could not duplicate “%1”.").arg(from));
        refresh();
    });
    done->setFuture(QtConcurrent::run([path, target, parent] {
        // Copy beside the library under a hidden name, then reveal it in one rename.
        QTemporaryDir stage(parent.filePath(".chorus-copy-XXXXXX"));
        if (!stage.isValid()) return false;
        const QString inner = QDir(stage.path()).filePath("project");
        if (!copyTree(path, inner)) return false;
        return QDir().rename(inner, target);
    }));
}

bool ProjectLibrary::moveToTrash(const QString& path) {
    const QString name = QFileInfo(path).fileName();
    if (isOpen(path)) editor->closeProject();
    bool ok;
    // Tests redirect the Trash so they never fill the user's.
    const QString testTrash = qEnvironmentVariable("CHORUS_TRASH_DIR");
    if (!testTrash.isEmpty()) ok = QDir().rename(path, QDir(testTrash).filePath(name + "-" + QUuid::createUuid().toString(QUuid::Id128)));
    else ok = QFile::moveToTrash(path);
    say(ok ? QString("Moved “%1” to the Trash.").arg(name) : QString("Could not move “%1” to the Trash.").arg(name));
    refresh();
    return ok;
}

void ProjectLibrary::reveal(const QString& path) {
    QProcess::startDetached("/usr/bin/open", {"-R", path});
}

void ProjectLibrary::create(const QString& name, const QUrl& song, const QUrl& base, const QUrl& insert) {
    if (auto problem = checkName(name); !problem.isEmpty()) { say(problem); emit created(false); return; }
    const QString dir = folders.value(index);
    if (!QDir().mkpath(dir)) { say("Could not create the library folder " + dir); emit created(false); return; }
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(editor, &Editor::importFinished, this, [this, connection](bool success) {
        disconnect(*connection);
        say(success ? QString() : editor->status());
        refresh();
        emit created(success);
    });
    editor->createProject(QUrl::fromLocalFile(dir), name.trimmed(), song, base, insert);
}
