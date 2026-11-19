#pragma once

#include <QFileSystemWatcher>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <QVariantList>

class Editor;

// The Project Manager's model, after Resolve's: project libraries are folders, and every
// subfolder holding a project.sqlite is a project. The list of libraries is remembered.
class ProjectLibrary : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantList libraries READ libraries NOTIFY librariesChanged)
    Q_PROPERTY(int current READ current WRITE setCurrent NOTIFY librariesChanged)
    Q_PROPERTY(QUrl folder READ folder NOTIFY librariesChanged)
    Q_PROPERTY(QVariantList projects READ projects NOTIFY projectsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
public:
    explicit ProjectLibrary(Editor* editor, QObject* parent = nullptr);
    QVariantList libraries() const;
    int current() const { return index; }
    void setCurrent(int value);
    QUrl folder() const;
    QVariantList projects() const { return list; }
    bool busy() const { return working; }
    QString message() const { return note; }

    Q_INVOKABLE void refresh();
    Q_INVOKABLE void addLibrary(const QUrl& folder);
    // Forgets a library; its projects stay on disk.
    Q_INVOKABLE void removeLibrary(int which);
    // Returns an error to show next to the name field, or an empty string when the name is usable.
    Q_INVOKABLE QString checkName(const QString& name, const QString& except = {}) const;
    Q_INVOKABLE bool open(const QString& path);
    Q_INVOKABLE bool rename(const QString& path, const QString& name);
    Q_INVOKABLE void duplicate(const QString& path);
    Q_INVOKABLE bool moveToTrash(const QString& path);
    Q_INVOKABLE void reveal(const QString& path);
    // The new-project dialog creates in the current library.
    Q_INVOKABLE void create(const QString& name, const QUrl& song, const QUrl& base, const QUrl& insert);

    // Saved inside each project folder; the editor writes it after opening a project.
    static QString thumbnailPath(const QString& project);
signals:
    void librariesChanged();
    void projectsChanged();
    void busyChanged();
    void messageChanged();
    void created(bool success);
private:
    Editor* editor;
    QStringList folders;
    int index = 0;
    QVariantList list;
    bool working = false;
    QString note;
    QFileSystemWatcher watcher;
    void say(const QString& text);
    void save();
    void watch();
    bool isOpen(const QString& path) const;
};
