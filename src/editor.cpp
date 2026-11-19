#include "editor.h"
#include "library.h"
#include <QPainter>
#include <QSqlQuery>
#include <QSqlError>
#include <QFileInfo>
#include <cmath>
#include <QFutureWatcher>
#include <QtConcurrentRun>

static QImage decode(Mlt::Frame& frame, QSize size) {
    int width = size.width(), height = size.height();
    auto format = mlt_image_rgb;
    auto pixels = frame.get_image(format, width, height);
    return pixels && format == mlt_image_rgb ? QImage(pixels, width, height, width * 3, QImage::Format_RGB888).copy() : QImage{};
}
void Monitor::paint(QPainter* p) {
    if (frame.isNull()) return;
    QSizeF fit = frame.size();
    fit.scale(boundingRect().size(), Qt::KeepAspectRatio);
    QRectF rect(QPointF(), fit);
    rect.moveCenter(boundingRect().center());
    p->drawImage(rect, frame);
}
namespace {
// One undoable picture edit. The trim is already committed when the command is pushed.
class TrimCommand : public QUndoCommand {
public:
    TrimCommand(Editor* editor, Trim before, Trim after, const QString& label)
        : QUndoCommand(label), editor(editor), before(before), after(after) {}
    void redo() override { if (skip) { skip = false; return; } editor->applyTrim(after); }
    void undo() override { editor->applyTrim(before); }
private:
    Editor* editor;
    Trim before, after;
    bool skip = true;
};
}

Editor::Editor() {
    comfyJobs = new ComfyJobs(this);
    lyricState.setHistory(&history);
    planState = new Plan(&lyricState, this);
    planState->setHistory(&history);
    agentBridge = new AgentBridge(this, this);  // serves agents once start() is called
    auto changed = [this] { emit edited(); };
    connect(&history, &QUndoStack::indexChanged, this, changed);
    connect(&history, &QUndoStack::cleanChanged, this, changed);
    // Replacing the lyric text or alignment renumbers words, so older steps no longer apply.
    connect(&lyricState, &Lyrics::historyInvalidated, this, [this] { history.clear(); emit edited(); });
    // Keep the Project Manager's card current: after peaks arrive and shortly after lyric edits.
    thumbTimer.setSingleShot(true);
    thumbTimer.setInterval(1500);
    connect(&thumbTimer, &QTimer::timeout, this, &Editor::saveThumbnail);
    connect(&lyricState, &Lyrics::changed, this, [this] { if (peaksReady()) thumbTimer.start(); });
    connect(this, &Editor::peaksChanged, this, [this] { if (peaksReady()) saveThumbnail(); });
}

Editor::~Editor() { saveThumbnail(); stop(); }

void Editor::closeProject() {
    if (!project) return;
    saveThumbnail();
    thumbTimer.stop();
    comfyJobs->detach();
    stop();
    listener.reset(); output.reset(); timeline.reset();
    lyricState.unload();
    planState->unload();
    project.reset();
    thumbPicture = {};
    ++peakSerial;
    peakData.clear();
    history.clear();
    cursor = 0;
    info({});
    emit imageReady(QImage());
    emit peaksChanged(); emit projectChanged(); emit edited(); emit cursorChanged();
}

void Editor::saveThumbnail() {
    if (!project || !peaksReady()) return;
    constexpr int W = 480, H = 270;
    const bool picture = sourceLength() > 0;
    if (picture && thumbPicture.isNull() && !running && timeline) {
        thumbPicture = imageAt(length() / 3);
        timeline->seek(cursor);
    }
    QImage image(W, H, QImage::Format_RGB32);
    image.fill(QColor("#0c1216"));
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    if (!thumbPicture.isNull()) {
        const QImage fill = thumbPicture.scaled(W, H, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);
        p.drawImage(QPoint((W - fill.width()) / 2, (H - fill.height()) / 2), fill);
        QLinearGradient shade(0, H * 0.55, 0, H);
        shade.setColorAt(0, QColor(12, 18, 22, 0));
        shade.setColorAt(1, QColor(12, 18, 22, 220));
        p.fillRect(image.rect(), shade);
    }
    // Song waveform: the whole song across the card.
    const int buckets = peakData.size() / 2;
    const double mid = picture ? H * 0.80 : H * 0.46, half = picture ? H * 0.12 : H * 0.30;
    p.setPen(QPen(QColor(135, 222, 199, picture ? 200 : 170), 1));
    for (int x = 0; x < W && buckets > 0; ++x) {
        int a = int(qint64(x) * buckets / W), b = qMax(a + 1, int(qint64(x + 1) * buckets / W));
        float lo = peakData[2 * a], hi = peakData[2 * a + 1];
        for (int i = a + 1; i < b && i < buckets; ++i) { lo = qMin(lo, peakData[2 * i]); hi = qMax(hi, peakData[2 * i + 1]); }
        p.drawLine(QPointF(x + 0.5, mid - hi * half), QPointF(x + 0.5, mid - lo * half + 0.5));
    }
    // Lyric lines along the bottom, amber where a line still needs checking.
    const double songMs = 1000.0 * length() / fps();
    if (songMs > 0) {
        for (const auto& value : lyricState.lines()) {
            const auto line = value.toMap();
            const double x0 = W * line["start"].toInt() / songMs, x1 = W * line["end"].toInt() / songMs;
            p.fillRect(QRectF(x0, H - 9, qMax(1.5, x1 - x0 - 1), 5),
                       line["flag"].toBool() ? QColor("#e7b75f") : QColor("#87dec7"));
        }
    }
    p.end();
    image.save(ProjectLibrary::thumbnailPath(project->dir.absolutePath()), "JPG", 88);
}
void Editor::info(const QString& text) { message = text; emit messageChanged(); }
QString Editor::projectName() const { return project ? project->dir.dirName() : QString(); }
QString Editor::performanceName() const { return project ? QFileInfo(project->performance.path).fileName() : QString(); }
QString Editor::cutawayName() const { return project ? QFileInfo(project->cutaway.path).fileName() : QString(); }
QString Editor::masterName() const { return project ? QFileInfo(project->master.path).fileName() : QString(); }

void Editor::createProject(const QUrl& parent, const QString& name, const QUrl& song,
                           const QUrl& base, const QUrl& insert) {
    if (importing()) return;
    QString leaf = name.trimmed();
    if (!parent.isLocalFile() || leaf.isEmpty() || leaf == "." || leaf == ".." || leaf.contains('/')) {
        info("Choose a parent folder and enter a project name");
        emit importFinished(false);
        return;
    }
    const QString path = QDir(parent.toLocalFile()).filePath(leaf);
    importInProgress = true;
    emit importingChanged();
    auto watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [this, path, watcher] {
        const QString error = watcher->result();
        watcher->deleteLater();
        if (!error.isEmpty()) info(error);
        const bool success = error.isEmpty() && openProject(QUrl::fromLocalFile(path));
        importInProgress = false;
        emit importingChanged();
        emit importFinished(success);
    });
    watcher->setFuture(QtConcurrent::run([path,song,base,insert] {
        QString error;
        if (!Project::create(path, song.toLocalFile(), base.toLocalFile(), insert.toLocalFile(), error) && error.isEmpty())
            return QStringLiteral("Could not create the project");
        return error;
    }));
}

bool Editor::openProject(const QUrl& folder) {
    if (!folder.isLocalFile()) { info("Choose a local project folder"); return false; }
    QString error;
    auto next = Project::open(folder.toLocalFile(), error);
    if (!next) { info(error); return false; }
    saveThumbnail();  // The outgoing project's card, before its lyrics are replaced.
    if (!lyricState.load(*next,error) || !planState->load(*next,error)) {
        info(error);
        if (project) { QString ignored; lyricState.load(*project, ignored); planState->load(*project, ignored); }
        return false;
    }
    thumbPicture = {};
    stop();
    listener.reset(); output.reset(); timeline.reset();
    project = std::move(next);
    profile = project->previewProfile();
    previewSize = QSize(profile->width(), profile->height());
    trim = project->trim;
    cursor = 0;
    history.clear();
    build();
    computePeaks();
    info({});
    emit projectChanged(); emit edited();
    seek(0);
    comfyJobs->resume();
    return true;
}

static Mlt::Producer producer(Project& project, Mlt::Profile& profile, const Media& media, bool audio) {
    Mlt::Producer p(profile, "loader", project.file(media).toUtf8());
    p.set(audio ? "audio_index" : "video_index", media.stream);
    p.set(audio ? "video_index" : "audio_index", -1);
    return p;
}

void Editor::build() {
    stop();
    listener.reset();
    output.reset();
    timeline = std::make_unique<Mlt::Tractor>(*profile);
    auto audio = producer(*project, *profile, project->master, true);
    Mlt::Producer black(*profile, "color", "black");
    black.set("length", length());
    black.set_in_and_out(0,length()-1);
    Mlt::Playlist picture(*profile);
    if (sourceLength() == 0) {
        picture.append(black,0,length()-1);
    } else {
        auto base = producer(*project, *profile, project->performance, false);
        auto insert = producer(*project, *profile, project->cutaway, false);
        auto appendBase = [&](int start, int end) {
            int stop = qMin(end, baseLength());
            if (start < stop) picture.append(base, start, stop-1);
            int gap = end - qMax(start, stop);
            if (gap > 0) {
                picture.append(black,0,gap-1);
            }
        };
        appendBase(0,trim.start);
        picture.append(insert,trim.source,trim.source + trim.end - trim.start - 1);
        appendBase(trim.end,length());
    }
    picture.set("hide",2); // MLT hide-audio flag.
    audio.set("hide",1);  // MLT hide-video flag.
    timeline->set_track(audio,0);
    timeline->set_track(picture,1);
    timeline->set_in_and_out(0,length()-1);
    timeline->set_speed(0);
}
QImage Editor::imageAt(int position) {
    timeline->seek(position);
    std::unique_ptr<Mlt::Frame> f(timeline->get_frame());
    return f ? decode(*f, previewSize) : QImage{};
}
void Editor::stop() {
    passageEnd = -1;
    passageStart = -1;
    ++generation;
    if (output && !output->is_stopped()) output->stop();
    if (timeline) timeline->set_speed(0);
    if (running) { running=false; emit playbackChanged(); }
}
void Editor::seek(int position) {
    if (!ready()) return;
    stop();
    cursor = qBound(0,position,length()-1);
    emit imageReady(imageAt(cursor));
    emit cursorChanged();
}
void Editor::seekLyric(int startMs) {
    seek(qFloor(startMs*fps()/1000.));
}
void Editor::playLyric(int startMs, int endMs) {
    if (!ready() || endMs<=startMs) return;
    seekLyric(startMs);
    passageStart=cursor;
    passageEnd=qBound(cursor+1,qCeil(endMs*fps()/1000.),length());
    togglePlay();
}
void Editor::onFrame(mlt_properties, void* self, mlt_event_data data) {
    auto e = static_cast<Editor*>(self);
    const int serial = e->generation.load();
    Mlt::Frame f(mlt_event_data_to_frame(data));
    const int position = f.get_position();
    const QImage img = decode(f, e->previewSize);
    QMetaObject::invokeMethod(e,[e,serial,position,img] {
        if (serial != e->generation.load() || !e->running) return;
        e->cursor=qBound(0,position,e->length()-1);
        emit e->imageReady(img); emit e->cursorChanged();
        if (e->passageEnd>0 && position>=e->passageEnd-1) {
            const int start=e->passageStart, end=e->passageEnd;
            e->seek(end-1);
            if (e->loopOn && start>=0) {
                e->seek(start);
                e->passageStart=start; e->passageEnd=end;
                e->togglePlay();
            }
        } else if (position >= e->length()-1) {
            e->seek(e->length()-1);
            if (e->loopOn) e->togglePlay();
        }
    },Qt::QueuedConnection);
}
void Editor::setMonitorMuted(bool muted) {
    if (muted == monitorMuted()) return;
    mutedOutput = muted;
    updateMonitorGain();
    emit monitorChanged();
}
void Editor::setMonitorDb(int dbValue) {
    dbValue = qBound(-60, dbValue, 0);
    if (dbValue == levelDb) return;
    levelDb = dbValue;
    updateMonitorGain();
    emit monitorChanged();
}
void Editor::updateMonitorGain() {
    if (output) output->set("volume", mutedOutput ? 0.0 : std::pow(10.0, levelDb / 20.0));
}
void Editor::togglePlay() {
    if (!ready()) return;
    if (running) { seek(cursor); return; }
    if (cursor == length()-1 && passageEnd<0) cursor=0;
    if (!output) {
        output=std::make_unique<Mlt::Consumer>(*profile,"sdl2_audio");
        if (!output->is_valid()) { info("MLT SDL2 audio output unavailable"); output.reset(); return; }
        updateMonitorGain();
        output->set("real_time",1);
        output->set("buffer",5);
        output->set("prefill",2);
        output->set("terminate_on_pause",0);
        output->set("mlt_image_format","rgb");
        listener.reset(output->listen("consumer-frame-show",this,reinterpret_cast<mlt_listener>(onFrame)));
        output->connect(*timeline);
    }
    output->purge();
    timeline->seek(cursor);
    timeline->set_speed(1);
    running=true; emit playbackChanged();
    if (output->start()) { stop(); info("Audio playback could not start"); }
}
bool Editor::commit(Trim value) {
    QSqlQuery q(project->db);
    q.prepare("UPDATE cutaway SET start=?,end=?,source=? WHERE id=1 AND start=? AND end=? AND source=?");
    q.addBindValue(value.start); q.addBindValue(value.end); q.addBindValue(value.source);
    q.addBindValue(trim.start); q.addBindValue(trim.end); q.addBindValue(trim.source);
    if (!q.exec()) { info(q.lastError().text()); return false; }
    if (q.numRowsAffected() != 1) { info("The saved cut changed. Reopen the project to edit it."); return false; }
    const bool resume = running;
    trim=value; build(); seek(cursor);
    if (resume) togglePlay();
    return true;
}
void Editor::change(const QString& mode,int frames) {
    if (!ready() || !sourceLength()) return;
    Trim next=trim;
    if (mode=="in") {
        int delta=qBound(qMax(-next.start,-next.source),frames,next.end-next.start-1);
        next.start+=delta; next.source+=delta;
    } else if (mode=="out") {
        next.end += qBound(next.start+1-next.end,frames,qMin(length()-next.end,sourceLength()-next.source-(next.end-next.start)));
    } else if (mode=="move") {
        int delta=qBound(-next.start,frames,length()-next.end);
        next.start+=delta; next.end+=delta;
    } else if (mode=="slip") {
        next.source += qBound(-next.source,frames,sourceLength()-(next.end-next.start)-next.source);
    } else return;
    Trim previous=trim;
    if (next==trim || !commit(next)) return;
    static const QHash<QString,QString> labels{{"in","Trim cutaway in"},{"out","Trim cutaway out"},
                                               {"move","Move cutaway"},{"slip","Slip cutaway"}};
    history.push(new TrimCommand(this, previous, next, labels.value(mode)));
    emit edited();
}
void Editor::undo() { history.undo(); }
void Editor::redo() { history.redo(); }

void Editor::computePeaks() {
    peakData.clear();
    emit peaksChanged();
    if (!project) return;
    const QString path = project->file(project->master);
    const int num = project->fpsNum, den = project->fpsDen, frames = length();
    const int serial = ++peakSerial;
    auto watcher = new QFutureWatcher<QVector<float>>(this);
    connect(watcher, &QFutureWatcher<QVector<float>>::finished, this, [this, watcher, serial] {
        if (serial == peakSerial) { peakData = watcher->result(); emit peaksChanged(); }
        watcher->deleteLater();
    });
    watcher->setFuture(QtConcurrent::run([path, num, den, frames] {
        // Decode through MLT's loader, the same path playback uses, at a low rate.
        constexpr int rate = 8000, bucket = rate * kPeakMs / 1000;
        Mlt::Profile profile("atsc_720p_24");
        profile.set_frame_rate(num, den);
        profile.set_explicit(1);
        Mlt::Producer producer(profile, "loader", path.toUtf8());
        QVector<float> out;
        if (!producer.is_valid()) return out;
        producer.set("video_index", -1);
        const double fps = double(num) / den;
        float lo = 0, hi = 0, loudest = 0;
        int filled = 0;
        for (int pos = 0; pos < frames; ++pos) {
            std::unique_ptr<Mlt::Frame> frame(producer.get_frame());
            if (!frame) break;
            auto format = mlt_audio_f32le;
            int frequency = rate, channels = 1;
            int samples = mlt_audio_calculate_frame_samples(fps, rate, pos);
            auto pcm = static_cast<const float*>(frame->get_audio(format, frequency, channels, samples));
            if (!pcm || format != mlt_audio_f32le || channels < 1) continue;
            for (int i = 0; i < samples; ++i) {
                float v = 0;
                for (int c = 0; c < channels; ++c) v += pcm[i * channels + c];
                v = std::isfinite(v) ? v / channels : 0.f;
                lo = filled ? qMin(lo, v) : v;
                hi = filled ? qMax(hi, v) : v;
                if (++filled == bucket) {
                    out << lo << hi;
                    loudest = qMax(loudest, qMax(-lo, hi));
                    filled = 0;
                }
            }
        }
        if (loudest > 0)
            for (auto& v : out) v /= loudest;
        return out;
    }));
}

bool Editor::checkPictures() {
    auto base = producer(*project, *profile, project->performance, false);
    auto insert = producer(*project, *profile, project->cutaway, false);
    for (int position : {0, trim.start-1, trim.start, trim.end-1, trim.end, baseLength()-1, baseLength(), length()-1}) {
        if (position<0 || position>=length()) continue;
        const bool covered = position>=trim.start && position<trim.end;
        auto preview = imageAt(position);
        if (preview.isNull()) return false;
        if (!covered && position >= baseLength()) {
            for (int y=0; y<preview.height(); ++y)
                for (int x=0; x<preview.width(); ++x)
                    if (qGray(preview.pixel(x,y)) > 2) return false;
            continue;
        }
        auto& source = covered ? insert : base;
        source.seek(covered ? trim.source+position-trim.start : position);
        std::unique_ptr<Mlt::Frame> expected(source.get_frame());
        if (!expected || preview != decode(*expected,previewSize)) return false;
    }
    return true;
}

bool Editor::checkMedia() {
    if (!checkPictures()) return false;
    // Check the format SDL actually consumes, across the entire example.
    timeline->set_speed(1);
    timeline->seek(0);
    double peak = 0;
    for (int position = 0; position < length(); ++position) {
        std::unique_ptr<Mlt::Frame> frame(timeline->get_frame());
        auto format = mlt_audio_f32le;
        int frequency = 48000, channels = 2, samples = 2000;
        auto pcm = static_cast<const float*>(frame->get_audio(format, frequency, channels, samples));
        if (!pcm || format != mlt_audio_f32le || channels != 2 || samples != 2000 || frequency != 48000)
            return false;
        for (int i = 0; i < samples * channels; ++i) {
            if (!std::isfinite(pcm[i])) return false;
            peak = qMax(peak, std::abs(static_cast<double>(pcm[i])));
        }
    }
    timeline->set_speed(0);
    return peak > 0.001 && peak <= 0.013;
}
