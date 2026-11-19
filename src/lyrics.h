#pragma once
#include "project.h"
#include <QJsonArray>
#include <QObject>
#include <QProcess>
#include <QUrl>
#include <QVariantList>

class QUndoStack;

// Lyric text plus word timings. Every timing edit goes through the commands below so
// the timeline, the inspector and (later) the agent share validation, persistence and undo.
class Lyrics : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString text READ text NOTIFY changed)
    Q_PROPERTY(QVariantList lines READ lines NOTIFY changed)
    Q_PROPERTY(QVariantList regions READ regions NOTIFY changed)
    Q_PROPERTY(QVariantList instrumental READ instrumental NOTIFY changed)
    Q_PROPERTY(int flaggedLines READ flaggedLines NOTIFY changed)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
public:
    bool load(Project& next, QString& error);
    void setHistory(QUndoStack* stack) { history = stack; }
    QString text() const { return draft; }
    QVariantList lines() const;
    QVariantList regions() const;
    QVariantList instrumental() const;
    int flaggedLines() const;
    bool busy() const { return process != nullptr; }
    QString message() const { return notice; }
    Q_INVOKABLE bool saveText(const QString& text);
    Q_INVOKABLE void align();
    Q_INVOKABLE void cancel();
    Q_INVOKABLE bool importAlignment(const QUrl& file);

    // Timing edits (milliseconds). Each is one undo step and marks the words as checked.
    Q_INVOKABLE bool correct(int word, int startMs, int endMs);
    Q_INVOKABLE bool moveWords(int first, int last, int deltaMs);
    Q_INVOKABLE bool markChecked(int first, int last);
    // Allowed range for a live drag preview: {"min": ms, "max": ms}.
    Q_INVOKABLE QVariantMap moveLimits(int first, int last) const;
    Q_INVOKABLE QVariantMap wordLimits(int word) const;

    // Used by the undo command; replaces words[first...] with the given objects.
    bool applyWords(int first, const QJsonArray& replacement, const QString& done);
    ~Lyrics() override;
signals:
    void changed();
    void busyChanged();
    void messageChanged();
    void finished(bool success);
    void historyInvalidated();
private:
    Project* project = nullptr;
    QUndoStack* history = nullptr;
    QString draft, alignedText, notice;
    QJsonArray words, sections;
    int revision = 0;
    QProcess* process = nullptr;
    int songMs() const;
    bool store(const QString& text, const QString& aligned, const QJsonArray& spans, const QJsonArray& gaps);
    bool edit(int first, const QJsonArray& replacement, const QString& label, const QString& done);
    bool accept(const QByteArray& result);
    void say(const QString& value);
};
