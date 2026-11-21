#include "editor.h"
#include "library.h"
#include "notify.h"
#include <QPainter>
#include <QSqlQuery>
#include <QSqlError>
#include <QFileInfo>
#include <cmath>
#include <algorithm>
#include <QFutureWatcher>
#include <QtConcurrentRun>
#include <QQuickWindow>
#include <QSGImageNode>
#include <QMouseEvent>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>

static QImage decode(Mlt::Frame& frame, QSize size) {
    int width = size.width(), height = size.height();
    auto format = mlt_image_rgb;
    auto pixels = frame.get_image(format, width, height);
    return pixels && format == mlt_image_rgb ? QImage(pixels, width, height, width * 3, QImage::Format_RGB888).copy() : QImage{};
}
void Monitor::setEditor(Editor* next) {
    if (next == source) return;
    disconnect(link);
    source = next;
    if (source) {
        link = connect(source, &Editor::imageReady, this, &Monitor::showImage);
        showImage(source->currentImage());
    }
    emit editorChanged();
}
QSGNode* Monitor::updatePaintNode(QSGNode* old, UpdatePaintNodeData*) {
    auto node = static_cast<QSGImageNode*>(old);
    if (frame.isNull() || !window()) { delete node; return nullptr; }
    if (!node) {
        node = window()->createImageNode();
        node->setOwnsTexture(true);  // setTexture then deletes the previous frame's texture
        node->setFiltering(QSGTexture::Linear);
        node->setMipmapFiltering(QSGTexture::Linear);
        fresh = true;
    }
    if (fresh) {
        node->setTexture(window()->createTextureFromImage(frame, QQuickWindow::TextureHasMipmaps));
        fresh = false;
    }
    QSizeF fit = frame.size();
    fit.scale(boundingRect().size(), Qt::KeepAspectRatio);
    QRectF rect(QPointF(), fit);
    rect.moveCenter(boundingRect().center());
    node->setRect(rect);
    return node;
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
    workflowLibrary = new Workflows(this);
    lyricState.setHistory(&history);
    planState = new Plan(&lyricState, this);
    planState->setHistory(&history);
    generatorJobs = new Generator(this);  // after the plan and the workflows it uses
    requestThreads = new Requests(this);
    agentBridge = new AgentBridge(this, this);  // after the requests it delivers; serves agents once start() is called
    notices = new Notifier(this);
    render = new Renderer(this);
    connect(generatorJobs, &Generator::finished, notices, &Notifier::jobFinished);
    connect(this, &Editor::imageReady, this, [this](const QImage& image) { shown = image; });
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
    // Choosing, adding or moving takes changes what the cut plays.
    connect(planState, &Plan::changed, this, [this] {
        if (!project || !timeline) return;
        const QString key = QString::fromUtf8(QJsonDocument(QJsonArray::fromVariantList(planState->timelineTakes())).toJson(QJsonDocument::Compact));
        if (key == takesKey) return;
        const int at = cursor;
        build();
        seek(at);
    });
}

Editor::~Editor() { saveThumbnail(); stop(); }

void Editor::closeProject() {
    if (!project) return;
    render->cancel();
    saveThumbnail();
    thumbTimer.stop();
    comfyJobs->detach();
    generatorJobs->detach();
    requestThreads->unload();
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

namespace {
// Watches presses before the window delivers them: one outside the focused text field takes the
// keyboard away from it (its focus scope, such as an open dialog, keeps it), so the field
// commits and its selection clears. A press on another field then focuses that one as usual.
class FocusRelease : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (event->type() != QEvent::MouseButtonPress) return false;
        auto* window = qobject_cast<QQuickWindow*>(watched);
        QQuickItem* focused = window ? window->activeFocusItem() : nullptr;
        if (!focused || !(focused->inherits("QQuickTextInput") || focused->inherits("QQuickTextEdit"))) return false;
        const auto* press = static_cast<QMouseEvent*>(event);
        if (!focused->contains(focused->mapFromScene(press->scenePosition()))) focused->setFocus(false);
        return false;
    }
};
}

void Editor::releaseFocusOnClick(QObject* window) {
    auto* w = qobject_cast<QQuickWindow*>(window);
    if (!w || w->findChild<QObject*>("focusRelease", Qt::FindDirectChildrenOnly)) return;
    auto* filter = new FocusRelease(w);
    filter->setObjectName("focusRelease");
    w->installEventFilter(filter);
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
    loadTracks();
    build();
    computePeaks();
    info({});
    emit projectChanged(); emit edited();
    seek(0);
    comfyJobs->resume();
    generatorJobs->resume();
    requestThreads->load();
    return true;
}

static Mlt::Producer producer(Project& project, Mlt::Profile& profile, const Media& media, bool audio) {
    Mlt::Producer p(profile, "loader", project.file(media).toUtf8());
    p.set(audio ? "audio_index" : "video_index", media.stream);
    p.set(audio ? "video_index" : "audio_index", -1);
    return p;
}

// A render's pictures of another shape: Fill crops the overflow, Stretch scales each axis on its
// own, Fit (MLT's default) pads with black. Playback leaves them as they are.
// The source's size comes from the project or the take's record: MLT may not know it yet here.
static void fitPicture(Mlt::Producer& picture, Mlt::Profile& profile, const QString& fit, QSize source) {
    if (fit != "fill" && fit != "stretch") return;
    const int w = source.width(), h = source.height();
    if (fit == "stretch") {
        if (w <= 0 || h <= 0) return;
        picture.set("force_aspect_ratio", (double(profile.width()) / profile.height()) / (double(w) / h));
    } else {
        // Left inactive, the filter only works out the centre crop; MLT's own crop step (which the
        // loader adds) applies it. Made active, it ignores center and crops nothing.
        Mlt::Filter crop(profile, "crop");
        crop.set("center", 1);
        picture.attach(crop);
    }
}

void Editor::assemble(Mlt::Tractor& tractor, Mlt::Profile& p, const Cut& cut) {
    auto audio = producer(*project, p, project->master, true);
    Mlt::Producer black(p, "color", "black");
    black.set("length", length());
    black.set_in_and_out(0,length()-1);
    Mlt::Playlist picture(p);
    const bool showV1 = !cut.hidden.contains("v1"), showV2 = !cut.hidden.contains("v2");
    if (sourceLength() == 0 || (!showV1 && !showV2)) {
        picture.append(black,0,length()-1);
    } else {
        auto base = producer(*project, p, project->performance, false);
        auto insert = producer(*project, p, project->cutaway, false);
        fitPicture(base, p, cut.fit, QSize(project->performance.width, project->performance.height));
        fitPicture(insert, p, cut.fit, QSize(project->cutaway.width, project->cutaway.height));
        // A hidden V1 leaves black where the performance was; a hidden V2 shows the
        // performance through the cutaway.
        auto appendBase = [&](int start, int end) {
            int stop = showV1 ? qMin(end, baseLength()) : start;
            if (start < stop) picture.append(base, start, stop-1);
            int gap = end - qMax(start, stop);
            if (gap > 0) {
                picture.append(black,0,gap-1);
            }
        };
        if (showV2) {
            appendBase(0,trim.start);
            picture.append(insert,trim.source,trim.source + trim.end - trim.start - 1);
            appendBase(trim.end,length());
        } else {
            appendBase(0,length());
        }
    }
    picture.set("hide",2); // MLT hide-audio flag.
    audio.set("hide",1);  // MLT hide-video flag.
    tractor.set_track(audio,0);
    tractor.set_track(picture,1);
    // Each shot's chosen take on V1 and V2, silent: the song is the soundtrack. With no
    // transitions MLT shows the topmost track that has a picture, so V2 covers V1 and both
    // cover the base picture; gaps show what is below. A render puts a card where a shot has none.
    struct Clip { int start, end, in; QString file; bool card; QSize size; };
    const auto takes = planState->timelineTakes();
    for (int track : {1, 2}) {
        QList<Clip> list;
        if (!cut.hidden.contains(track == 1 ? "v1" : "v2")) {
            for (const auto& v : takes) {
                const auto t = v.toMap();
                if (t["track"].toInt() != track) continue;
                const auto take = planState->item("take", t["take"].toInt());
                list.append({t["start"].toInt(), t["end"].toInt(), t["in"].toInt(), t["file"].toString(), false, QSize(take["width"].toInt(), take["height"].toInt())});
            }
            for (auto it = cut.cards.begin(); it != cut.cards.end(); ++it) {
                const auto s = planState->shot(it.key());
                if (s["track"].toInt() == track) list.append({s["start"].toInt(), s["end"].toInt(), 0, it.value(), true, QSize()});
            }
            std::sort(list.begin(), list.end(), [](const Clip& a, const Clip& b) { return a.start < b.start; });
        }
        Mlt::Playlist clips(p);
        int at = 0;
        for (const Clip& c : list) {
            if (c.start < at || c.end <= c.start) continue;
            if (c.start > at) clips.blank(c.start - at - 1);
            Mlt::Producer clip(p, c.card ? "qimage" : "loader", c.file.toUtf8());
            if (!clip.is_valid()) continue;
            if (c.card) {
                clip.set("length", c.end - c.start);
                clip.set_in_and_out(0, c.end - c.start - 1);
            } else {
                clip.set("audio_index", -1);
                fitPicture(clip, p, cut.fit, c.size);
            }
            clips.append(clip, c.in, c.in + (c.end - c.start) - 1);
            at = c.end;
        }
        if (at < length()) clips.blank(length() - at - 1);
        clips.set("hide", 2);
        tractor.set_track(clips, 1 + track);
    }
}

void Editor::build() {
    stop();
    listener.reset();
    output.reset();
    timeline = std::make_unique<Mlt::Tractor>(*profile);
    takesKey = QString::fromUtf8(QJsonDocument(QJsonArray::fromVariantList(planState->timelineTakes())).toJson(QJsonDocument::Compact));
    assemble(*timeline, *profile, Cut{hidden, QString(), {}});
    timeline->set_in_and_out(0,length()-1);
    timeline->set_speed(0);
}
QString Editor::projectDir() const { return project ? project->dir.absolutePath() : QString(); }
QSize Editor::pictureSize() const { return project ? QSize(project->performance.width, project->performance.height) : QSize(); }
int Editor::fpsNum() const { return project ? project->fpsNum : 24; }
int Editor::fpsDen() const { return project ? project->fpsDen : 1; }
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
    // The master track is the monitor's only sound, so muting A1 silences it like Mute does.
    const bool silent = mutedOutput || muted.contains("a1");
    if (output) output->set("volume", silent ? 0.0 : std::pow(10.0, levelDb / 20.0));
}
static const QStringList kTracks{"v1", "v2", "lyrics", "a1"};
void Editor::loadTracks() {
    hidden.clear(); muted.clear();
    QSqlQuery q(project->db);
    if (q.exec("SELECT value FROM view_state WHERE key='tracks'") && q.next()) {
        const auto o = QJsonDocument::fromJson(q.value(0).toString().toUtf8()).object();
        for (const auto& v : o["hidden"].toArray()) if (kTracks.contains(v.toString()) && v.toString() != "a1") hidden << v.toString();
        for (const auto& v : o["muted"].toArray()) if (v.toString() == "a1") muted << v.toString();
    }
    emit tracksChanged();
}
void Editor::saveTracks() {
    if (!project) return;
    QSqlQuery q(project->db);
    q.exec("CREATE TABLE IF NOT EXISTS view_state(key TEXT PRIMARY KEY, value TEXT NOT NULL)");
    q.prepare("INSERT OR REPLACE INTO view_state VALUES('tracks', ?)");
    q.addBindValue(QString::fromUtf8(QJsonDocument(QJsonObject{{"hidden", QJsonArray::fromStringList(hidden)},
                                                               {"muted", QJsonArray::fromStringList(muted)}}).toJson(QJsonDocument::Compact)));
    if (!q.exec()) info("Could not keep the track switches: " + q.lastError().text());
}
void Editor::setTrackHidden(const QString& track, bool hide) {
    if (!kTracks.contains(track) || track == "a1" || hidden.contains(track) == hide) return;
    if (hide) hidden << track; else hidden.removeAll(track);
    saveTracks();
    emit tracksChanged();
    if (track != "lyrics" && project && timeline) { const int at = cursor; build(); seek(at); }
}
void Editor::setTrackMuted(const QString& track, bool mute) {
    if (track != "a1" || muted.contains(track) == mute) return;
    if (mute) muted << track; else muted.removeAll(track);
    saveTracks();
    updateMonitorGain();
    emit tracksChanged();
}
void Editor::togglePlay() {
    if (!ready()) return;
    if (running) { seek(cursor); return; }
    if (cursor == length()-1 && passageEnd<0) cursor=0;
    if (!output) {
        output=std::make_unique<Mlt::Consumer>(*profile,"sdl2_audio");
        if (!output->is_valid()) { info("MLT SDL2 audio output unavailable"); output.reset(); return; }
        updateMonitorGain();
        // MLT's SDL consumer scales pictures nearest-neighbour unless told otherwise, which made
        // takes (1376×768 from H3) shimmer and crawl when shrunk for the viewer during playback;
        // a seek decodes bilinear. Play the way a seek shows.
        output->set("rescale","bilinear");
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

QString Editor::writeExcerpt(double startSeconds, double seconds, const QString& path) const {
    if (!project) return "No project is open.";
    constexpr int rate = 48000, channels = 2;
    const qint64 total = qint64(std::llround(seconds * rate));
    if (total <= 0) return "The excerpt is empty.";
    QVector<qint16> pcm(total * channels, 0);
    Mlt::Profile p("atsc_720p_24");
    p.set_frame_rate(project->fpsNum, project->fpsDen);
    p.set_explicit(1);
    Mlt::Producer song(p, "loader", project->file(project->master).toUtf8());
    if (!song.is_valid()) return "Cannot read the song.";
    song.set("video_index", -1);
    song.set("audio_index", project->master.stream);
    const double fps = double(project->fpsNum) / project->fpsDen;
    // The first song frame the excerpt reaches, and how far into it the excerpt begins.
    const int first = qMax(0, int(std::floor(startSeconds * fps)));
    qint64 written = startSeconds < 0 ? qint64(std::llround(-startSeconds * rate)) : 0;
    qint64 skip = startSeconds < 0 ? 0 : qint64(std::llround((startSeconds - first / fps) * rate));
    song.seek(first);
    for (int pos = first; written < total && pos < song.get_length(); ++pos) {
        std::unique_ptr<Mlt::Frame> frame(song.get_frame());
        if (!frame) break;
        auto format = mlt_audio_s16;
        int frequency = rate, ch = channels;
        int samples = mlt_audio_calculate_frame_samples(fps, rate, pos);
        auto data = static_cast<const qint16*>(frame->get_audio(format, frequency, ch, samples));
        if (!data || format != mlt_audio_s16 || ch != channels) return "Cannot decode the song.";
        for (int i = 0; i < samples && written < total; ++i) {
            if (skip > 0) { --skip; continue; }
            pcm[written * channels] = data[i * channels];
            pcm[written * channels + 1] = data[i * channels + 1];
            ++written;
        }
    }
    QFile out(path);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return "Cannot write the song excerpt.";
    auto le32 = [](quint32 v) { return QByteArray(reinterpret_cast<const char*>(&v), 4); };
    auto le16 = [](quint16 v) { return QByteArray(reinterpret_cast<const char*>(&v), 2); };
    const quint32 bytes = quint32(pcm.size() * 2);
    QByteArray header = "RIFF" + le32(36 + bytes) + "WAVEfmt " + le32(16) + le16(1) + le16(channels) + le32(rate)
                        + le32(rate * channels * 2) + le16(channels * 2) + le16(16) + "data" + le32(bytes);
    out.write(header);
    out.write(reinterpret_cast<const char*>(pcm.constData()), bytes);
    return {};
}

QVariantMap Editor::probeVideo(const QString& path, const QString& thumb) {
    Mlt::Profile p;
    Mlt::Producer video(p, "loader", path.toUtf8());
    if (!video.is_valid()) return {};
    p.from_producer(video);
    Mlt::Producer sized(p, "loader", path.toUtf8());
    if (!sized.is_valid()) return {};
    const int frames = sized.get_length();
    const double fps = p.fps();
    QVariantMap out{{"width", p.width()}, {"height", p.height()}, {"frames", frames}, {"fps", fps}};
    if (!thumb.isEmpty() && frames > 0) {
        sized.seek(frames / 2);
        std::unique_ptr<Mlt::Frame> frame(sized.get_frame());
        const int w = 320, h = qMax(2, int(std::lround(320.0 * p.height() / qMax(1, p.width()))));
        if (frame) {
            const QImage still = decode(*frame, QSize(w, h));
            if (!still.isNull()) still.save(thumb, "JPEG", 85);
        }
    }
    return out;
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
