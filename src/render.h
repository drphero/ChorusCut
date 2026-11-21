#pragma once
#include <QObject>
#include <QSize>
#include <QTemporaryDir>
#include <QTimer>
#include <QVariantMap>
#include <memory>

class Editor;
namespace Mlt { class Profile; class Tractor; class Consumer; }

// Renders the cut to a video file: the same assembly playback shows (every track, whatever the
// viewer's switches), at a chosen size, with pictures of another shape filled, fitted or
// stretched, a card for each shot with no take yet, and the master song as the only sound.
// H.264 (x264, CRF) and AAC in an MP4. It runs on MLT's own thread; the UI follows progress.
class Renderer : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(QString message READ message NOTIFY stateChanged)
    Q_PROPERTY(QString file READ file NOTIFY stateChanged)
    Q_PROPERTY(int crf READ crf WRITE setCrf NOTIFY crfChanged)
public:
    explicit Renderer(Editor* editor);
    ~Renderer() override;
    bool busy() const { return running; }
    double progress() const { return done; }
    QString message() const { return note; }
    QString file() const { return finishedFile; }
    int crf() const { return quality; }
    void setCrf(int value);

    // What the dialog offers: {native: {width, height}, aspect, sizes: [{label, width, height}],
    // missing: [shot numbers without a take], shots, name, frames}.
    Q_INVOKABLE QVariantMap options() const;
    // A free path in the project's Renders folder, named after the project and the part rendered.
    Q_INVOKABLE QString defaultPath(const QString& part) const;
    // {startMs, endMs (both -1 for the whole song), width, height, fit, crf, path}. False with
    // the reason in message when it can't start.
    Q_INVOKABLE bool start(const QVariantMap& request);
    Q_INVOKABLE void cancel();
    Q_INVOKABLE void reveal() const;
    // The frames [first, last) a request covers: the start rounds down, the end up.
    static QPair<int, int> frames(double fps, int length, int startMs, int endMs);
signals:
    void stateChanged();
    void progressChanged();
    void crfChanged();
    void finished(bool ok, const QString& path);
private:
    Editor* editor;
    std::unique_ptr<Mlt::Profile> profile;
    std::unique_ptr<Mlt::Tractor> tractor;
    std::unique_ptr<Mlt::Consumer> consumer;
    std::unique_ptr<QTemporaryDir> cardDir;
    QTimer poll;
    bool running = false;
    double done = 0;
    QString note, finishedFile, target, part;
    int first = 0, last = 0, quality = 19;
    void check();
    void finish(bool ok, const QString& text);
    QString card(int shot, QSize size);
};
