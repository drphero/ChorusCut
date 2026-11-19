#pragma once

#include <mlt++/Mlt.h>
#include <QDir>
#include <QSqlDatabase>
#include <QString>
#include <memory>

struct Trim {
    int start = 0;
    int end = 1; // Exclusive on the timeline.
    int source = 0;
    bool operator==(const Trim& other) const {
        return start == other.start && end == other.end && source == other.source;
    }
};

struct Media {
    QString path;
    int frames = 0;
    int stream = -1;
    int width = 0, height = 0;
    int channels = 0, sampleRate = 0, bits = 0;
};

struct Project {
    QDir dir;
    QSqlDatabase db;
    Media performance, cutaway, master;
    int fpsNum = 0, fpsDen = 1;
    Trim trim;
    ~Project();
    std::unique_ptr<Mlt::Profile> previewProfile() const;
    QString file(const Media& media) const { return dir.absoluteFilePath(media.path); }
    static std::unique_ptr<Project> open(const QString& directory, QString& error);
    static bool create(const QString& directory, const QString& song, const QString& base,
                       const QString& insert, QString& error);
private:
    bool connect(QString& error);
    bool probe(QString& error);
    bool writeSchema(bool legacy, QString& error);
};
