#pragma once
#include <QTimer>
#include "lyrics.h"
#include "comfy.h"
#include "generate.h"
#include "requests.h"
#include "workflows.h"
#include "plan.h"
#include "agent.h"
#include "notify.h"
#include "render.h"
#include <mlt++/Mlt.h>
#include <QtQuick/QQuickItem>
#include <QImage>
#include <QUndoStack>
#include <QVector>
#include <QUrl>
#include <memory>
#include <atomic>
#include <vector>

class Editor;
// Paints the editor's current frame. Any number can show it at once (the Edit page's viewer and
// the pop-out window): each follows the editor it is given and starts from its latest frame.
// The viewer. Draws the frame as a texture the GPU scales: linear when enlarging, through
// mipmaps when shrinking, so a large preview frame stays sharp and doesn't shimmer in a small
// viewer, at any display's pixel density.
class Monitor : public QQuickItem {
    Q_OBJECT
    Q_PROPERTY(Editor* editor READ editor WRITE setEditor NOTIFY editorChanged)
public:
    explicit Monitor(QQuickItem* parent = nullptr) : QQuickItem(parent) { setFlag(ItemHasContents); }
    void showImage(const QImage& next) { frame = next; fresh = true; update(); }
    QSGNode* updatePaintNode(QSGNode* old, UpdatePaintNodeData*) override;
    Editor* editor() const { return source; }
    void setEditor(Editor* next);
signals:
    void editorChanged();
private:
    QImage frame;
    bool fresh = false;
    Editor* source = nullptr;
    QMetaObject::Connection link;
};

class Editor : public QObject {
    Q_OBJECT
    Q_PROPERTY(Lyrics* lyrics READ lyrics CONSTANT)
    Q_PROPERTY(ComfyJobs* comfy READ comfy CONSTANT)
    Q_PROPERTY(Workflows* workflows READ workflows CONSTANT)
    Q_PROPERTY(Generator* generator READ generator CONSTANT)
    Q_PROPERTY(Requests* requests READ requests CONSTANT)
    Q_PROPERTY(Plan* plan READ plan CONSTANT)
    Q_PROPERTY(AgentBridge* agent READ agent CONSTANT)
    Q_PROPERTY(Notifier* notifier READ notifier CONSTANT)
    Q_PROPERTY(Renderer* renderer READ renderer CONSTANT)
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
    // Track switches, kept with the project: hidden picture tracks ("v1", "v2") drop out of the
    // viewer, hidden "lyrics" leave the viewer's lyric line off, and a muted "a1" is silent in the
    // monitor. None of them changes the plan or the exported timeline.
    Q_PROPERTY(QStringList hiddenTracks READ hiddenTracks NOTIFY tracksChanged)
    Q_PROPERTY(QStringList mutedTracks READ mutedTracks NOTIFY tracksChanged)
public:
    Editor();
    Lyrics* lyrics() { return &lyricState; }
    ComfyJobs* comfy() { return comfyJobs; }
    Workflows* workflows() { return workflowLibrary; }
    Generator* generator() { return generatorJobs; }
    Requests* requests() { return requestThreads; }
    Plan* plan() { return planState; }
    AgentBridge* agent() { return agentBridge; }
    Notifier* notifier() { return notices; }
    Project* current() { return project.get(); }
    Q_INVOKABLE void seekLyric(int startMs);
    Q_INVOKABLE void playLyric(int startMs, int endMs);
    // Plays [startMs, endMs); with looping on, the passage repeats.
    Q_INVOKABLE void playRange(int startMs, int endMs) { playLyric(startMs, endMs); }
    ~Editor();
    Q_INVOKABLE bool openProject(const QUrl& folder);
    // Clicking anywhere outside the text field being edited leaves it (which commits it), so a
    // field never holds the keyboard until another field is clicked.
    Q_INVOKABLE void releaseFocusOnClick(QObject* window);
    // The song from startSeconds for seconds, as a 48 kHz stereo 16-bit WAV at path; silence
    // before the song starts or after it ends. "" or why not. For a lip-sync take's audio.
    QString writeExcerpt(double startSeconds, double seconds, const QString& path) const;
    // A video file's {width, height, frames, fps} and a still from its middle saved at thumb
    // (JPEG); empty when it can't be read.
    static QVariantMap probeVideo(const QString& path, const QString& thumb);
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
    QStringList hiddenTracks() const { return hidden; }
    QStringList mutedTracks() const { return muted; }
    Q_INVOKABLE void setTrackHidden(const QString& track, bool hide);
    Q_INVOKABLE void setTrackMuted(const QString& track, bool mute);
    // The frame the viewers show now.
    QImage currentImage() const { return shown; }
    Q_INVOKABLE void seek(int frame);
    Q_INVOKABLE void togglePlay();
    Q_INVOKABLE void change(const QString& mode, int frames);
    Q_INVOKABLE void undo();
    Q_INVOKABLE void redo();
    Q_INVOKABLE bool exportTimeline(const QUrl& destination);
    Q_INVOKABLE void loop(bool enabled) { if (loopOn != enabled) { loopOn = enabled; emit loopingChanged(); } }
    bool looping() const { return loopOn; }
    QUrl defaultExport() const { return project ? QUrl::fromLocalFile(project->dir.filePath("Chorus.xml")) : QUrl(); }

    // How to assemble the cut. Playback leaves out hidden tracks; a render shows every track,
    // fits pictures of another shape to the frame, and puts a card where a shot has no take.
    struct Cut {
        QStringList hidden;
        QString fit;               // "" (playback), "fill", "fit" or "stretch"
        QHash<int, QString> cards; // shot id -> placeholder picture
    };
    void assemble(Mlt::Tractor& tractor, Mlt::Profile& profile, const Cut& cut);
    Renderer* renderer() { return render; }
    QString projectDir() const;
    QSize pictureSize() const;
    int fpsNum() const;
    int fpsDen() const;

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
    void tracksChanged();
private:
    QStringList hidden, muted;
    QImage shown;
    void loadTracks();
    void saveTracks();
    std::unique_ptr<Project> project;
    Lyrics lyricState;
    ComfyJobs* comfyJobs;
    Workflows* workflowLibrary;
    Generator* generatorJobs;
    Requests* requestThreads;
    Plan* planState;
    AgentBridge* agentBridge;
    Notifier* notices;
    Renderer* render;
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
    QString takesKey;   // what the take tracks were built from, to rebuild only when it changes
    void stop();
    bool commit(Trim value);
    void info(const QString& text);
    static void onFrame(mlt_properties, void* self, mlt_event_data data);
};
