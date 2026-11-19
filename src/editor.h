#pragma once
#include <QTimer>
#include "lyrics.h"
#include "comfy.h"
#include "plan.h"
#include <mlt++/Mlt.h>
#include <QtQuick/QQuickPaintedItem>
#include <QImage>
#include <QUndoStack>
#include <QVector>
#include <QUrl>
#include <memory>
#include <atomic>
#include <vector>

class Monitor : public QQuickPaintedItem {
    Q_OBJECT
public:
    using QQuickPaintedItem::QQuickPaintedItem;
    void showImage(const QImage& next) { frame = next; update(); }
    void paint(QPainter* painter) override;
private:
    QImage frame;
};

class Editor : public QObject {
    Q_OBJECT
    Q_PROPERTY(Lyrics* lyrics READ lyrics CONSTANT)
    Q_PROPERTY(ComfyJobs* comfy READ comfy CONSTANT)
    Q_PROPERTY(Plan* plan READ plan CONSTANT)
    Q_PROPERTY(bool monitorMuted READ monitorMuted WRITE setMonitorMuted NOTIFY monitorChanged)
    Q_PROPERTY(int monitorDb READ monitorDb WRITE setMonitorDb NOTIFY monitorChanged)
    Q_PROPERTY(int frame READ frame NOTIFY cursorChanged)
    Q_PROPERTY(int cutIn READ cutIn NOTIFY edited)
    Q_PROPERTY(int cutOut READ cutOut NOTIFY edited)
    Q_PROPERTY(int sourceIn READ sourceIn NOTIFY edited)
    Q_PROPERTY(bool playing READ playing NOTIFY playbackChanged)
    Q_PROPERTY(bool canUndo READ canUndo NOTIFY edited)
    Q_PROPERTY(bool canRedo READ canRedo NOTIFY edited)
    Q_PROPERTY(QString status READ status NOTIFY messageChanged)
    Q_PROPERTY(bool importing READ importing NOTIFY importingChanged)
    Q_PROPERTY(bool ready READ ready NOTIFY projectChanged)
    Q_PROPERTY(int length READ length NOTIFY projectChanged)
    Q_PROPERTY(double fps READ fps NOTIFY projectChanged)
    Q_PROPERTY(int sourceLength READ sourceLength NOTIFY projectChanged)
    Q_PROPERTY(int baseLength READ baseLength NOTIFY projectChanged)
    Q_PROPERTY(QString projectName READ projectName NOTIFY projectChanged)
    Q_PROPERTY(QString performanceName READ performanceName NOTIFY projectChanged)
    Q_PROPERTY(QString cutawayName READ cutawayName NOTIFY projectChanged)
    Q_PROPERTY(QString masterName READ masterName NOTIFY projectChanged)
    Q_PROPERTY(QUrl defaultExport READ defaultExport NOTIFY projectChanged)
    Q_PROPERTY(QString undoText READ undoText NOTIFY edited)
    Q_PROPERTY(QString redoText READ redoText NOTIFY edited)
    Q_PROPERTY(bool peaksReady READ peaksReady NOTIFY peaksChanged)
    Q_PROPERTY(bool looping READ looping WRITE loop NOTIFY loopingChanged)
public:
    Editor();
    Lyrics* lyrics() { return &lyricState; }
    ComfyJobs* comfy() { return comfyJobs; }
    Plan* plan() { return planState; }
    Project* current() { return project.get(); }
    Q_INVOKABLE void seekLyric(int startMs);
    Q_INVOKABLE void playLyric(int startMs, int endMs);
    // Plays [startMs, endMs); with looping on, the passage repeats.
    Q_INVOKABLE void playRange(int startMs, int endMs) { playLyric(startMs, endMs); }
    ~Editor();
    Q_INVOKABLE bool openProject(const QUrl& folder);
    // Saves the thumbnail and releases the project, e.g. before the manager renames or trashes it.
    Q_INVOKABLE void closeProject();
    QString projectPath() const { return project ? project->dir.absolutePath() : QString(); }
    // Writes the Project Manager card image: a picture frame or black, the song waveform and lyric lines.
    void saveThumbnail();
    Q_INVOKABLE void createProject(const QUrl& parent, const QString& name, const QUrl& song,
                                   const QUrl& base, const QUrl& insert);
    bool importing() const { return importInProgress; }
    bool ready() const { return bool(project); }
    int length() const { return project ? project->master.frames : 0; }
    double fps() const { return project ? double(project->fpsNum)/project->fpsDen : 24; }
    int sourceLength() const { return project ? project->cutaway.frames : 0; }
    int baseLength() const { return project ? qMin(length(),project->performance.frames) : 0; }
    QString projectName() const;
    QString performanceName() const;
    QString cutawayName() const;
    QString masterName() const;
    int frame() const { return cursor; }
    int cutIn() const { return trim.start; }
    int cutOut() const { return trim.end; }
    int sourceIn() const { return trim.source; }
    bool canUndo() const { return history.canUndo(); }
    bool canRedo() const { return history.canRedo(); }
    QString undoText() const { return history.undoText(); }
    QString redoText() const { return history.redoText(); }
    // Waveform overview: min/max pairs per kPeakMs bucket, normalised to the loudest sample.
    static constexpr int kPeakMs = 5;
    bool peaksReady() const { return !peakData.isEmpty(); }
    const QVector<float>& peaks() const { return peakData; }
    bool monitorMuted() const { return mutedOutput; }
    int monitorDb() const { return levelDb; }
    void setMonitorMuted(bool muted);
    void setMonitorDb(int dbValue);
    bool playing() const { return running; }
    QString status() const { return message; }
    Q_INVOKABLE void seek(int frame);
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void change(const QString& mode, int frames);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE bool exportTimeline(const QUrl& destination);
    Q_INVOKABLE void loop(bool enabled) { if (loopOn != enabled) { loopOn = enabled; emit loopingChanged(); } }
    bool looping() const { return loopOn; }
    QUrl defaultExport() const { return project ? QUrl::fromLocalFile(project->dir.filePath("Chorus.xml")) : QUrl(); }

    QImage imageAt(int position);
    bool checkMedia();
    bool checkPictures();
    bool applyTrim(Trim value) { return commit(value); }
signals:
    void projectChanged();
    void importingChanged();
    void importFinished(bool success);
    void imageReady(const QImage& image);
    void edited();
    void cursorChanged();
    void playbackChanged();
    void messageChanged();
    void monitorChanged();
    void peaksChanged();
    void loopingChanged();
private:
    std::unique_ptr<Project> project;
    Lyrics lyricState;
    ComfyJobs* comfyJobs;
    Plan* planState;
    int passageEnd = -1;
    int passageStart = -1;
    QUndoStack history;
    QVector<float> peakData;
    void computePeaks();
    QImage thumbPicture;
    QTimer thumbTimer;
    int peakSerial = 0;
    QSize previewSize;
    std::unique_ptr<Mlt::Profile> profile;
    std::unique_ptr<Mlt::Tractor> timeline;
    std::unique_ptr<Mlt::Consumer> output;
    std::unique_ptr<Mlt::Event> listener;
    Trim trim;
    int cursor = 0;
    std::atomic<int> generation{0};
    bool importInProgress = false;
    bool running = false;
    bool loopOn = false;
    bool mutedOutput = false;
    int levelDb = 0;
    void updateMonitorGain();
    QString message;
    void build();
    void stop();
    bool commit(Trim value);
    void info(const QString& text);
    static void onFrame(mlt_properties, void* self, mlt_event_data data);
};
