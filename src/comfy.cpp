#include "comfy.h"
#include "editor.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QSettings>
#include <QSqlError>
#include <QSqlQuery>
#include <QRegularExpression>
#include <QUuid>

namespace {
constexpr int kPollMs = 1500;
constexpr char kNode[] = "ChorusCutAlignLyrics";

QString mmss(qint64 ms) {
    const qint64 s = ms / 1000;
    return QString("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QChar('0'));
}
}

ComfyJobs::ComfyJobs(Editor* editor) : QObject(editor), editor(editor) {
    QSettings settings;
    server = settings.value("comfyui/url", "http://127.0.0.1:8188").toString();
    token = settings.value("comfyui/token").toString();
    poll.setInterval(kPollMs);
    connect(&poll, &QTimer::timeout, this, &ComfyJobs::check);
    net.setTransferTimeout(30000);
}

QNetworkRequest ComfyJobs::request(const QString& path) const {
    QUrl base(server.endsWith('/') ? server.chopped(1) : server);
    QNetworkRequest r(QUrl(base.toString() + path));
    if (!token.isEmpty()) r.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    r.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::NoLessSafeRedirectPolicy);
    return r;
}

QString ComfyJobs::errorOf(QNetworkReply* reply) {
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (code == 401 || code == 403) return "ComfyUI refused the request: check the access token";
    if (code) return QString("ComfyUI answered HTTP %1").arg(code);
    return reply->errorString();
}

void ComfyJobs::setConnection(const QString& value, const QString& text) {
    state = value;
    stateText = text;
    emit connectionChanged();
}

void ComfyJobs::say(const QString& text) {
    status = text;
    emit jobChanged();
}

void ComfyJobs::configure(const QString& url, const QString& newToken, bool clearToken) {
    QString clean = url.trimmed();
    if (!clean.isEmpty() && !clean.contains("://")) clean = "http://" + clean;
    server = clean;
    if (clearToken) token.clear();
    else if (!newToken.trimmed().isEmpty()) token = newToken.trimmed();
    QSettings settings;
    settings.setValue("comfyui/url", server);
    if (token.isEmpty()) settings.remove("comfyui/token");
    else settings.setValue("comfyui/token", token);
    emit settingsChanged();
    setConnection("unknown", {});
}

void ComfyJobs::testConnection() {
    const QUrl url(server);
    if (!url.isValid() || url.host().isEmpty()) { setConnection("unreachable", "Enter the ComfyUI address, for example http://127.0.0.1:8188"); return; }
    setConnection("checking", "Connecting…");
    const int mine = ++probe;
    auto stats = net.get(request("/system_stats"));
    connect(stats, &QNetworkReply::finished, this, [this, stats, mine] {
        stats->deleteLater();
        if (mine != probe) return;
        if (stats->error() != QNetworkReply::NoError) { setConnection("unreachable", "Can't reach ComfyUI: " + errorOf(stats)); return; }
        const auto info = QJsonDocument::fromJson(stats->readAll()).object();
        const QString version = info["system"].toObject()["comfyui_version"].toString();
        const auto devices = info["devices"].toArray();
        // ComfyUI names devices like "cuda:0 NVIDIA GeForce RTX 3090 : cudaMallocAsync".
        QString device = devices.isEmpty() ? QString() : devices[0].toObject()["name"].toString().section(" : ", 0, 0);
        device = device.remove(QRegularExpression("^\\w+:\\d+\\s*")).trimmed();
        auto node = net.get(request(QString("/object_info/") + kNode));
        connect(node, &QNetworkReply::finished, this, [this, node, version, device, mine] {
            node->deleteLater();
            if (mine != probe) return;
            const auto info = QJsonDocument::fromJson(node->readAll()).object();
            const QString where = QString("ComfyUI %1 on %2").arg(version.isEmpty() ? "" : version, device.isEmpty() ? "an unknown device" : device).simplified();
            if (node->error() != QNetworkReply::NoError || !info.contains(kNode))
                setConnection("missingNode", where + ". Install the Chorus Cut node pack there to align lyrics.");
            else setConnection("ready", where + ", with the Chorus Cut nodes.");
        });
    });
}

bool ComfyJobs::ensureTable() {
    auto project = editor->current();
    if (!project) return false;
    QSqlQuery q(project->db);
    return q.exec("CREATE TABLE IF NOT EXISTS jobs(prompt_id TEXT PRIMARY KEY, kind TEXT NOT NULL, server TEXT NOT NULL, "
                  "language TEXT NOT NULL, lyrics TEXT NOT NULL, submitted INTEGER NOT NULL, state TEXT NOT NULL, detail TEXT)");
}

void ComfyJobs::record(const QString& value, const QString& detail) {
    auto project = editor->current();
    if (!project || promptId.isEmpty()) return;
    QSqlQuery q(project->db);
    q.prepare("UPDATE jobs SET state=?, detail=? WHERE prompt_id=?");
    q.addBindValue(value);
    q.addBindValue(detail.isNull() ? QString("") : detail);
    q.addBindValue(promptId);
    q.exec();
}

void ComfyJobs::alignLyrics(const QString& language) {
    auto project = editor->current();
    if (!project || busy()) return;
    const QString text = editor->lyrics()->text();
    if (text.trimmed().isEmpty()) { say("Paste the lyrics first."); emit finished(false); return; }
    if (!ensureTable()) { say("Could not record the job in the project."); emit finished(false); return; }
    lastLanguage = language;
    jobLyrics = text;
    uploading = true;
    clock.start();
    say("Uploading the song to ComfyUI…");
    // The upload is named by content, so re-sending the same song replaces one file.
    QFile song(project->file(project->master));
    if (!song.open(QIODevice::ReadOnly)) { uploading = false; fail("Cannot read the song file."); return; }
    const QByteArray bytes = song.readAll();
    const QString name = "chorus-" + QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(16) +
                         "." + QFileInfo(song.fileName()).suffix().toLower();
    auto form = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart file;
    file.setHeader(QNetworkRequest::ContentDispositionHeader, QString("form-data; name=\"image\"; filename=\"%1\"").arg(name));
    file.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    file.setBody(bytes);
    QHttpPart overwrite;
    overwrite.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"overwrite\"");
    overwrite.setBody("true");
    form->append(file);
    form->append(overwrite);
    const int mine = ++serial;
    auto reply = net.post(request("/upload/image"), form);
    form->setParent(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, mine, language] {
        reply->deleteLater();
        if (mine != serial) return;
        uploading = false;
        if (reply->error() != QNetworkReply::NoError) { fail("Upload failed: " + errorOf(reply)); return; }
        const auto info = QJsonDocument::fromJson(reply->readAll()).object();
        const QString sub = info["subfolder"].toString();
        submit(sub.isEmpty() ? info["name"].toString() : sub + "/" + info["name"].toString(), language);
    });
    emit jobChanged();
}

void ComfyJobs::submit(const QString& uploaded, const QString& language) {
    QJsonObject graph{
        {"1", QJsonObject{{"class_type", "LoadAudio"}, {"inputs", QJsonObject{{"audio", uploaded}}}}},
        {"2", QJsonObject{{"class_type", kNode},
                          {"inputs", QJsonObject{{"audio", QJsonArray{"1", 0}}, {"lyrics", jobLyrics}, {"language", language}}}}}};
    QJsonObject body{{"prompt", graph}, {"client_id", "chorus-cut-" + QUuid::createUuid().toString(QUuid::Id128)}};
    auto r = request("/prompt");
    r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    const int mine = ++serial;
    say("Queueing the alignment…");
    auto reply = net.post(r, QJsonDocument(body).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, mine, language] {
        reply->deleteLater();
        if (mine != serial) return;
        const auto answer = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || !answer.contains("prompt_id")) {
            QString why = answer["error"].toObject()["message"].toString();
            for (const auto& n : answer["node_errors"].toObject())
                for (const auto& e : n.toObject()["errors"].toArray())
                    why += " " + e.toObject()["message"].toString() + ": " + e.toObject()["details"].toString();
            fail("ComfyUI did not accept the job: " + (why.trimmed().isEmpty() ? errorOf(reply) : why.trimmed()));
            return;
        }
        promptId = answer["prompt_id"].toString();
        QSqlQuery q(editor->current()->db);
        q.prepare("INSERT OR REPLACE INTO jobs(prompt_id,kind,server,language,lyrics,submitted,state,detail) VALUES(?,?,?,?,?,?,?,?)");
        for (const QVariant& v : {QVariant(promptId), QVariant("align"), QVariant(server), QVariant(language), QVariant(jobLyrics),
                                  QVariant(QDateTime::currentSecsSinceEpoch()), QVariant("queued"), QVariant("")})
            q.addBindValue(v);
        if (!q.exec()) { fail("Could not record the job: " + q.lastError().text()); return; }
        misses = 0;
        say("Queued on ComfyUI.");
        poll.start();
    });
}

void ComfyJobs::check() {
    if (promptId.isEmpty()) { poll.stop(); return; }
    const int mine = serial;
    const QString id = promptId;
    auto history = net.get(request("/history/" + id));
    connect(history, &QNetworkReply::finished, this, [this, history, mine, id] {
        history->deleteLater();
        if (mine != serial || id != promptId) return;
        if (history->error() != QNetworkReply::NoError) {
            ++misses;
            say(QString("Can't reach ComfyUI (%1). The job is saved; retrying…").arg(errorOf(history)));
            if (misses == 3) poll.setInterval(10000);
            return;
        }
        if (misses) { misses = 0; poll.setInterval(kPollMs); }
        const auto entry = QJsonDocument::fromJson(history->readAll()).object()[id].toObject();
        if (!entry.isEmpty()) {
            const auto st = entry["status"].toObject();
            if (st["status_str"].toString() == "error") {
                QString why = "the job failed";
                for (const auto& m : st["messages"].toArray()) {
                    const auto pair = m.toArray();
                    if (pair[0].toString() == "execution_error") why = pair[1].toObject()["exception_message"].toString().trimmed();
                    if (pair[0].toString() == "execution_interrupted") why = "the job was interrupted";
                }
                fail("Alignment failed on ComfyUI: " + why);
                return;
            }
            for (const auto& out : entry["outputs"].toObject()) {
                const auto texts = out.toObject()["chorus_cut_alignment"].toArray();
                if (!texts.isEmpty()) { complete(texts[0].toString().toUtf8()); return; }
            }
            if (st["completed"].toBool()) { fail("ComfyUI finished without an alignment."); return; }
        }
        // Not finished: report the queue position.
        auto queue = net.get(request("/queue"));
        connect(queue, &QNetworkReply::finished, this, [this, queue, mine, id] {
            queue->deleteLater();
            if (mine != serial || id != promptId || queue->error() != QNetworkReply::NoError) return;
            const auto q = QJsonDocument::fromJson(queue->readAll()).object();
            auto has = [&](const QJsonArray& list) {
                for (const auto& item : list) if (item.toArray()[1].toString() == id) return true;
                return false;
            };
            const auto running = q["queue_running"].toArray(), pending = q["queue_pending"].toArray();
            if (has(running)) {
                record("running");
                // A first run on a fresh GPU spends minutes downloading the models.
                say(QString("Aligning on ComfyUI… %1%2").arg(mmss(clock.elapsed()),
                    clock.elapsed() > 90000 ? " (a first run on a new GPU also downloads about 5.5 GB of models)" : ""));
            } else if (has(pending)) {
                int ahead = running.size();
                for (const auto& item : pending) if (item.toArray()[1].toString() != id) ++ahead; else break;
                say(QString("Waiting in the ComfyUI queue (%1 ahead)…").arg(ahead));
            } else {
                // Gone from both queue and history: ComfyUI restarted or the history was cleared.
                fail("ComfyUI no longer has this job (it may have restarted). Align again.");
            }
        });
    });
}

void ComfyJobs::complete(const QByteArray& alignment) {
    poll.stop();
    auto project = editor->current();
    const QString id = promptId;
    const QString folder = project->dir.filePath("Jobs");
    QDir().mkpath(folder);
    const QString path = QDir(folder).filePath("alignment-" + id + ".json");
    QFile out(path);
    const bool saved = out.open(QIODevice::WriteOnly) && out.write(alignment) == alignment.size();
    out.close();
    record("done", saved ? QDir(project->dir).relativeFilePath(path) : QString());
    promptId.clear();
    auto lyrics = editor->lyrics();
    bool ok = false;
    if (!saved) say("The alignment arrived but could not be saved in the project.");
    else if (lyrics->text() != jobLyrics) say("The lyrics changed while aligning, so the result was saved in Jobs/ but not applied. Align again.");
    else {
        ok = lyrics->importAlignment(QUrl::fromLocalFile(path));
        say(ok ? lyrics->message() + QString(" Aligned on ComfyUI in %1.").arg(mmss(clock.elapsed())) : lyrics->message());
    }
    emit finished(ok);
}

void ComfyJobs::fail(const QString& why) {
    poll.stop();
    record("failed", why);
    promptId.clear();
    uploading = false;
    say(why);
    emit finished(false);
}

void ComfyJobs::cancel() {
    if (!busy()) return;
    if (!promptId.isEmpty()) {
        // Remove it from the queue if it is waiting, and interrupt it if it is running.
        auto r = request("/queue");
        r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        auto i = request("/interrupt");
        i.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        // Delete each reply once it finishes; deleting it earlier would abort the request.
        for (auto reply : {net.post(r, QJsonDocument(QJsonObject{{"delete", QJsonArray{promptId}}}).toJson()),
                           net.post(i, QJsonDocument(QJsonObject{{"prompt_id", promptId}}).toJson())})
            connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
    }
    ++serial;
    record("cancelled");
    poll.stop();
    promptId.clear();
    uploading = false;
    say("Alignment cancelled.");
    emit finished(false);
}

void ComfyJobs::forget() {
    ++serial;
    poll.stop();
    promptId.clear();
    jobLyrics.clear();
    uploading = false;
    misses = 0;
}

void ComfyJobs::resume() {
    forget();
    status.clear();
    auto project = editor->current();
    if (!project) { emit jobChanged(); return; }
    QSqlQuery q(project->db);
    if (q.exec("SELECT language FROM jobs ORDER BY submitted DESC LIMIT 1") && q.next()) lastLanguage = q.value(0).toString();
    if (q.exec("SELECT prompt_id,server,lyrics,submitted FROM jobs WHERE kind='align' AND state IN ('queued','running') "
               "ORDER BY submitted DESC LIMIT 1") && q.next()) {
        if (q.value(1).toString() != server) {
            say("An alignment was started on " + q.value(1).toString() + ", which is not the current ComfyUI. Align again here.");
        } else {
            promptId = q.value(0).toString();
            jobLyrics = q.value(2).toString();
            clock.start();
            say("Reconnecting to the alignment on ComfyUI…");
            poll.start();
            check();
        }
    }
    emit jobChanged();
}

void ComfyJobs::detach() {
    forget();
    status.clear();
    emit jobChanged();
}
