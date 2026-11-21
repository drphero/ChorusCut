#include "project.h"
#include <QFileInfo>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>
#include <cmath>
#include <QSize>
#include <algorithm>
#include <limits>
extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/display.h>
}

namespace {
bool fail(QString& error, const QString& message) {
    error = message;
    return false;
}

bool probeSource(Project& project, Media& media, bool audio, QString& error) {
    const QString path = project.file(media);
    auto reject = [&](const QString& reason) { return fail(error, media.path + " — " + reason); };
    if (!QFileInfo(path).isFile()) return reject("File is missing");
    AVFormatContext* raw = nullptr;
    if (avformat_open_input(&raw, QFile::encodeName(path), nullptr, nullptr) < 0)
        return reject("Cannot read media");
    auto close = [](AVFormatContext* ctx) { avformat_close_input(&ctx); };
    std::unique_ptr<AVFormatContext, decltype(close)> input(raw, close);
    if (avformat_find_stream_info(raw, nullptr) < 0) return reject("Cannot read streams");
    media.stream = av_find_best_stream(raw, audio ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO,
                                       -1, -1, nullptr, 0);
    if (media.stream < 0) return reject(audio ? "No audio stream" : "No video stream");
    const auto* stream = raw->streams[media.stream];
    const auto* codec = stream->codecpar;
    if (audio) {
        media.channels = codec->ch_layout.nb_channels;
        media.sampleRate = codec->sample_rate;
        media.bits = codec->bits_per_raw_sample ? codec->bits_per_raw_sample : codec->bits_per_coded_sample;
        if (media.channels < 1 || media.channels > 2 || media.sampleRate <= 0)
            return reject("Choose a mono or stereo master recording");
    } else {
        if (stream->disposition & AV_DISPOSITION_ATTACHED_PIC) return reject("Choose moving footage");
        auto rate = av_guess_frame_rate(raw, raw->streams[media.stream], nullptr);
        if (rate.num <= 0 || rate.den <= 0) return reject("Unknown frame rate");
        int n, d;
        av_reduce(&n, &d, rate.num, rate.den, INT_MAX);
        if (d != 1 && !(d == 1001 && n % 1000 == 0))
            return reject("Frame rate cannot be represented in Final Cut Pro 7 XML");
        if (stream->avg_frame_rate.num > 0 &&
            std::abs(av_q2d(stream->avg_frame_rate) - av_q2d(rate)) > 0.02)
            return reject("Use constant-frame-rate footage");
        if (project.fpsNum && (project.fpsNum != n || project.fpsDen != d))
            return reject("Use footage at the same frame rate as the performance");
        project.fpsNum = n;
        project.fpsDen = d;
        const auto aspect = av_guess_sample_aspect_ratio(raw, raw->streams[media.stream], nullptr);
        if ((aspect.num && av_cmp_q(aspect, {1,1}) != 0) ||
            (codec->field_order != AV_FIELD_UNKNOWN && codec->field_order != AV_FIELD_PROGRESSIVE))
            return reject("Use progressive footage with square pixels");
        const auto* matrix = av_packet_side_data_get(codec->coded_side_data, codec->nb_coded_side_data,
                                                     AV_PKT_DATA_DISPLAYMATRIX);
        if (matrix && std::abs(av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix->data))) > 0.01)
            return reject("Apply the footage rotation before importing");
        media.width = codec->width;
        media.height = codec->height;
        if (media.width <= 0 || media.height <= 0) return reject("Unknown picture size");
    }
    const AVRational frameTime{project.fpsDen, project.fpsNum};
    int64_t frames = audio ? 0 : stream->nb_frames;
    if (frames <= 0 && stream->duration > 0)
        frames = av_rescale_q(stream->duration, stream->time_base, frameTime);
    if (frames <= 0 && raw->nb_streams == 1 && raw->duration > 0)
        frames = av_rescale_q(raw->duration, AV_TIME_BASE_Q, frameTime);
    if (frames <= 0 || frames > std::numeric_limits<int>::max()/2)
        return reject("No supported finite duration");
    media.frames = int(frames);
    return true;
}
}

Project::~Project() {
    if (db.isValid()) {
        const QString name = db.connectionName();
        db.close();
        db = QSqlDatabase();
        QSqlDatabase::removeDatabase(name);
    }
}

bool Project::connect(QString& error) {
    db = QSqlDatabase::addDatabase("QSQLITE", QUuid::createUuid().toString());
    db.setDatabaseName(dir.filePath("project.sqlite"));
    return db.open() || fail(error, db.lastError().text());
}

std::unique_ptr<Mlt::Profile> Project::previewProfile() const {
    auto profile = std::make_unique<Mlt::Profile>("atsc_720p_24");
    // The picture's shape at up to 1920 × 1080: a Retina or full-screen viewer needs at least the
    // sources' own pixels (takes are around 1376 × 768), and the viewer scales down smoothly.
    QSize size(performance.width, performance.height);
    if (size.isEmpty()) size = QSize(1920,1080);
    const QSize aspect = size;
    size.scale(1920,1080,Qt::KeepAspectRatio);
    profile->set_width(qMax(2,size.width() & ~1));
    profile->set_height(qMax(2,size.height() & ~1));
    profile->set_display_aspect(aspect.width(), aspect.height());
    profile->set_sample_aspect(1,1);
    profile->set_frame_rate(fpsNum, fpsDen);
    profile->set_explicit(1);
    return profile;
}

bool Project::probe(QString& error) {
    if (performance.path.isEmpty() != cutaway.path.isEmpty())
        return fail(error, "Select both picture sources or leave both empty.");
    if (performance.path.isEmpty()) fpsNum = 24;
    else if (!probeSource(*this, performance, false, error) || !probeSource(*this, cutaway, false, error))
        return false;
    if (!probeSource(*this, master, true, error)) return false;
    auto profile = previewProfile();
    for (const Media* item : {&performance, &cutaway, &master}) {
        if (item->path.isEmpty()) continue;
        Mlt::Producer clip(*profile, "loader", file(*item).toUtf8());
        if (!clip.is_valid() || clip.get_length() < item->frames)
            return fail(error, "Cannot preview the full source: " + file(*item));
    }
    return true;
}

bool Project::writeSchema(bool legacy, QString& error) {
    if (!db.transaction()) return fail(error, db.lastError().text());
    QSqlQuery q(db);
    auto rollback = [&] {
        error = q.lastError().text();
        db.rollback();
        return false;
    };
    if (!q.exec("CREATE TABLE media(role TEXT PRIMARY KEY, path TEXT NOT NULL)") ||
        !q.prepare("INSERT INTO media VALUES(?,?)")) return rollback();
    auto insert = [&](const QString& role, const Media& item) {
        if (item.path.isEmpty()) return true;
        q.bindValue(0, role);
        q.bindValue(1, item.path);
        return q.exec();
    };
    if (!insert("master", master) || !insert("performance", performance) || !insert("cutaway", cutaway))
        return rollback();
    if (!legacy) {
        if (!q.exec("CREATE TABLE cutaway(id INTEGER PRIMARY KEY CHECK(id=1), start INTEGER NOT NULL, "
                    "end INTEGER NOT NULL, source INTEGER NOT NULL)") ||
            !q.prepare("INSERT INTO cutaway VALUES(1,?,?,?)")) return rollback();
        q.addBindValue(trim.start);
        q.addBindValue(trim.end);
        q.addBindValue(trim.source);
        if (!q.exec()) return rollback();
    }
    if (!q.exec("PRAGMA user_version=1")) return rollback();
    return db.commit() || fail(error, db.lastError().text());
}

std::unique_ptr<Project> Project::open(const QString& directory, QString& error) {
    auto p = std::make_unique<Project>();
    p->dir = QDir(QFileInfo(directory).absoluteFilePath());
    if (!p->dir.exists()) { error = "Project folder is missing"; return {}; }
    const bool existing = QFileInfo::exists(p->dir.filePath("project.sqlite"));
    auto examplePaths = [&] {
        p->performance.path = "Media/Performance.mp4";
        p->cutaway.path = "Media/Cutaway.mp4";
        p->master.path = "Media/Master.wav";
    };
    if (!existing) {
        examplePaths();
        if (!p->probe(error)) { error = "Choose a Chorus Cut project folder"; return {}; }
        p->trim = {72, 120, 24};
        if (p->master.frames < p->trim.end || p->cutaway.frames < p->trim.source + p->trim.end - p->trim.start) {
            error = "The files do not fit the example cut. Use New to import them."; return {};
        }
        if (!p->connect(error) || !p->writeSchema(false, error)) return {};
        return p;
    }
    if (!p->connect(error)) return {};
    QSqlQuery q(p->db);
    if (!q.exec("PRAGMA user_version") || !q.next()) { error = q.lastError().text(); return {}; }
    int version = q.value(0).toInt();
    q.finish();
    if (version == 0) {
        if (!p->db.tables().contains("cutaway")) { error = "Unrecognized project database"; return {}; }
        examplePaths();
        if (!p->probe(error)) return {};
    } else if (version == 1) {
        if (!q.exec("SELECT role,path FROM media")) { error = q.lastError().text(); return {}; }
        while (q.next()) {
            const QString role = q.value(0).toString(), path = q.value(1).toString();
            if (!path.startsWith("Media/") || QDir::cleanPath(path) != path) {
                error = "Invalid project media path"; return {};
            }
            if (role == "master") p->master.path = path;
            else if (role == "performance") p->performance.path = path;
            else if (role == "cutaway") p->cutaway.path = path;
        }
        q.finish();
        if (!p->probe(error)) return {};
    } else { error = "Unsupported project version"; return {}; }
    if (!q.exec("SELECT start,end,source FROM cutaway WHERE id=1") || !q.next()) {
        error = "Cannot read saved cutaway"; return {};
    }
    p->trim = {q.value(0).toInt(), q.value(1).toInt(), q.value(2).toInt()};
    q.finish();
    const Trim& t = p->trim;
    if (!p->cutaway.path.isEmpty() && (t.start < 0 || t.end <= t.start || t.end > p->master.frames || t.source < 0 ||
        t.source > p->cutaway.frames - (t.end-t.start))) { error = "Invalid saved cutaway range"; return {}; }
    if (version == 0 && !p->writeSchema(true, error)) return {};
    return p;
}

bool Project::create(const QString& directory, const QString& song, const QString& base,
                     const QString& insert, QString& error) {
    QFileInfo target(directory);
    if (target.exists() || target.isSymLink()) return fail(error, "Choose a new project folder name");
    QDir parent = target.absoluteDir();
    QTemporaryDir stage(parent.filePath(".chorus-import-XXXXXX"));
    if (!stage.isValid()) return fail(error, stage.errorString());
    {
        Project p;
        p.dir = QDir(stage.path());
        auto copy = [&](const QString& role, const QString& source, Media& media) {
            const QString folder = "Media/" + role;
            media.path = folder + "/" + QFileInfo(source).fileName();
            QFile file(source);
            if (!QFileInfo(source).isFile() || !p.dir.mkpath(folder) || !file.copy(p.file(media)))
                return fail(error, "Could not copy " + source + " into the project");
            return true;
        };
        if (!copy("Master",song,p.master)) return false;
        if (base.isEmpty() != insert.isEmpty()) return fail(error, "Choose both picture sources or leave both empty");
        if (!base.isEmpty() && (!copy("Performance",base,p.performance) || !copy("Cutaway",insert,p.cutaway)))
            return false;
        if (!p.probe(error)) return false;
        int start = p.master.frames / 3;
        int duration = std::min({qMax(1,qRound(2.0*p.fpsNum/p.fpsDen)), p.cutaway.frames, p.master.frames-start});
        p.trim = duration ? Trim{start,start+duration,0} : Trim{0,0,0};
        if (!p.connect(error) || !p.writeSchema(false,error)) return false;
    }
    if (!parent.rename(stage.path(), target.absoluteFilePath()))
        return fail(error,"Could not create project folder; check its name and permissions");
    stage.setAutoRemove(false);
    return true;
}
