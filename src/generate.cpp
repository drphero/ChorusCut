#include "generate.h"
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
#include <QRandomGenerator>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrlQuery>
#include <QUuid>
#include <cmath>
#include <memory>

namespace {
constexpr int kPollMs = 1500;
QString ownerNoun(const QString& owner) { return owner == "look" ? "look" : owner == "location" ? "location" : "character"; }
int round16(double v) { return std::max(16, int(std::lround(v / 16.0)) * 16); }
}

Generator::Generator(Editor* editor) : QObject(editor), editor(editor) {
    clientId = "chorus-cut-" + QUuid::createUuid().toString(QUuid::Id128);
    poll.setInterval(kPollMs);
    connect(&poll, &QTimer::timeout, this, &Generator::check);
    net.setTransferTimeout(60000);
    // A workflow fixed or added in the library changes what can be generated.
    connect(editor->workflows(), &Workflows::changed, this, &Generator::changed);
    connect(editor->plan(), &Plan::changed, this, &Generator::changed);
}

QVariantList Generator::jobs() const {
    QVariantList out;
    for (const auto& j : list)
        out.append(QVariantMap{{"id", j.id}, {"owner", j.owner}, {"ownerId", j.ownerId}, {"use", j.use},
                               {"workflow", j.workflowName}, {"state", j.state}, {"text", j.text}});
    return out;
}

Generator::Job* Generator::find(int id) {
    for (auto& j : list) if (j.id == id) return &j;
    return nullptr;
}

void Generator::say(Job& job, const QString& state, const QString& text) {
    if (job.state == state && job.text == text) return;
    job.state = state;
    job.text = text;
    emit changed();
}

QStringList Generator::picturesFor(const QString& owner, int ownerId) const {
    // Approved pictures, in order: a look's own, then its character's (the face first).
    QStringList out;
    auto plan = editor->plan();
    auto project = editor->current();
    if (!project) return out;
    auto add = [&](const QString& kind, int id) {
        for (const auto& v : plan->referencesFor(kind, id)) {
            const auto r = v.toMap();
            if (r["approved"].toBool() && !r["missing"].toBool()) out << project->dir.filePath(r["file"].toString());
        }
    };
    add(owner, ownerId);
    if (owner == "look") add("character", plan->item("look", ownerId)["character"].toInt());
    return out;
}

QString Generator::whyNot(const QString& owner, int ownerId, const QString& use, const QString& workflow) const {
    auto flows = editor->workflows();
    const auto item = editor->plan()->item(owner, ownerId);
    if (item.isEmpty()) return "Nothing to generate for.";
    if (workflow.isEmpty())
        return use == "ref-text" ? "Set up a workflow for reference pictures from a prompt." : "Set up a workflow for reference pictures from pictures.";
    bool ready = false;
    for (const auto& w : flows->ready(use)) ready |= w.toMap()["id"] == workflow;
    if (!ready) return flows->nameOf(workflow) + " is not ready for this kind of picture. Check it in Workflows.";
    if (item["fields"].toMap()["prompt"].toString().trimmed().isEmpty()) return "Write a picture prompt first, or ask your agent to write one.";
    if (use == "ref-edit") {
        const int need = flows->pictureCount(workflow), have = picturesFor(owner, ownerId).size();
        if (have < need)
            return QString("%1 takes %2 approved %3; this %4 has %5.").arg(flows->nameOf(workflow)).arg(need)
                .arg(need == 1 ? "picture" : "pictures").arg(ownerNoun(owner)).arg(have);
    }
    return {};
}

QVariantMap Generator::options(const QString& owner, int ownerId) const {
    auto flows = editor->workflows();
    const QString prompt = editor->plan()->item(owner, ownerId)["fields"].toMap()["prompt"].toString().trimmed();
    auto one = [&](const QString& use) {
        const QString w = flows->forUse(use);
        QVariantMap m{{"workflow", w.isEmpty() ? QString() : flows->nameOf(w)}, {"id", w}, {"why", whyNot(owner, ownerId, use, w)},
                      {"workflows", flows->ready(use)}};
        if (use == "ref-edit") m["pictures"] = w.isEmpty() ? 0 : flows->pictureCount(w);
        m["ready"] = m["why"].toString().isEmpty();
        return m;
    };
    return {{"prompt", prompt}, {"text", one("ref-text")}, {"edit", one("ref-edit")}};
}

QString Generator::generate(const QString& owner, int ownerId, const QString& use) {
    return generateWith(owner, ownerId, use, {});
}

QString Generator::generateWith(const QString& owner, int ownerId, const QString& use, const QVariantMap& o) {
    if (!editor->current()) return "Open a project first.";
    if (use != "ref-text" && use != "ref-edit") return "Unknown kind of picture.";
    auto flows = editor->workflows();
    const QString workflow = o.value("workflow").toString().isEmpty() ? flows->forUse(use) : o.value("workflow").toString();
    const QString why = whyNot(owner, ownerId, use, workflow);
    if (!why.isEmpty()) return why;
    // A seed fits in a double exactly below 2^53, and every sampler takes it.
    constexpr qint64 kMaxSeed = (qint64(1) << 53) - 1;
    qint64 seed = -1;
    const QVariant given = o.value("seed");
    if (given.isValid() && !given.isNull() && given.toString().trimmed() != "") {
        bool ok = false;
        if (given.typeId() == QMetaType::Double || given.typeId() == QMetaType::Float) {
            const double d = given.toDouble();
            ok = std::isfinite(d) && d == std::floor(d) && d >= 0 && d <= double(kMaxSeed);
            seed = ok ? qint64(d) : -1;
        } else {
            seed = given.toString().trimmed().toLongLong(&ok);
        }
        if (!ok || seed < 0 || seed > kMaxSeed) return "The seed must be a whole number from 0 to " + QString::number(kMaxSeed) + ".";
    }
    const int count = o.contains("count") ? o.value("count").toInt() : 1;
    if (count < 1 || count > 8) return "Generate 1 to 8 pictures at a time.";
    QString error;
    const QJsonObject chosen = flows->checkValues(workflow, o.value("settings").toMap(), error);
    if (!error.isEmpty()) return error;
    if (!ensureTable()) return "Could not record the job in the project.";
    if (!chosen.isEmpty()) { error = flows->setValues(workflow, chosen); if (!error.isEmpty()) return error; }
    // What the pictures were made with, as the reference will show it.
    QJsonArray shown;
    QJsonObject used;
    for (const auto& v : flows->settings(workflow)) {
        const auto s = v.toMap();
        const QJsonValue value = chosen.contains(s["key"].toString()) ? chosen[s["key"].toString()] : QJsonValue::fromVariant(s["value"]);
        used[s["key"].toString()] = value;
        shown.append(QJsonObject{{"key", s["key"].toString()}, {"label", s["label"].toString()}, {"value", value}});
    }
    const QString prompt = editor->plan()->item(owner, ownerId)["fields"].toMap()["prompt"].toString().trimmed();
    const QStringList files = use == "ref-edit" ? picturesFor(owner, ownerId).mid(0, flows->pictureCount(workflow)) : QStringList{};
    const qint64 first = seed >= 0 ? seed : qint64(QRandomGenerator::global()->bounded(quint64(1) << 48));
    for (int i = 0; i < count; ++i) {
        Job job;
        job.id = ++serial;
        job.owner = owner;
        job.ownerId = ownerId;
        job.use = use;
        job.workflow = workflow;
        job.workflowName = flows->nameOf(workflow);
        job.prompt = prompt;
        job.hasSeed = flows->hasSlot(workflow, "seed");
        // Fixed: this seed and the ones after it. Random: a fresh one for each picture.
        job.seed = seed >= 0 ? std::min(kMaxSeed, first + i) : i == 0 ? first : qint64(QRandomGenerator::global()->bounded(quint64(1) << 48));
        job.settings = used;
        job.shown = shown;
        list.append(job);
        say(list.last(), "uploading", files.isEmpty() ? "Preparing…" : "Uploading pictures…");
        upload(job.id, files, {});
    }
    return {};
}

void Generator::fetchChoices(const QString& workflow) {
    // Each node type is asked once per run of the app; ComfyUI lists its samplers, schedulers
    // and models in /object_info.
    QSet<QString> classes;
    for (const auto& v : editor->workflows()->settings(workflow))
        if (v.toMap()["type"] == "text") classes << v.toMap()["classType"].toString();
    for (const QString& cls : classes) {
        const QString key = editor->comfy()->url() + "|" + cls;
        if (asked.contains(key)) continue;
        asked.insert(key);
        auto reply = net.get(editor->comfy()->request("/object_info/" + QString::fromUtf8(QUrl::toPercentEncoding(cls))));
        connect(reply, &QNetworkReply::finished, this, [this, reply, cls, key] {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) { asked.remove(key); return; }
            const auto info = QJsonDocument::fromJson(reply->readAll()).object()[cls].toObject()["input"].toObject();
            bool any = false;
            for (const char* group : {"required", "optional"}) {
                const auto inputs = info[group].toObject();
                for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                    // A combo input is [[choice, ...], {...}]; newer servers write ["COMBO", {options: [...]}].
                    const auto spec = it.value().toArray();
                    QJsonArray values = spec.at(0).toArray();
                    if (values.isEmpty() && spec.at(0).toString() == "COMBO") values = spec.at(1).toObject()["options"].toArray();
                    if (values.isEmpty()) continue;
                    QVariantList choices;
                    for (const auto& c : values) if (c.isString()) choices << c.toString();
                    if (choices.isEmpty()) continue;
                    choiceMap[cls + "/" + it.key()] = choices;
                    any = true;
                }
            }
            if (any) emit choicesChanged();
        });
    }
}

void Generator::upload(int id, QStringList files, QStringList names) {
    if (!find(id)) return;
    if (files.isEmpty()) { submit(id, names); return; }
    const QString path = files.takeFirst();
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) { failJob(id, "Cannot read " + QFileInfo(path).fileName() + "."); return; }
    const QByteArray bytes = file.readAll();
    // Named by content, so the same picture sent twice is stored once on the server.
    const QString name = "chorus-" + QCryptographicHash::hash(bytes, QCryptographicHash::Sha1).toHex().left(16) + "." + QFileInfo(path).suffix().toLower();
    auto form = new QHttpMultiPart(QHttpMultiPart::FormDataType);
    QHttpPart part;
    part.setHeader(QNetworkRequest::ContentDispositionHeader, QString("form-data; name=\"image\"; filename=\"%1\"").arg(name));
    part.setHeader(QNetworkRequest::ContentTypeHeader, "application/octet-stream");
    part.setBody(bytes);
    QHttpPart overwrite;
    overwrite.setHeader(QNetworkRequest::ContentDispositionHeader, "form-data; name=\"overwrite\"");
    overwrite.setBody("true");
    form->append(part);
    form->append(overwrite);
    auto reply = net.post(editor->comfy()->request("/upload/image"), form);
    form->setParent(reply);
    const int mine = epoch;
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, files, names, mine]() mutable {
        reply->deleteLater();
        if (mine != epoch || !find(id)) return;
        if (reply->error() != QNetworkReply::NoError) { failJob(id, "Upload failed: " + ComfyJobs::errorOf(reply)); return; }
        const auto info = QJsonDocument::fromJson(reply->readAll()).object();
        const QString sub = info["subfolder"].toString();
        names << (sub.isEmpty() ? info["name"].toString() : sub + "/" + info["name"].toString());
        upload(id, files, names);
    });
}

void Generator::submit(int id, const QStringList& names) {
    Job* job = find(id);
    if (!job) return;
    auto flows = editor->workflows();
    Workflows::Values v;
    v.prompt = job->prompt;
    v.seed = job->seed;
    v.pictures = names;
    v.settings = job->settings;
    v.prefix = "ChorusCut/reference";
    // Locations take the video's frame; people and looks a portrait. The workflow's own size
    // sets the scale its model prefers.
    const QSize base = flows->size(job->workflow);
    if (base.width() > 0 && base.height() > 0) {
        double ratio = 3.0 / 4.0;
        if (job->owner == "location") {
            const QString aspect = editor->plan()->concept()["fields"].toMap()["aspect"].toString();
            const auto parts = aspect.split(':');
            ratio = parts.size() == 2 && parts[1].toDouble() > 0 ? parts[0].toDouble() / parts[1].toDouble() : 16.0 / 9.0;
        }
        const double longSide = std::max(base.width(), base.height());
        v.width = round16(ratio >= 1 ? longSide : longSide * ratio);
        v.height = round16(ratio >= 1 ? longSide / ratio : longSide);
    }
    QString error;
    const QJsonObject graph = flows->build(job->workflow, v, error);
    if (graph.isEmpty()) { failJob(id, error); return; }
    job->graph = graph;
    say(*job, "queued", "Queueing…");
    auto r = editor->comfy()->request("/prompt");
    r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    auto reply = net.post(r, QJsonDocument(QJsonObject{{"prompt", graph}, {"client_id", clientId}}).toJson(QJsonDocument::Compact));
    const int mine = epoch;
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, mine] {
        reply->deleteLater();
        if (mine != epoch) return;
        Job* job = find(id);
        if (!job) return;
        const auto answer = QJsonDocument::fromJson(reply->readAll()).object();
        if (reply->error() != QNetworkReply::NoError || !answer.contains("prompt_id")) {
            QString why = answer["error"].toObject()["message"].toString();
            for (const auto& n : answer["node_errors"].toObject())
                for (const auto& e : n.toObject()["errors"].toArray())
                    why += " " + e.toObject()["message"].toString() + ": " + e.toObject()["details"].toString();
            failJob(id, "ComfyUI did not accept the job: " + (why.trimmed().isEmpty() ? ComfyJobs::errorOf(reply) : why.trimmed()));
            return;
        }
        job->promptId = answer["prompt_id"].toString();
        auto project = editor->current();
        QSqlQuery q(project->db);
        q.prepare("INSERT OR REPLACE INTO jobs(prompt_id,kind,server,language,lyrics,submitted,state,detail,client_id) VALUES(?,?,?,?,?,?,?,?,?)");
        const QJsonObject detail{{"owner", job->owner}, {"ownerId", job->ownerId}, {"use", job->use}, {"workflow", job->workflow},
                                 {"workflowName", job->workflowName}, {"prompt", job->prompt}, {"seed", double(job->seed)}, {"hasSeed", job->hasSeed},
                                 {"settings", job->shown}, {"graph", job->graph}};
        for (const QVariant& value : {QVariant(job->promptId), QVariant("reference"), QVariant(editor->comfy()->url()), QVariant(""), QVariant(""),
                                      QVariant(QDateTime::currentSecsSinceEpoch()), QVariant("queued"),
                                      QVariant(QString::fromUtf8(QJsonDocument(detail).toJson(QJsonDocument::Compact))), QVariant(clientId)})
            q.addBindValue(value);
        if (!q.exec()) { failJob(id, "Could not record the job: " + q.lastError().text()); return; }
        say(*job, "queued", "Queued on ComfyUI");
        poll.start();
    });
}

void Generator::check() {
    bool active = false;
    for (const auto& j : list) active |= !j.promptId.isEmpty() && (j.state == "queued" || j.state == "running");
    if (!active) { poll.stop(); return; }
    const int mine = epoch;
    auto queue = net.get(editor->comfy()->request("/queue"));
    connect(queue, &QNetworkReply::finished, this, [this, queue, mine] {
        queue->deleteLater();
        if (mine != epoch) return;
        const bool reached = queue->error() == QNetworkReply::NoError;
        const auto q = QJsonDocument::fromJson(queue->readAll()).object();
        const auto running = q["queue_running"].toArray(), pending = q["queue_pending"].toArray();
        QList<int> ids;
        for (const auto& j : list) if (!j.promptId.isEmpty() && (j.state == "queued" || j.state == "running")) ids << j.id;
        for (int id : ids) {
            Job* job = find(id);
            if (!job) continue;
            if (!reached) {
                if (++job->misses >= 3) poll.setInterval(10000);
                say(*job, job->state, "Can't reach ComfyUI (" + ComfyJobs::errorOf(queue) + "). The job is saved; retrying…");
                continue;
            }
            if (job->misses) { job->misses = 0; poll.setInterval(kPollMs); }
            const QString pid = job->promptId;
            int place = -1;
            bool isRunning = false;
            for (const auto& item : running) if (item.toArray()[1].toString() == pid) isRunning = true;
            for (int i = 0; i < pending.size(); ++i) if (pending[i].toArray()[1].toString() == pid) place = i;
            if (isRunning) {
                job->waiting = 0;
                if (job->state != "running") record(*job, "running");
                say(*job, "running", "Generating…");
                continue;
            }
            if (place >= 0) {
                job->waiting = 0;
                say(*job, "queued", place + running.size() ? QString("Waiting in the queue (%1 ahead)").arg(place + running.size()) : QString("Queued on ComfyUI"));
                continue;
            }
            // Neither running nor waiting: finished, failed, or gone from the server.
            auto history = net.get(editor->comfy()->request("/history/" + pid));
            connect(history, &QNetworkReply::finished, this, [this, history, id, pid, mine] {
                history->deleteLater();
                Job* job = find(id);
                if (mine != epoch || !job || job->promptId != pid || history->error() != QNetworkReply::NoError) return;
                const auto entry = QJsonDocument::fromJson(history->readAll()).object()[pid].toObject();
                if (entry.isEmpty()) {
                    // A job can leave the queue just before its history is written: give it a moment.
                    if (++job->waiting >= 3) failJob(id, "ComfyUI no longer has this job (it may have restarted). Try again.");
                    return;
                }
                const auto st = entry["status"].toObject();
                if (st["status_str"].toString() == "error") {
                    QString why = "the job failed";
                    for (const auto& m : st["messages"].toArray()) {
                        const auto pair = m.toArray();
                        if (pair[0].toString() == "execution_error") why = pair[1].toObject()["exception_message"].toString().trimmed();
                        if (pair[0].toString() == "execution_interrupted") why = "the job was interrupted";
                    }
                    failJob(id, "ComfyUI: " + why);
                    return;
                }
                collect(id, entry["outputs"].toObject());
            });
        }
    });
}

void Generator::collect(int id, const QJsonObject& outputs) {
    Job* job = find(id);
    if (!job || job->state == "collecting") return;
    // Saved pictures, from the workflow's output nodes; previews only when nothing was saved.
    QJsonArray images, previews;
    const QJsonArray wanted = editor->workflows()->outputs(job->workflow);
    for (auto it = outputs.begin(); it != outputs.end(); ++it) {
        if (!wanted.isEmpty() && !wanted.contains(it.key())) continue;
        for (const auto& image : it.value().toObject()["images"].toArray())
            (image.toObject()["type"].toString() == "temp" ? previews : images).append(image);
    }
    if (images.isEmpty()) images = previews;
    if (images.isEmpty()) { failJob(id, "ComfyUI finished without a picture. Check the workflow's Save Image node."); return; }
    say(*job, "collecting", "Downloading…");
    auto project = editor->current();
    const QString folder = project->dir.filePath("Jobs");
    QDir().mkpath(folder);
    auto files = std::make_shared<QStringList>();
    auto left = std::make_shared<int>(images.size());
    auto failed = std::make_shared<QString>();
    for (int i = 0; i < images.size(); ++i) {
        const auto image = images[i].toObject();
        QUrlQuery query;
        query.addQueryItem("filename", image["filename"].toString());
        query.addQueryItem("subfolder", image["subfolder"].toString());
        query.addQueryItem("type", image["type"].toString());
        auto reply = net.get(editor->comfy()->request("/view?" + query.toString(QUrl::FullyEncoded)));
        const QString suffix = QFileInfo(image["filename"].toString()).suffix().toLower();
        const QString path = QDir(folder).filePath(QString("%1-%2.%3").arg(job->promptId).arg(i + 1).arg(suffix.isEmpty() ? "png" : suffix));
        const int mine = epoch;
        connect(reply, &QNetworkReply::finished, this, [this, reply, id, path, files, left, failed, mine] {
            reply->deleteLater();
            if (mine != epoch) return;
            QFile out(path);
            if (reply->error() != QNetworkReply::NoError) *failed = "Could not download the picture: " + ComfyJobs::errorOf(reply);
            else if (!out.open(QIODevice::WriteOnly) || out.write(reply->readAll()) < 0) *failed = "Could not save the picture in the project.";
            else files->append(path);
            out.close();
            if (--*left > 0) return;
            Job* job = find(id);
            if (!job) return;
            if (!failed->isEmpty() && files->isEmpty()) { failJob(id, *failed); return; }
            auto plan = editor->plan();
            if (plan->item(job->owner, job->ownerId).isEmpty()) {
                failJob(id, QString("The %1 was removed before the picture arrived; it is in the project's Jobs folder.").arg(ownerNoun(job->owner)));
                return;
            }
            QVariantList commands;
            files->sort();
            QVariantMap generation{{"workflow", job->workflow}, {"workflowName", job->workflowName}, {"use", job->use},
                                   {"prompt", job->prompt}, {"promptId", job->promptId}};
            if (job->hasSeed) generation["seed"] = double(job->seed);
            if (!job->shown.isEmpty()) generation["settings"] = job->shown.toVariantList();
            for (const QString& f : *files)
                commands.append(QVariantMap{{"op", "ref.add"}, {"owner", job->owner}, {"ownerId", job->ownerId}, {"path", f},
                                            {"source", "generated"}, {"name", "Generated with " + job->workflowName}, {"generation", generation}});
            const auto r = plan->run(commands.size() == 1 ? commands[0].toMap()
                                     : QVariantMap{{"op", "batch"}, {"label", QString("Add %1 generated references").arg(commands.size())}, {"commands", commands}});
            if (!r.value("ok").toBool()) { failJob(id, r.value("error").toString()); return; }
            for (const QString& f : *files) QFile::remove(f);  // copied into Media/References
            record(*job, "done");
            for (int i = 0; i < list.size(); ++i) if (list[i].id == id) { list.removeAt(i); break; }
            emit changed();
        });
    }
}

void Generator::failJob(int id, const QString& why) {
    Job* job = find(id);
    if (!job) return;
    if (!job->promptId.isEmpty()) record(*job, "failed", why);
    say(*job, "failed", why);
}

void Generator::record(const Job& job, const QString& state, const QString& detail) {
    auto project = editor->current();
    if (!project || job.promptId.isEmpty()) return;
    QSqlQuery q(project->db);
    // The detail column keeps the request; a failure's reason goes beside it.
    if (detail.isEmpty()) q.prepare("UPDATE jobs SET state=? WHERE prompt_id=?");
    else {
        q.prepare("UPDATE jobs SET state=?, detail=json_set(detail, '$.error', ?) WHERE prompt_id=?");
        q.addBindValue(state);
        q.addBindValue(detail);
        q.addBindValue(job.promptId);
        q.exec();
        return;
    }
    q.addBindValue(state);
    q.addBindValue(job.promptId);
    q.exec();
}

bool Generator::ensureTable() const {
    auto project = editor->current();
    if (!project) return false;
    QSqlQuery q(project->db);
    if (!q.exec("CREATE TABLE IF NOT EXISTS jobs(prompt_id TEXT PRIMARY KEY, kind TEXT NOT NULL, server TEXT NOT NULL, "
                "language TEXT NOT NULL, lyrics TEXT NOT NULL, submitted INTEGER NOT NULL, state TEXT NOT NULL, detail TEXT)"))
        return false;
    bool hasClient = false;
    if (q.exec("PRAGMA table_info(jobs)")) while (q.next()) hasClient |= q.value(1).toString() == "client_id";
    return hasClient || q.exec("ALTER TABLE jobs ADD COLUMN client_id TEXT");
}

void Generator::cancel(int id) {
    Job* job = find(id);
    if (!job) return;
    if (!job->promptId.isEmpty() && job->state != "failed") {
        auto r = editor->comfy()->request("/queue");
        r.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        auto i = editor->comfy()->request("/interrupt");
        i.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
        QList<QNetworkReply*> replies{net.post(r, QJsonDocument(QJsonObject{{"delete", QJsonArray{job->promptId}}}).toJson())};
        if (job->state == "running") replies << net.post(i, QJsonDocument(QJsonObject{{"prompt_id", job->promptId}}).toJson());
        for (auto reply : replies) connect(reply, &QNetworkReply::finished, reply, &QObject::deleteLater);
        record(*job, "cancelled");
    }
    dismiss(id);
}

void Generator::dismiss(int id) {
    for (int i = 0; i < list.size(); ++i)
        if (list[i].id == id) { list.removeAt(i); emit changed(); return; }
}

void Generator::retry(int id) {
    Job* job = find(id);
    if (!job) return;
    const QString owner = job->owner, use = job->use;
    const int ownerId = job->ownerId;
    // The same request again: same workflow, seed and settings.
    QVariantMap settings;
    for (const auto& v : job->shown) settings[v.toObject()["key"].toString()] = v.toObject()["value"].toVariant();
    const QVariantMap again{{"workflow", job->workflow}, {"seed", QString::number(job->seed)}, {"settings", settings}};
    const QString why = generateWith(owner, ownerId, use, again);
    if (why.isEmpty()) dismiss(id);
    else say(*find(id), "failed", why);
}

void Generator::resume() {
    detach();
    auto project = editor->current();
    if (!project || !project->db.tables().contains("jobs") || !ensureTable()) return;
    QSqlQuery q(project->db);
    if (!q.exec("SELECT prompt_id, server, detail, state FROM jobs WHERE kind='reference' AND state IN ('queued','running') ORDER BY submitted"))
        return;
    while (q.next()) {
        const auto d = QJsonDocument::fromJson(q.value(2).toString().toUtf8()).object();
        Job job;
        job.id = ++serial;
        job.promptId = q.value(0).toString();
        job.owner = d["owner"].toString();
        job.ownerId = d["ownerId"].toInt();
        job.use = d["use"].toString();
        job.workflow = d["workflow"].toString();
        job.workflowName = d["workflowName"].toString();
        job.prompt = d["prompt"].toString();
        job.seed = qint64(d["seed"].toDouble());
        job.hasSeed = d["hasSeed"].toBool(true);
        job.shown = d["settings"].toArray();
        for (const auto& v : job.shown) job.settings[v.toObject()["key"].toString()] = v.toObject()["value"];
        job.graph = d["graph"].toObject();
        if (q.value(1).toString() != editor->comfy()->url()) {
            job.state = "failed";
            job.text = "Started on " + q.value(1).toString() + ", which is not the current ComfyUI.";
        } else {
            job.state = q.value(3).toString();
            job.text = "Reconnecting…";
        }
        list.append(job);
    }
    emit changed();
    if (!list.isEmpty()) { poll.start(); check(); }
}

void Generator::detach() {
    ++epoch;
    poll.stop();
    if (list.isEmpty()) return;
    list.clear();
    emit changed();
}
