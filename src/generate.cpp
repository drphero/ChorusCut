#include "generate.h"
#include "editor.h"
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QPainter>
#include <QHttpMultiPart>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkReply>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrlQuery>
#include <QStandardPaths>
#include <QUuid>
#include <QWebSocket>
#include <cmath>
#include <memory>

namespace {
constexpr int kPollMs = 1500;
// The longest side of a live preview clip. Takes are about 1216 to 1504 wide; the viewer shows
// the preview large, while every step's clip crosses the network.
constexpr int kPreviewSide = 1024;
QString ownerNoun(const QString& owner) { return owner == "look" ? "look" : owner == "location" ? "location" : owner == "prop" ? "prop" : "character"; }
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
    // Live preview clips are only for jobs still listed; those of an earlier run are stale.
    liveDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/live";
    QDir(liveDir).removeRecursively();
    connect(this, &Generator::changed, this, &Generator::sweepLive);
}

void Generator::watch(int id) {
    if (id && !find(id)) id = 0;
    if (id == watched) return;
    watched = id;
    emit watchingChanged();
}

void Generator::sweepLive() {
    bool again = false;
    for (auto it = liveFiles.begin(); it != liveFiles.end();) {
        if (find(it.key())) { ++it; continue; }
        auto& files = it.value();
        for (auto f = files.begin(); f != files.end();)
            if (QFile::exists(*f) && !QFile::remove(*f)) { again = true; ++f; }
            else f = files.erase(f);
        if (files.isEmpty()) it = liveFiles.erase(it); else ++it;
    }
    // The viewer may still be reading the last clip, and Windows cannot unlink an open
    // file; sweep once more when it has let go.
    if (again) QTimer::singleShot(300, this, [this] { sweepLive(); });
    if (watched && !find(watched)) { watched = 0; emit watchingChanged(); }
}

void Generator::addLiveNode(Job& job, QJsonObject& graph) {
    // KJ Nodes' Model Preview Override goes directly before the guider: it wraps the model so
    // each sampling step's frames, decoded by the H3 tiny VAE, go to the client that queued the
    // job. preview_frames is the take's length, or it shows one picture that never changes.
    auto comfy = editor->comfy();
    if (!comfy->previewsOn() || job.owner != "shot" || job.frames <= 0) return;
    QStringList guiders;
    for (const QString& k : graph.keys()) if (graph[k].toObject()["class_type"] == "BasicGuider") guiders << k;
    if (guiders.size() != 1) return;
    auto guider = graph[guiders[0]].toObject();
    auto inputs = guider["inputs"].toObject();
    if (!inputs["model"].isArray()) return;
    int next = 0;
    for (const QString& k : graph.keys()) next = qMax(next, k.toInt());
    job.liveNode = QString::number(next + 1);
    graph[job.liveNode] = QJsonObject{{"class_type", "ModelPreviewOverrideKJ"},
                                      {"inputs", QJsonObject{{"model", inputs["model"]}, {"max_resolution", kPreviewSide}, {"jpeg_quality", 80},
                                                             {"suppress_default_preview", true}, {"preview_frames", qBound(1, job.frames, 1024)},
                                                             {"preview_fps", comfy->previewFps()}, {"tiny_vae", comfy->previewVae()}}},
                                      {"_meta", QJsonObject{{"title", "ChorusCut: live preview"}}}};
    inputs["model"] = QJsonArray{job.liveNode, 0};
    guider["inputs"] = inputs;
    graph[guiders[0]] = guider;
    job.liveFps = comfy->previewFps();
    job.client = clientId;
}

void Generator::listen() {
    // A socket for each client ID with a live job queued or running, none otherwise.
    QSet<QString> want;
    for (const auto& j : list)
        if (!j.liveNode.isEmpty() && (j.state == "queued" || j.state == "running")) want.insert(j.client.isEmpty() ? clientId : j.client);
    for (auto it = sockets.begin(); it != sockets.end();) {
        if (want.contains(it.key())) { ++it; continue; }
        it.value()->disconnect(this);
        it.value()->abort();
        it.value()->deleteLater();
        runningPrompt.remove(it.key());
        it = sockets.erase(it);
    }
    for (const QString& c : want) if (!sockets.contains(c)) openSocket(c);
}

void Generator::openSocket(const QString& client) {
    QNetworkRequest r = editor->comfy()->request("/ws");
    QUrl url = r.url();
    url.setScheme(url.scheme() == "https" ? "wss" : "ws");
    url.setQuery("clientId=" + client);
    r.setUrl(url);
    auto socket = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
    // A clip of a whole take can be several megabytes per step.
    socket->setMaxAllowedIncomingMessageSize(256 * 1024 * 1024);
    socket->setMaxAllowedIncomingFrameSize(256 * 1024 * 1024);
    sockets[client] = socket;
    connect(socket, &QWebSocket::textMessageReceived, this, [this, client](const QString& text) {
        const auto message = QJsonDocument::fromJson(text.toUtf8()).object();
        const QString type = message["type"].toString();
        const auto data = message["data"].toObject();
        if (type == "execution_start" || type == "executing" || type == "progress" || type == "executed") {
            const QString pid = data["prompt_id"].toString();
            if (!pid.isEmpty()) runningPrompt[client] = pid;
            return;
        }
        if (type != "kj_preview_override") return;
        const QString node = data["node_id"].isString() ? data["node_id"].toString() : QString::number(data["node_id"].toInt());
        const QString pid = runningPrompt.value(client);
        Job* job = nullptr;
        for (auto& j : list)
            if ((j.client.isEmpty() ? clientId : j.client) == client && j.liveNode == node && j.state != "failed" && (pid.isEmpty() || j.promptId == pid)) { job = &j; break; }
        if (job) showLive(*job, data);
    });
    // Previews are a nicety: a lost socket is opened again on the next poll, and polling still
    // finishes the job.
    connect(socket, &QWebSocket::disconnected, this, [this, socket, client] {
        if (sockets.value(client) != socket) return;
        sockets.remove(client);
        socket->deleteLater();
    });
    socket->open(r);
}

void Generator::showLive(Job& job, const QJsonObject& data) {
    if (data["total"].toInt() > 0) job.liveSteps = data["total"].toInt();
    const int step = data["step"].toInt();
    // Step 0 is the starting noise: only the step count is worth showing.
    if (step <= 0 || !data["image"].isString()) {
        if (job.state != "running") say(job, "running", "Generating…");
        emit changed();
        return;
    }
    const QString mime = data["mime"].toString("image/jpeg");
    const QString ext = mime == "image/webp" ? "webp" : mime == "video/mp4" ? "mp4" : "jpg";
    QDir().mkpath(liveDir);
    const QString path = QString("%1/job%2-step%3.%4").arg(liveDir).arg(job.id).arg(step).arg(ext);
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write(QByteArray::fromBase64(data["image"].toString().toLatin1()));
    f.close();
    auto& files = liveFiles[job.id];
    files << path;
    // The viewer may still be reading the clip before the latest; older ones can go.
    while (files.size() > 2) QFile::remove(files.takeFirst());
    job.livePath = path;
    job.liveStep = step;
    if (data["fps"].toInt() > 0) job.liveFps = data["fps"].toInt();
    job.liveWidth = data["w"].toInt();
    job.liveHeight = data["h"].toInt();
    if (job.state != "running") record(job, "running");
    say(job, "running", job.liveSteps ? QString("Generating… step %1 of %2").arg(step).arg(job.liveSteps) : "Generating…");
    emit changed();
}

QVariantList Generator::jobs() const {
    QVariantList out;
    for (const auto& j : list)
        out.append(QVariantMap{{"id", j.id}, {"owner", j.owner}, {"ownerId", j.ownerId}, {"use", j.use}, {"view", j.view},
                               {"workflow", j.workflowName}, {"state", j.state}, {"text", j.text},
                               {"started", double(j.started)}, {"since", double(j.since)}, {"what", j.what},
                               {"live", !j.liveNode.isEmpty()}, {"preview", j.livePath.isEmpty() ? QString() : QUrl::fromLocalFile(j.livePath).toString()},
                               {"step", j.liveStep}, {"steps", j.liveSteps}, {"previewFps", j.liveFps},
                               {"previewWidth", j.liveWidth}, {"previewHeight", j.liveHeight}});
    return out;
}

Generator::Job* Generator::find(int id) {
    for (auto& j : list) if (j.id == id) return &j;
    return nullptr;
}

void Generator::say(Job& job, const QString& state, const QString& text) {
    if (job.state == state && job.text == text) return;
    if (job.state != state) job.since = QDateTime::currentMSecsSinceEpoch();
    if (!job.started) job.started = job.since ? job.since : QDateTime::currentMSecsSinceEpoch();
    job.state = state;
    job.text = text;
    emit changed();
}

QString Generator::subjectOf(const QString& owner, int ownerId) const {
    // A look is drawn as its own words first, then its character's (what the look keeps).
    auto plan = editor->plan();
    const QString own = plan->item(owner, ownerId)["fields"].toMap()["prompt"].toString().trimmed();
    if (owner != "look") return own;
    const QString person = plan->item("character", plan->item("look", ownerId)["character"].toInt())["fields"].toMap()["prompt"].toString().trimmed();
    return own.isEmpty() ? person : person.isEmpty() ? own : own + "\n\n" + person;
}

QList<int> Generator::guessInputs(int view) const {
    // A first guess the user changes freely: a look's first view starts from its character's
    // chosen first picture; any later view from its own item's first view's chosen picture.
    auto plan = editor->plan();
    const auto v = plan->item("view", view);
    if (v.isEmpty()) return {};
    const QString owner = v["owner"].toString();
    const int ownerId = v["ownerId"].toInt();
    const auto views = plan->viewsFor(owner, ownerId);
    QList<int> out;
    auto chosenOf = [&](const QVariantList& list, int skip) {
        for (const auto& x : list) {
            const auto m = x.toMap();
            if (m["id"].toInt() != skip && !m["chosen"].toMap().isEmpty()) return m["chosen"].toMap()["id"].toInt();
        }
        return 0;
    };
    if (owner == "look" && !views.isEmpty() && views.first().toMap()["id"].toInt() == view) {
        if (int c = chosenOf(plan->viewsFor("character", plan->item("look", ownerId)["character"].toInt()), 0)) out << c;
    } else if (int c = chosenOf(views, view)) {
        out << c;
    }
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
    if (use == "ref-text" && subjectOf(owner, ownerId).isEmpty()) return "Write a picture prompt first, or ask your agent to write one.";
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

QVariantMap Generator::viewOptions(int view, const QString& wantedUse, const QString& wantedWorkflow) const {
    auto flows = editor->workflows();
    auto plan = editor->plan();
    const auto v = plan->item("view", view);
    if (v.isEmpty()) return {{"ready", false}, {"why", "That view is gone."}};
    const auto f = v["fields"].toMap();
    const QString owner = v["owner"].toString();
    const int ownerId = v["ownerId"].toInt();
    const QString use = !wantedUse.isEmpty() ? wantedUse : f["from"].toString() == "pictures" ? "ref-edit" : "ref-text";
    QString workflow = wantedWorkflow;
    if (workflow.isEmpty() || !flows->serves(workflow, use)) workflow = flows->forUse(use);
    QVariantList inputs;
    bool guessed = false;
    for (const auto& i : f["inputs"].toList()) if (!plan->item("reference", i.toInt()).isEmpty()) inputs << i.toInt();
    if (inputs.isEmpty() && use == "ref-edit") { for (int i : guessInputs(view)) inputs << i; guessed = !inputs.isEmpty(); }
    QString why = whyNot(owner, ownerId, use, workflow);
    const int most = workflow.isEmpty() ? 0 : flows->pictureCount(workflow);
    const bool known = !workflow.isEmpty() && infoKnown(workflow);
    const int least = workflow.isEmpty() ? 0 : known ? flows->minPictures(workflow, optionalFor(workflow)) : (use == "ref-edit" ? most : 0);
    if (why.isEmpty() && use == "ref-edit") {
        if (inputs.isEmpty()) why = "Pick the pictures to make it from.";
        else if (inputs.size() > most) why = QString("%1 takes at most %2 %3.").arg(flows->nameOf(workflow)).arg(most).arg(most == 1 ? "picture" : "pictures");
        else if (known && inputs.size() < least) why = QString("%1 needs at least %2 %3.").arg(flows->nameOf(workflow)).arg(least).arg(least == 1 ? "picture" : "pictures");
    }
    return {{"use", use}, {"workflow", workflow}, {"workflowName", workflow.isEmpty() ? QString() : flows->nameOf(workflow)},
            {"workflows", flows->ready(use)}, {"inputs", inputs}, {"guessed", guessed}, {"instruction", f["framing"].toString()},
            {"subject", subjectOf(owner, ownerId)}, {"why", why}, {"ready", why.isEmpty()},
            {"pictures", QVariantMap{{"min", least}, {"max", most}, {"known", known}}},
            {"pictureName", workflow.isEmpty() ? QString() : pictureLabel(workflow, 1)}, {"owner", owner}, {"ownerId", ownerId},
            {"name", f["name"].toString()}};
}

QString Generator::autoAspect(const QString& owner, const QString& workflow, const QString& key) const {
    // People and looks a portrait, props square, places and shots the video's frame.
    QString want = owner == "prop" ? "1:1" : "3:4";
    if (owner == "location" || owner == "shot") {
        want = editor->plan()->concept()["fields"].toMap()["aspect"].toString();
        if (want.isEmpty()) want = "16:9";
    }
    for (const auto& sv : editor->workflows()->settings(workflow)) {
        const auto s = sv.toMap();
        if (s["key"] != key || s["input"] != "aspect_ratio") continue;
        for (const auto& c : choiceMap.value(s["classType"].toString() + "/aspect_ratio").toList()) {
            const QString choice = c.toString();
            if (choice == want || choice.startsWith(want + " ") || choice.startsWith(want + "(")) return choice;
        }
    }
    return {};
}

QString Generator::keepSettings(const QString& workflow, const QVariantMap& values) {
    auto flows = editor->workflows();
    QString error;
    const QJsonObject checked = flows->checkValues(workflow, values, error);
    if (!error.isEmpty()) return error;
    return checked.isEmpty() ? QString() : flows->setValues(workflow, checked);
}

QString Generator::pictureLabel(const QString& workflow, int n) const {
    QString pattern = editor->workflows()->pictureName(workflow);
    if (pattern.isEmpty()) {
        // What the model's own nodes say: "<Picture 1>", "@Image1", "Picture 1", "image 1".
        QString text;
        const QString server = editor->comfy()->url();
        for (const QString& cls : editor->workflows()->classes(workflow)) text += info.value(server + "|" + cls).text + "\n";
        static const QList<QPair<QRegularExpression, QString>> known = {
            {QRegularExpression("<Picture\\s*([1inN]|\\{?n\\}?)>"), "<Picture {n}>"}, {QRegularExpression("@Image1\\b"), "@Image{n}"},
            {QRegularExpression("\\bPicture 1\\b"), "Picture {n}"}, {QRegularExpression("\\b[Ii]mage 1\\b"), "image {n}"}};
        for (const auto& [re, p] : known) if (re.match(text).hasMatch()) { pattern = p; break; }
    }
    if (pattern.isEmpty()) pattern = "picture {n}";
    return QString(pattern).replace("{n}", QString::number(n));
}

QString Generator::generate(const QString& owner, int ownerId, const QString& use) {
    return generateWith(owner, ownerId, use, {});
}

QString Generator::generateWith(const QString& ownerIn, int ownerIdIn, const QString& useIn, const QVariantMap& o) {
    if (!editor->current()) return "Open a project first.";
    auto flows = editor->workflows();
    auto plan = editor->plan();
    // The view: the one named, else the item's first.
    int view = o.value("view").toInt();
    QString owner = ownerIn;
    int ownerId = ownerIdIn;
    if (view) {
        const auto v = plan->item("view", view);
        if (v.isEmpty()) return "That view is gone.";
        owner = v["owner"].toString();
        ownerId = v["ownerId"].toInt();
    } else if (const auto views = plan->viewsFor(owner, ownerId); !views.isEmpty()) {
        view = views.first().toMap()["id"].toInt();
    }
    const auto vf = plan->item("view", view)["fields"].toMap();
    QString use = useIn;
    if (use.isEmpty()) use = vf["from"].toString() == "pictures" ? "ref-edit" : "ref-text";
    if (use != "ref-text" && use != "ref-edit") return "Unknown kind of picture.";
    const QString workflow = o.value("workflow").toString().isEmpty() ? flows->forUse(use) : o.value("workflow").toString();
    QString why = whyNot(owner, ownerId, use, workflow);
    if (!why.isEmpty()) return why;
    // The pictures to edit from: as given, else the view's own picks, else the first guess.
    QList<int> inputs;
    if (use == "ref-edit") {
        QVariantList given = o.contains("inputs") ? o.value("inputs").toList() : vf["inputs"].toList();
        for (const auto& i : given) if (!plan->item("reference", i.toInt()).isEmpty()) inputs << i.toInt();
        if (inputs.isEmpty() && !o.contains("inputs")) inputs = guessInputs(view);
        if (inputs.isEmpty()) return "Pick the pictures to make it from (Generate ▸ With options…).";
        const int most = flows->pictureCount(workflow);
        if (inputs.size() > most) return QString("%1 takes at most %2 %3.").arg(flows->nameOf(workflow)).arg(most).arg(most == 1 ? "picture" : "pictures");
    }
    const QString instruction = (o.contains("instruction") ? o.value("instruction").toString() : vf["framing"].toString()).trimmed();
    const QString subject = subjectOf(owner, ownerId);
    const QString prompt = instruction.isEmpty() ? subject : subject.isEmpty() ? instruction : instruction + "\n\n" + subject;
    if (prompt.isEmpty()) return "Write a picture prompt or an instruction first.";
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
    // What stays with the workflow: the settings the user changed ("keep"), else all of them.
    if (o.contains("keep")) { error = keepSettings(workflow, o.value("keep").toMap()); if (!error.isEmpty()) return error; }
    else if (!chosen.isEmpty()) { error = flows->setValues(workflow, chosen); if (!error.isEmpty()) return error; }
    // The aspect ratio is the user's when they chose it now (the dialog says so) or kept one with
    // the workflow; otherwise it is picked for the kind of picture when queueing.
    bool aspectChosen = false;
    for (const auto& v : flows->settings(workflow)) {
        const auto st = v.toMap();
        if (st["input"] != "aspect_ratio") continue;
        aspectChosen |= st["value"] != st["original"];
        if (o.contains("aspectChosen")) aspectChosen |= o.value("aspectChosen").toBool();
        else aspectChosen |= chosen.contains(st["key"].toString());
    }
    // What the pictures were made with, as the reference will show it.
    QJsonArray shown;
    QJsonObject used;
    for (const auto& v : flows->settings(workflow)) {
        const auto s = v.toMap();
        const QJsonValue value = chosen.contains(s["key"].toString()) ? chosen[s["key"].toString()] : QJsonValue::fromVariant(s["value"]);
        used[s["key"].toString()] = value;
        shown.append(QJsonObject{{"key", s["key"].toString()}, {"label", s["label"].toString()}, {"value", value}});
    }
    QStringList files;
    for (int i : inputs) files << editor->current()->dir.filePath(plan->item("reference", i)["file"].toString());
    const qint64 first = seed >= 0 ? seed : qint64(QRandomGenerator::global()->bounded(quint64(1) << 48));
    QList<int> made;
    for (int i = 0; i < count; ++i) {
        Job job;
        job.id = ++serial;
        job.owner = owner;
        job.ownerId = ownerId;
        job.use = use;
        job.workflow = workflow;
        job.workflowName = flows->nameOf(workflow);
        job.prompt = prompt;
        job.view = view;
        job.inputs = inputs;
        job.instruction = instruction;
        job.hasSeed = flows->hasSlot(workflow, "seed");
        // Fixed: this seed and the ones after it. Random: a fresh one for each picture.
        job.seed = seed >= 0 ? std::min(kMaxSeed, first + i) : i == 0 ? first : qint64(QRandomGenerator::global()->bounded(quint64(1) << 48));
        job.settings = used;
        job.shown = shown;
        job.aspectChosen = aspectChosen;
        list.append(job);
        say(list.last(), "uploading", "Preparing…");
        made << job.id;
    }
    // ComfyUI's node details first, so pictures left out can be taken out of the graph.
    withInfo(workflow, [this, made, files] {
        for (int id : made) {
            Job* job = find(id);
            if (!job) continue;
            say(*job, "uploading", files.isEmpty() ? "Preparing…" : "Uploading pictures…");
            upload(id, files, {});
        }
    });
    return {};
}

// ---------------------------------------------------------------- takes

namespace {
// A shot's route decides the kind of take: H3 lip-sync sings to the song, reference-to-video
// doesn't. Other routes (stills, first and last frame) come later.
QString takeUse(const QString& route) { return route == "h3-lipsync" ? "take-sync" : route == "ref2v" ? "take-ref" : QString(); }
constexpr double kDefaultHandles = 0.5;
}

int Generator::takeFrames(const QString& workflow, double seconds, double fps, double& handles, QString& why) const {
    int lo = 1, step = 1, hi = 0;
    const QString server = editor->comfy()->url();
    for (const QString& cls : editor->workflows()->classes(workflow)) {
        const auto ni = info.value(server + "|" + cls);
        if (ni.lengthStep > 0) { lo = qMax(1, ni.lengthMin); step = ni.lengthStep; hi = ni.lengthMax; break; }
    }
    // One generation holds about 15 s (H3's 362 frames at 24 fps), whatever the node allows.
    const int most = int(std::floor(editor->plan()->maxFrames("ref2v") / editor->fps() * fps + 1e-6));
    const int limit = hi > 0 ? qMin(hi, most) : most;
    auto snap = [&](double s) {
        const int want = qMax(lo, int(std::ceil(s * fps - 1e-6)));
        return lo + int(std::ceil(double(want - lo) / step - 1e-9)) * step;
    };
    if (snap(seconds) > limit) {
        why = QString("This shot is longer than one take can be (%1 s): split it.").arg(QString::number(limit / fps, 'f', 1));
        return 0;
    }
    // Handles shrink to fit the limit.
    while (handles > 0 && snap(seconds + 2 * handles) > limit) handles = qMax(0.0, handles - 1.0 / fps);
    return snap(seconds + 2 * handles);
}

QVariantMap Generator::takeOptions(int shotId, const QString& wanted, double handles) const {
    auto plan = editor->plan();
    auto flows = editor->workflows();
    const auto shot = plan->shot(shotId);
    if (shot.isEmpty()) return {{"ready", false}, {"why", "That shot is gone."}};
    const auto f = shot["fields"].toMap();
    const QString route = f["route"].toString();
    const QString use = takeUse(route);
    QVariantMap out{{"route", route}, {"use", use}, {"lipSync", use == "take-sync"}, {"workflows", QVariantList{}}};
    QString why;
    if (use.isEmpty())
        why = route.isEmpty() ? "Choose the shot's route: H3 lip-sync or reference-to-video."
                              : "Takes are made with H3 lip-sync or reference-to-video for now. Change the shot's route to one of them.";
    QString workflow;
    if (!use.isEmpty()) {
        out["workflows"] = flows->ready(use);
        // Asked for, else the one kept on the shot (the take dialog's Save), else the library's default.
        const QString kept = f["workflow"].toString();
        workflow = !wanted.isEmpty() && flows->serves(wanted, use) ? wanted
                 : !kept.isEmpty() && flows->serves(kept, use) ? kept : flows->forUse(use);
        if (workflow.isEmpty() && why.isEmpty())
            why = use == "take-sync" ? "Set up a workflow for takes singing to the song (Workflows…)." : "Set up a workflow for takes from reference pictures (Workflows…).";
    }
    out["workflow"] = workflow;
    out["workflowName"] = workflow.isEmpty() ? QString() : flows->nameOf(workflow);
    out["prompt"] = f["prompts"].toMap().value(route).toString();
    QVariantList pictures;
    const auto picks = plan->shotPictures(shotId)["picked"].toList();
    for (int i = 0; i < picks.size(); ++i) {
        const auto ref = plan->item("reference", picks[i].toMap()["id"].toInt());
        if (ref.isEmpty()) continue;
        pictures.append(QVariantMap{{"id", ref["id"]}, {"url", ref["url"]}, {"label", workflow.isEmpty() ? QString("<Picture %1>").arg(i + 1) : pictureLabel(workflow, i + 1)},
                                    {"viewName", ref["viewName"]}});
    }
    out["pictures"] = pictures;
    const int most = workflow.isEmpty() ? 0 : flows->pictureCount(workflow);
    out["maxPictures"] = most;
    if (why.isEmpty() && pictures.size() > most) why = QString("%1 takes at most %2 pictures; this shot has %3.").arg(flows->nameOf(workflow)).arg(most).arg(pictures.size());
    if (why.isEmpty() && out["prompt"].toString().trimmed().isEmpty()) why = "Write the shot's video prompt first, or ask your agent to.";
    const double fps = workflow.isEmpty() ? 24 : flows->fps(workflow);
    double h = handles >= 0 ? handles : kDefaultHandles;
    const double seconds = (shot["end"].toInt() - shot["start"].toInt()) / editor->fps();
    QString tooLong;
    const int frames = takeFrames(workflow, seconds, fps, h, tooLong);
    if (why.isEmpty() && !tooLong.isEmpty()) why = tooLong;
    out["handles"] = h;
    out["frames"] = frames;
    out["fps"] = fps;
    out["seconds"] = frames / fps;
    out["shotSeconds"] = seconds;
    out["why"] = why;
    out["ready"] = why.isEmpty();
    return out;
}

namespace {
// A silent 48 kHz stereo 16-bit WAV of this length, for a face refine of a take that isn't sung.
QString writeSilence(double seconds, const QString& path) {
    const quint32 frames = quint32(std::llround(seconds * 48000)), bytes = frames * 4;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return "Cannot write " + QFileInfo(path).fileName() + ".";
    QByteArray h;
    auto u32 = [&](quint32 v) { for (int i = 0; i < 4; ++i) h.append(char((v >> (8 * i)) & 0xff)); };
    auto u16 = [&](quint16 v) { h.append(char(v & 0xff)); h.append(char(v >> 8)); };
    h.append("RIFF"); u32(36 + bytes); h.append("WAVEfmt "); u32(16); u16(1); u16(2); u32(48000); u32(48000 * 4); u16(4); u16(16);
    h.append("data"); u32(bytes);
    f.write(h);
    f.write(QByteArray(int(bytes), '\0'));
    return {};
}
}

QVariantMap Generator::refineOptions(int takeId, const QString& wanted) const {
    auto plan = editor->plan();
    auto flows = editor->workflows();
    const auto take = plan->item("take", takeId);
    if (take.isEmpty()) return {{"ready", false}, {"why", "That take is gone."}, {"people", QVariantList{}}, {"workflows", QVariantList{}}};
    const int shotId = take["shot"].toInt();
    const auto shot = plan->item("shot", shotId);
    QVariantMap out{{"take", takeId}, {"shot", shotId}, {"lipSync", take["locked"].toBool()}, {"frames", take["frames"]},
                    {"workflows", flows->ready("take-face")}, {"width", take["width"]}, {"height", take["height"]},
                    {"upscaled", take.contains("upscaledUrl") && !take["upscaledMissing"].toBool()},
                    {"upscaledWidth", take["upscaledWidth"]}, {"upscaledHeight", take["upscaledHeight"]}};
    const QString workflow = !wanted.isEmpty() && flows->serves(wanted, "take-face") ? wanted : flows->forUse("take-face");
    out["workflow"] = workflow;
    out["workflowName"] = workflow.isEmpty() ? QString() : flows->nameOf(workflow);
    const int most = workflow.isEmpty() ? 3 : flows->pictureCount(workflow);
    out["maxPictures"] = most;
    QString why;
    if (workflow.isEmpty()) why = "Set up a workflow for refining faces (Workflows…): H3 Face Refine, exported in the Export (API) format.";
    else if (!QFileInfo::exists(editor->current()->dir.filePath(take["file"].toString()))) why = "This take's video file is missing.";
    // The take's pictures, to start each person from what made it.
    QList<int> made;
    for (const auto& v : take["generation"].toMap()["inputs"].toList()) made << v.toInt();
    QVariantList people;
    const QVariant who = shot["fields"].toMap()["who"];
    for (const auto& w : who.typeId() == QMetaType::QVariantList ? who.toList() : QVariantList{}) {
        const int character = w.toMap()["character"].toInt(), look = w.toMap()["look"].toInt();
        const auto person = plan->item("character", character);
        if (person.isEmpty()) continue;
        const QString name = person["fields"].toMap()["name"].toString().isEmpty() ? "Someone" : person["fields"].toMap()["name"].toString();
        QVariantList own = plan->referencesFor("character", character), available = own;
        if (look) for (const auto& r : plan->referencesFor("look", look)) available << r;
        QVariantList faces;
        int face = 0;
        // Found by their Face view's chosen picture, else any chosen picture of theirs, else any.
        for (const auto& r : own) if (!r.toMap()["missing"].toBool()) faces << r;
        for (const auto& r : faces) if (!face && r.toMap()["approved"].toBool() && r.toMap()["viewName"].toString().compare("Face", Qt::CaseInsensitive) == 0) face = r.toMap()["id"].toInt();
        for (const auto& r : faces) if (!face && r.toMap()["approved"].toBool()) face = r.toMap()["id"].toInt();
        if (!face && !faces.isEmpty()) face = faces.first().toMap()["id"].toInt();
        QSet<int> theirs;
        for (const auto& r : available) theirs.insert(r.toMap()["id"].toInt());
        // Their face picture first (what a face pass is about), then theirs among what made the take.
        QVariantList pictures;
        if (face) pictures << face;
        for (int id : made) if (theirs.contains(id) && id != face && pictures.size() < qMin(3, most)) pictures << id;
        if (pictures.size() < 2)
            for (const auto& r : available)
                if (r.toMap()["approved"].toBool() && !r.toMap()["missing"].toBool() && !pictures.contains(r.toMap()["id"]) && pictures.size() < qMin(3, most)) pictures << r.toMap()["id"];
        people.append(QVariantMap{{"character", character}, {"look", look}, {"name", name}, {"face", face}, {"faces", faces},
                                  {"pictures", pictures}, {"available", available}, {"prompt", person["fields"].toMap()["facePrompt"].toString()},
                                  {"why", face ? QString() : "No picture of " + name + " to find their face by: add one to their Face view."}});
    }
    out["people"] = people;
    if (why.isEmpty() && people.isEmpty()) why = "No one is in this shot (Who), so there is no face to refine.";
    out["why"] = why;
    out["ready"] = why.isEmpty();
    return out;
}

int Generator::rootTake(int take) const {
    auto plan = editor->plan();
    for (int guard = 0; guard < 64; ++guard) {
        const auto g = plan->item("take", take)["generation"].toMap();
        if (g["use"] != "take-face" || plan->item("take", g["source"].toInt()).isEmpty()) return take;
        take = g["source"].toInt();
    }
    return take;
}

QVariantMap Generator::upscaleOptions(int takeId, const QString& wanted) const {
    auto plan = editor->plan();
    auto flows = editor->workflows();
    const auto take = plan->item("take", takeId);
    if (take.isEmpty()) return {{"ready", false}, {"why", "That take is gone."}, {"pictures", QVariantList{}}, {"workflows", QVariantList{}}};
    const int shotId = take["shot"].toInt();
    int number = 0;
    for (const auto& t : plan->takesFor(shotId)) if (t.toMap()["id"].toInt() == takeId) number = t.toMap()["number"].toInt();
    QVariantMap out{{"take", takeId}, {"shot", shotId}, {"number", number}, {"lipSync", take["locked"].toBool()}, {"frames", take["frames"]},
                    {"width", take["width"]}, {"height", take["height"]}, {"upscaled", take.contains("upscaledUrl")}, {"hd", take["hd"]},
                    {"workflows", flows->ready("take-upscale")}};
    const QString workflow = !wanted.isEmpty() && flows->serves(wanted, "take-upscale") ? wanted : flows->forUse("take-upscale");
    out["workflow"] = workflow;
    out["workflowName"] = workflow.isEmpty() ? QString() : flows->nameOf(workflow);
    // The prompt and pictures the take was made with: a refined take's are its chain's first take's.
    const auto made = plan->item("take", rootTake(takeId))["generation"].toMap();
    out["prompt"] = made["prompt"].toString();
    QVariantList pictures;
    int gone = 0;
    for (const auto& v : made["inputs"].toList()) {
        const auto ref = plan->item("reference", v.toInt());
        if (ref.isEmpty() || ref["missing"].toBool()) { ++gone; continue; }
        pictures.append(QVariantMap{{"id", ref["id"]}, {"url", ref["url"]}, {"viewName", ref["viewName"]}, {"whose", ref["whose"]},
                                    {"width", ref["width"]}, {"height", ref["height"]},
                                    {"label", workflow.isEmpty() ? QString("<Picture %1>").arg(pictures.size() + 1) : pictureLabel(workflow, pictures.size() + 1)}});
    }
    out["pictures"] = pictures;
    out["gone"] = gone;
    for (const auto& v : workflow.isEmpty() ? QVariantList{} : flows->settings(workflow))
        if (v.toMap()["label"] == "Megapixels" || v.toMap()["input"].toString().contains("megapixels"))
            out["megapixels"] = QVariantMap{{"key", v.toMap()["key"]}, {"value", v.toMap()["value"]}};
    QString why;
    if (workflow.isEmpty()) why = "Set up a workflow for upscaling takes (Workflows…): H3 Upscale, exported in the Export (API) format.";
    else if (!QFileInfo::exists(editor->current()->dir.filePath(take["file"].toString()))) why = "This take's video file is missing.";
    else if (out["prompt"].toString().trimmed().isEmpty()) why = "There is no prompt recorded for this take, so it can't be re-drawn as it was made.";
    else if (pictures.size() > flows->pictureCount(workflow)) why = QString("%1 takes at most %2 pictures; this take was made with %3.").arg(flows->nameOf(workflow)).arg(flows->pictureCount(workflow)).arg(pictures.size());
    out["why"] = why;
    out["ready"] = why.isEmpty();
    return out;
}

QVariantList Generator::cutToUpscale() const {
    auto plan = editor->plan();
    QVariantList out;
    for (const auto& v : plan->timelineTakes()) {
        const int take = v.toMap()["take"].toInt();
        const auto t = plan->item("take", take);
        if (t["hd"].toBool()) continue;
        const int shot = t["shot"].toInt();
        int number = 0;
        for (const auto& x : plan->takesFor(shot)) if (x.toMap()["id"].toInt() == take) number = x.toMap()["number"].toInt();
        out.append(QVariantMap{{"take", take}, {"shot", shot}, {"shotNumber", plan->shot(shot)["number"]}, {"number", number},
                               {"frames", t["frames"]}, {"seconds", t["frames"].toDouble() / (t["fps"].toDouble() > 0 ? t["fps"].toDouble() : 24)}});
    }
    return out;
}

QString Generator::upscale(const QVariantList& takes, const QVariantMap& o) {
    auto flows = editor->workflows();
    if (takes.isEmpty()) return "Choose the takes to upscale.";
    if (!ensureTable()) return "Could not record the job in the project.";
    constexpr qint64 kMaxSeed = (qint64(1) << 53) - 1;
    qint64 seed = -1;
    const QString given = o.value("seed").toString().trimmed();
    if (!given.isEmpty()) {
        bool ok = false;
        seed = given.toLongLong(&ok);
        if (!ok || seed < 0 || seed > kMaxSeed) return "The seed must be a whole number from 0 to " + QString::number(kMaxSeed) + ".";
    }
    // Every take is checked before any is queued.
    QList<QVariantMap> ready;
    for (const auto& v : takes) {
        const auto opts = upscaleOptions(v.toInt(), o.value("workflow").toString());
        if (!opts["ready"].toBool()) {
            const auto shot = editor->plan()->shot(opts["shot"].toInt());
            return opts["shot"].toInt() ? QString("Shot %1, take %2: %3").arg(shot["number"].toInt()).arg(opts["number"].toInt()).arg(opts["why"].toString()) : opts["why"].toString();
        }
        ready << opts;
    }
    const QString workflow = ready.first()["workflow"].toString();
    QString error;
    const QJsonObject chosen = flows->checkValues(workflow, o.value("settings").toMap(), error);
    if (!error.isEmpty()) return error;
    if (o.contains("keep")) { error = keepSettings(workflow, o.value("keep").toMap()); if (!error.isEmpty()) return error; }
    QJsonArray shown;
    QJsonObject used;
    for (const auto& v : flows->settings(workflow)) {
        const auto st = v.toMap();
        const QJsonValue value = chosen.contains(st["key"].toString()) ? chosen[st["key"].toString()] : QJsonValue::fromVariant(st["value"]);
        used[st["key"].toString()] = value;
        shown.append(QJsonObject{{"key", st["key"].toString()}, {"label", st["label"].toString()}, {"value", value}});
    }
    QList<int> made;
    for (const auto& opts : ready) {
        Job job;
        job.id = ++serial;
        job.owner = "shot";
        job.ownerId = opts["shot"].toInt();
        job.use = "take-upscale";
        job.workflow = workflow;
        job.workflowName = flows->nameOf(workflow);
        job.prompt = opts["prompt"].toString();
        for (const auto& p : opts["pictures"].toList()) job.inputs << p.toMap()["id"].toInt();
        job.what = QString("Take %1").arg(opts["number"].toInt());
        job.hasSeed = flows->hasSlot(workflow, "seed");
        job.seed = seed >= 0 ? seed : qint64(QRandomGenerator::global()->bounded(quint64(1) << 48));
        job.settings = used;
        job.shown = shown;
        job.aspectChosen = true;
        job.source = opts["take"].toInt();
        list.append(job);
        say(list.last(), "uploading", "Preparing…");
        made << job.id;
    }
    withInfo(workflow, [this, made] { for (int id : made) startRefine(id); });
    return {};
}

QString Generator::refineFaces(int takeId, const QVariantList& passes, const QVariantMap& o) {
    auto plan = editor->plan();
    auto flows = editor->workflows();
    const auto opts = refineOptions(takeId, o.value("workflow").toString());
    if (!opts["ready"].toBool()) return opts["why"].toString();
    if (passes.isEmpty()) return "Tick whose face to refine.";
    const QString workflow = opts["workflow"].toString();
    QMap<int, QVariantMap> people;
    for (const auto& p : opts["people"].toList()) people[p.toMap()["character"].toInt()] = p.toMap();
    for (const auto& v : passes) {
        const auto pass = v.toMap();
        const auto person = people.value(pass["character"].toInt());
        if (person.isEmpty()) return "Only people in the shot (Who) can be refined.";
        if (plan->item("reference", pass["face"].toInt()).isEmpty()) return "Choose a picture to find " + person["name"].toString() + "'s face by.";
        if (pass["pictures"].toList().size() > opts["maxPictures"].toInt())
            return QString("%1 takes at most %2 pictures.").arg(flows->nameOf(workflow)).arg(opts["maxPictures"].toInt());
        for (const auto& r : pass["pictures"].toList())
            if (plan->item("reference", r.toInt()).isEmpty()) return "A picture picked for " + person["name"].toString() + " is gone.";
    }
    constexpr qint64 kMaxSeed = (qint64(1) << 53) - 1;
    qint64 seed = -1;
    const QString given = o.value("seed").toString().trimmed();
    if (!given.isEmpty()) {
        bool ok = false;
        seed = given.toLongLong(&ok);
        if (!ok || seed < 0 || seed > kMaxSeed) return "The seed must be a whole number from 0 to " + QString::number(kMaxSeed) + ".";
    }
    QString error;
    const QJsonObject chosen = flows->checkValues(workflow, o.value("settings").toMap(), error);
    if (!error.isEmpty()) return error;
    if (!ensureTable()) return "Could not record the job in the project.";
    if (o.contains("keep")) { error = keepSettings(workflow, o.value("keep").toMap()); if (!error.isEmpty()) return error; }
    QJsonArray shown;
    QJsonObject used;
    for (const auto& v : flows->settings(workflow)) {
        const auto st = v.toMap();
        const QJsonValue value = chosen.contains(st["key"].toString()) ? chosen[st["key"].toString()] : QJsonValue::fromVariant(st["value"]);
        used[st["key"].toString()] = value;
        shown.append(QJsonObject{{"key", st["key"].toString()}, {"label", st["label"].toString()}, {"value", value}});
    }
    const int shotId = opts["shot"].toInt();
    int previous = 0, first = 0;
    for (int i = 0; i < passes.size(); ++i) {
        const auto pass = passes[i].toMap();
        Job job;
        job.id = ++serial;
        job.owner = "shot";
        job.ownerId = shotId;
        job.use = "take-face";
        job.workflow = workflow;
        job.workflowName = flows->nameOf(workflow);
        job.prompt = pass["prompt"].toString().trimmed();
        for (const auto& r : pass["pictures"].toList()) job.inputs << r.toInt();
        job.character = pass["character"].toInt();
        job.face = pass["face"].toInt();
        job.what = people.value(job.character)["name"].toString() + "'s face";
        job.hasSeed = flows->hasSlot(workflow, "seed");
        job.seed = seed >= 0 ? seed : qint64(QRandomGenerator::global()->bounded(quint64(1) << 48));
        job.settings = used;
        job.shown = shown;
        job.aspectChosen = true;
        job.source = i == 0 ? takeId : 0;
        job.after = previous;
        job.fromUpscaled = i == 0 && o.value("fromUpscaled").toBool() && opts["upscaled"].toBool();
        list.append(job);
        if (i == 0) { first = job.id; say(list.last(), "uploading", "Preparing…"); }
        else say(list.last(), "waiting", "Waits for " + people.value(passes[i - 1].toMap()["character"].toInt())["name"].toString() + "'s face, then refines that take");
        previous = job.id;
    }
    withInfo(workflow, [this, first] { startRefine(first); });
    return {};
}

void Generator::startRefine(int id) {
    Job* job = find(id);
    if (!job) return;
    auto plan = editor->plan();
    auto project = editor->current();
    const auto take = plan->item("take", job->source);
    if (take.isEmpty()) { failJob(id, "The take to refine was removed."); return; }
    // The draft, or the take's upscaled version when the refine works from it.
    const bool up = job->fromUpscaled && take.contains("upscaledUrl");
    const QString video = up ? QUrl(take["upscaledUrl"].toString()).toLocalFile() : project->dir.filePath(take["file"].toString());
    if (!QFileInfo::exists(video)) { failJob(id, up ? "The take's upscaled version is missing." : "The take's video file is missing."); return; }
    // The result lands where its source is, as long, and as locked to the song.
    job->anchor = take["anchor"].toInt();
    job->frames = take["frames"].toInt();
    job->takeFps = take["fps"].toDouble() > 0 ? take["fps"].toDouble() : 24;
    job->locked = take["locked"].toBool();
    job->width = up ? take["upscaledWidth"].toInt() : take["width"].toInt();
    job->height = up ? take["upscaledHeight"].toInt() : take["height"].toInt();
    job->handles = take["generation"].toMap().value("handles", kDefaultHandles).toDouble();
    QStringList files{video};
    if (job->use == "take-face") {
        const auto face = plan->item("reference", job->face);
        if (face.isEmpty()) { failJob(id, "The picture to find the face by was removed."); return; }
        files << project->dir.filePath(face["file"].toString());
    }
    for (int r : job->inputs) {
        const auto ref = plan->item("reference", r);
        if (ref.isEmpty()) { failJob(id, "A picture picked for this pass was removed."); return; }
        files << project->dir.filePath(ref["file"].toString());
    }
    // Takes carry no sound: a sung take gets the song under it, any other silence of its length.
    const QString folder = project->dir.filePath("Jobs");
    QDir().mkpath(folder);
    job->excerpt = QDir(folder).filePath(QString("%1-shot%2-take%3-%4.wav").arg(job->use == "take-face" ? "face" : "upscale").arg(job->ownerId).arg(job->source).arg(job->id));
    const double seconds = job->frames / job->takeFps;
    const QString problem = job->locked ? editor->writeExcerpt(job->anchor / editor->fps(), seconds, job->excerpt) : writeSilence(seconds, job->excerpt);
    if (!problem.isEmpty()) { failJob(id, problem); return; }
    files << job->excerpt;
    say(*job, "uploading", job->locked ? "Uploading the take, pictures and the song…" : "Uploading the take and pictures…");
    upload(id, files, {});
}

QString Generator::generateTake(int shotId, const QVariantMap& o) {
    auto flows = editor->workflows();
    const auto opts = takeOptions(shotId, o.value("workflow").toString(), o.contains("handles") ? o.value("handles").toDouble() : -1);
    const QString prompt = o.contains("prompt") ? o.value("prompt").toString().trimmed() : opts["prompt"].toString().trimmed();
    QString why = opts["why"].toString();
    if (why == "Write the shot's video prompt first, or ask your agent to." && !prompt.isEmpty()) why.clear();
    if (!why.isEmpty()) return why;
    if (prompt.isEmpty()) return "Write the shot's video prompt first, or ask your agent to.";
    const QString workflow = opts["workflow"].toString();
    constexpr qint64 kMaxSeed = (qint64(1) << 53) - 1;
    qint64 seed = -1;
    const QString given = o.value("seed").toString().trimmed();
    if (!given.isEmpty()) {
        bool ok = false;
        seed = given.toLongLong(&ok);
        if (!ok || seed < 0 || seed > kMaxSeed) return "The seed must be a whole number from 0 to " + QString::number(kMaxSeed) + ".";
    }
    const int count = o.contains("count") ? o.value("count").toInt() : 1;
    if (count < 1 || count > 8) return "Generate 1 to 8 takes at a time.";
    QString error;
    const QJsonObject chosen = flows->checkValues(workflow, o.value("settings").toMap(), error);
    if (!error.isEmpty()) return error;
    if (!ensureTable()) return "Could not record the job in the project.";
    if (o.contains("keep")) { error = keepSettings(workflow, o.value("keep").toMap()); if (!error.isEmpty()) return error; }
    QJsonArray shown;
    QJsonObject used;
    bool aspectChosen = o.value("aspectChosen").toBool();
    for (const auto& v : flows->settings(workflow)) {
        const auto st = v.toMap();
        const QJsonValue value = chosen.contains(st["key"].toString()) ? chosen[st["key"].toString()] : QJsonValue::fromVariant(st["value"]);
        used[st["key"].toString()] = value;
        shown.append(QJsonObject{{"key", st["key"].toString()}, {"label", st["label"].toString()}, {"value", value}});
        if (st["input"] == "aspect_ratio") aspectChosen |= st["value"] != st["original"];
    }
    QStringList pictures;
    QList<int> inputs;
    for (const auto& p : opts["pictures"].toList()) {
        inputs << p.toMap()["id"].toInt();
        pictures << editor->current()->dir.filePath(editor->plan()->item("reference", inputs.last())["file"].toString());
    }
    const auto shot = editor->plan()->shot(shotId);
    const qint64 first = seed >= 0 ? seed : qint64(QRandomGenerator::global()->bounded(quint64(1) << 48));
    QList<int> made;
    for (int i = 0; i < count; ++i) {
        Job job;
        job.id = ++serial;
        job.owner = "shot";
        job.ownerId = shotId;
        job.use = opts["use"].toString();
        job.workflow = workflow;
        job.workflowName = flows->nameOf(workflow);
        job.prompt = prompt;
        job.inputs = inputs;
        job.hasSeed = flows->hasSlot(workflow, "seed");
        job.seed = seed >= 0 ? std::min(kMaxSeed, first + i) : i == 0 ? first : qint64(QRandomGenerator::global()->bounded(quint64(1) << 48));
        job.settings = used;
        job.shown = shown;
        job.aspectChosen = aspectChosen;
        job.locked = job.use == "take-sync";
        job.handles = o.contains("handles") ? o.value("handles").toDouble() : kDefaultHandles;
        job.takeFps = opts["fps"].toDouble();
        job.anchor = shot["start"].toInt();
        list.append(job);
        say(list.last(), "uploading", "Preparing…");
        made << job.id;
    }
    // The length waits for ComfyUI's node details (its frame grid); then the excerpt, uploads, queue.
    withInfo(workflow, [this, made, pictures] { for (int id : made) startTake(id, pictures); });
    return {};
}

void Generator::startTake(int id, const QStringList& pictures) {
    Job* job = find(id);
    if (!job) return;
    const auto shot = editor->plan()->shot(job->ownerId);
    if (shot.isEmpty()) { failJob(id, "The shot was removed."); return; }
    const double seconds = (shot["end"].toInt() - shot["start"].toInt()) / editor->fps();
    QString why;
    job->frames = takeFrames(job->workflow, seconds, job->takeFps, job->handles, why);
    if (!job->frames) { failJob(id, why); return; }
    // The take starts its handle before the shot; any rounding onto the grid goes at its end.
    job->anchor = shot["start"].toInt() - int(std::lround(job->handles * editor->fps()));
    QStringList files = pictures;
    if (job->use == "take-sync") {
        const QString folder = editor->current()->dir.filePath("Jobs");
        QDir().mkpath(folder);
        job->excerpt = QDir(folder).filePath(QString("excerpt-shot%1-%2.wav").arg(job->ownerId).arg(job->anchor));
        const QString problem = editor->writeExcerpt(job->anchor / editor->fps(), job->frames / job->takeFps, job->excerpt);
        if (!problem.isEmpty()) { failJob(id, problem); return; }
        files << job->excerpt;
    }
    say(*job, "uploading", files.isEmpty() ? "Preparing…" : job->use == "take-sync" ? "Uploading pictures and the song…" : "Uploading pictures…");
    upload(id, files, {});
}

void Generator::collectTake(int id, const QJsonObject& outputs) {
    Job* job = find(id);
    if (!job) return;
    // The saved video: Save Video lists it with the pictures, Video Combine as "gifs".
    QJsonObject file;
    const QJsonArray wanted = editor->workflows()->outputs(job->workflow);
    static const QStringList videoTypes{"mp4", "webm", "mov", "mkv", "m4v", "gif", "webp"};
    for (auto it = outputs.begin(); it != outputs.end() && file.isEmpty(); ++it) {
        if (!wanted.isEmpty() && !wanted.contains(it.key())) continue;
        for (const char* key : {"images", "gifs", "videos"})
            for (const auto& v : it.value().toObject()[key].toArray())
                if (file.isEmpty() && videoTypes.contains(QFileInfo(v.toObject()["filename"].toString()).suffix().toLower())) file = v.toObject();
    }
    if (file.isEmpty()) { failJob(id, "ComfyUI finished without a video. Check the workflow's Save Video node."); return; }
    say(*job, "collecting", "Downloading the take…");
    QUrlQuery query;
    query.addQueryItem("filename", file["filename"].toString());
    query.addQueryItem("subfolder", file["subfolder"].toString());
    query.addQueryItem("type", file["type"].toString());
    auto reply = net.get(editor->comfy()->request("/view?" + query.toString(QUrl::FullyEncoded)));
    const QString folder = editor->current()->dir.filePath("Jobs");
    QDir().mkpath(folder);
    const QString path = QDir(folder).filePath(job->promptId + "." + QFileInfo(file["filename"].toString()).suffix().toLower());
    const int mine = epoch;
    connect(reply, &QNetworkReply::finished, this, [this, reply, id, path, mine, outputs] {
        reply->deleteLater();
        if (mine != epoch) return;
        Job* job = find(id);
        if (!job) return;
        QFile out(path);
        if (reply->error() != QNetworkReply::NoError) { failJob(id, "Could not download the take: " + ComfyJobs::errorOf(reply)); return; }
        if (!out.open(QIODevice::WriteOnly) || out.write(reply->readAll()) < 0) { failJob(id, "Could not save the take in the project."); return; }
        out.close();
        // A face refine's picture of the faces found comes down too, when the graph made one.
        QJsonObject preview;
        if (!job->previewNode.isEmpty()) preview = outputs[job->previewNode].toObject()["images"].toArray().first().toObject();
        if (preview.isEmpty()) { addTake(id, path, {}); return; }
        QUrlQuery q;
        q.addQueryItem("filename", preview["filename"].toString());
        q.addQueryItem("subfolder", preview["subfolder"].toString());
        q.addQueryItem("type", preview["type"].toString());
        auto shot = net.get(editor->comfy()->request("/view?" + q.toString(QUrl::FullyEncoded)));
        const QString faces = path + "-faces." + (QFileInfo(preview["filename"].toString()).suffix().isEmpty() ? QString("png") : QFileInfo(preview["filename"].toString()).suffix().toLower());
        connect(shot, &QNetworkReply::finished, this, [this, shot, id, path, faces, mine] {
            shot->deleteLater();
            if (mine != epoch || !find(id)) return;
            QFile f(faces);
            const bool saved = shot->error() == QNetworkReply::NoError && f.open(QIODevice::WriteOnly) && f.write(shot->readAll()) >= 0;
            f.close();
            addTake(id, path, saved ? faces : QString());
        });
    });
}

void Generator::addTake(int id, const QString& path, const QString& faces) {
    Job* job = find(id);
    if (!job) return;
    auto plan = editor->plan();
    if (plan->shot(job->ownerId).isEmpty()) { failJob(id, "The shot was removed before its take arrived; it is in the project's Jobs folder."); return; }
    // What arrived, measured: a workflow may round the length its own way.
    const QString thumb = path + ".jpg";
    const auto probe = Editor::probeVideo(path, thumb);
    if (probe.isEmpty()) { failJob(id, "The take ComfyUI made can't be read."); return; }
    const bool face = job->use == "take-face";
    if (job->use == "take-upscale") {
        // An upscaled version must have every frame of its take, and its shape.
        const double want = job->height ? double(job->width) / job->height : 0, got = probe["height"].toInt() ? probe["width"].toDouble() / probe["height"].toDouble() : 0;
        // Its shape within 3%: the upscaler rounds each side up to a step of 32.
        if (probe["frames"].toInt() != job->frames || want <= 0 || std::abs(got / want - 1) > 0.03) {
            failJob(id, QString("The upscaled take is %1 × %2, %3 frames; the take is %4 × %5, %6 frames, so it would not replace it frame for frame. It is in the project's Jobs folder.")
                            .arg(probe["width"].toInt()).arg(probe["height"].toInt()).arg(probe["frames"].toInt()).arg(job->width).arg(job->height).arg(job->frames));
            return;
        }
        QVariantMap generation{{"workflow", job->workflow}, {"workflowName", job->workflowName}, {"use", job->use}, {"prompt", job->prompt}, {"promptId", job->promptId}};
        if (job->hasSeed) generation["seed"] = double(job->seed);
        if (!job->shown.isEmpty()) generation["settings"] = job->shown.toVariantList();
        QVariantList inputs;
        for (int i : job->inputs) inputs << i;
        generation["inputs"] = inputs;
        const auto r = plan->run({{"op", "take.upscale"}, {"id", job->source}, {"path", path}, {"thumb", thumb}, {"width", probe["width"]}, {"height", probe["height"]},
                                  {"frames", probe["frames"]}, {"generation", generation}});
        if (!r.value("ok").toBool()) { failJob(id, r.value("error").toString()); return; }
        QFile::remove(path);
        QFile::remove(thumb);
        if (!job->excerpt.isEmpty()) QFile::remove(job->excerpt);
        record(*job, "done");
        const int ownerId = job->ownerId, source = job->source;
        for (int i = 0; i < list.size(); ++i) if (list[i].id == id) { list.removeAt(i); break; }
        const bool wasWatched = watched == id;
        emit changed();
        if (wasWatched) emit watchedArrived(source, true);
        emit finished("shot", ownerId, "take-upscale", true, {}, 1);
        return;
    }
    // A refined take must fit exactly where its source plays.
    if (face && (probe["frames"].toInt() != job->frames || probe["width"].toInt() != job->width || probe["height"].toInt() != job->height)) {
        failJob(id, QString("The refined take is %1 × %2, %3 frames; the take it came from is %4 × %5, %6 frames, so it would not fit in its place. It is in the project's Jobs folder.")
                        .arg(probe["width"].toInt()).arg(probe["height"].toInt()).arg(probe["frames"].toInt()).arg(job->width).arg(job->height).arg(job->frames));
        return;
    }
    QVariantMap generation{{"workflow", job->workflow}, {"workflowName", job->workflowName}, {"use", job->use},
                           {"prompt", job->prompt}, {"promptId", job->promptId}, {"handles", job->handles},
                           {"songFrom", job->anchor / editor->fps()}, {"songSeconds", job->frames / job->takeFps}};
    if (job->hasSeed) generation["seed"] = double(job->seed);
    if (!job->shown.isEmpty()) generation["settings"] = job->shown.toVariantList();
    QVariantList inputs;
    for (int i : job->inputs) inputs << i;
    generation["inputs"] = inputs;
    if (face) {
        generation["source"] = job->source; generation["character"] = job->character; generation["face"] = job->face;
        // Made at the upscaled size: from an upscaled version, or from a take that already was.
        generation["fromUpscaled"] = job->fromUpscaled || plan->item("take", job->source)["generation"].toMap()["fromUpscaled"].toBool();
    }
    const double rate = probe["fps"].toDouble() > 0 ? probe["fps"].toDouble() : job->takeFps;
    const auto r = plan->run({{"op", "take.add"}, {"shot", job->ownerId}, {"path", path}, {"thumb", thumb}, {"faces", faces},
                              {"frames", probe["frames"].toInt() > 0 ? probe["frames"].toInt() : job->frames}, {"fps", rate},
                              {"anchor", job->anchor}, {"locked", job->locked}, {"width", probe["width"]}, {"height", probe["height"]},
                              {"generation", generation}});
    if (!r.value("ok").toBool()) { failJob(id, r.value("error").toString()); return; }
    QFile::remove(path);
    QFile::remove(thumb);
    if (!faces.isEmpty()) QFile::remove(faces);
    if (!job->excerpt.isEmpty()) QFile::remove(job->excerpt);
    record(*job, "done");
    const QString owner = job->owner, use = job->use, workflow = job->workflow;
    const int ownerId = job->ownerId, made = r.value("id").toInt();
    for (int i = 0; i < list.size(); ++i) if (list[i].id == id) { list.removeAt(i); break; }
    // The next face pass refines this take.
    QList<int> next;
    for (auto& j : list) if (j.after == id) { j.after = 0; j.source = made; next << j.id; say(j, "uploading", "Preparing…"); }
    const bool wasWatched = watched == id;
    emit changed();
    emit takeArrived(ownerId, made);
    if (wasWatched) emit watchedArrived(made, false);
    emit finished(owner, ownerId, use, true, {}, 1);
    for (int n : next) withInfo(workflow, [this, n] { startRefine(n); });
}

QString Generator::composeSheet(int view, const QVariantList& panels) {
    auto plan = editor->plan();
    auto project = editor->current();
    const auto v = plan->item("view", view);
    if (!project || v.isEmpty()) return "That view is gone.";
    QList<QImage> images;
    QVariantList from, panelIds;
    for (const auto& p : panels) {
        const auto pv = plan->item("view", p.toInt());
        if (pv.isEmpty() || p.toInt() == view) continue;
        QVariantMap chosen;
        for (const auto& x : plan->viewsFor(pv["owner"].toString(), pv["ownerId"].toInt()))
            if (x.toMap()["id"].toInt() == p.toInt()) chosen = x.toMap()["chosen"].toMap();
        const QString name = pv["fields"].toMap()["name"].toString();
        if (chosen.isEmpty()) return name + " has no chosen picture yet.";
        QImageReader reader(project->dir.filePath(chosen["file"].toString()));
        reader.setAutoTransform(true);
        QImage image = reader.read();
        if (image.isNull()) return "Cannot read the chosen picture of " + name + ".";
        images << image;
        from << chosen["id"].toInt();
        panelIds << p.toInt();
    }
    if (images.size() < 2) return "Tick at least two views with chosen pictures.";
    // One row at a shared height, no gaps or labels (text can turn up in the video); kept to a
    // size image and video models take.
    int height = 1536;
    for (const auto& i : images) height = std::min(height, i.height());
    height = std::max(height, 512);
    auto widthAt = [&](int h) { int w = 0; for (const auto& i : images) w += int(std::lround(double(i.width()) * h / i.height())); return w; };
    while (widthAt(height) > 6144 && height > 256) height = height * 9 / 10;
    QImage sheet(widthAt(height), height, QImage::Format_RGB32);
    sheet.fill(Qt::white);
    QPainter painter(&sheet);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    int x = 0;
    for (const auto& i : images) {
        const int w = int(std::lround(double(i.width()) * height / i.height()));
        painter.drawImage(QRect(x, 0, w, height), i);
        x += w;
    }
    painter.end();
    QDir().mkpath(project->dir.filePath("Jobs"));
    const QString path = project->dir.filePath(QString("Jobs/sheet-%1.png").arg(QDateTime::currentMSecsSinceEpoch()));
    if (!sheet.save(path)) return "Could not save the sheet.";
    const QString name = v["fields"].toMap()["name"].toString();
    const auto r = plan->run({{"op", "batch"}, {"label", "Compose " + name}, {"commands", QVariantList{
        QVariantMap{{"op", "set"}, {"kind", "view"}, {"id", view}, {"fields", QVariantMap{{"panels", panelIds}}}},
        QVariantMap{{"op", "ref.add"}, {"owner", v["owner"]}, {"ownerId", v["ownerId"]}, {"view", view}, {"path", path}, {"source", "composed"},
                    {"name", name + " (composed)"}, {"generation", QVariantMap{{"composedFrom", from}}}}}}});
    QFile::remove(path);
    return r.value("ok").toBool() ? QString() : r.value("error").toString();
}

void Generator::withInfo(const QString& workflow, std::function<void()> then) {
    fetchChoices(workflow);
    const QString server = editor->comfy()->url();
    bool pending = false;
    for (const QString& cls : editor->workflows()->classes(workflow)) pending |= waiting.contains(server + "|" + cls);
    if (!pending) { then(); return; }
    afterInfo.append(then);
}

bool Generator::infoKnown(const QString& workflow) const {
    const QString server = editor->comfy()->url();
    for (const QString& cls : editor->workflows()->classes(workflow)) if (!info.contains(server + "|" + cls)) return false;
    return true;
}

Workflows::OptionalInputs Generator::optionalFor(const QString& workflow) const {
    Workflows::OptionalInputs out;
    const QString server = editor->comfy()->url();
    for (const QString& cls : editor->workflows()->classes(workflow))
        if (info.contains(server + "|" + cls)) out[cls] = info.value(server + "|" + cls).optional;
    return out;
}

void Generator::fetchChoices(const QString& workflow) {
    // Each node type is asked once per server: ComfyUI's /object_info lists its inputs (which are
    // optional, what a combo offers) and describes it.
    const QString server = editor->comfy()->url();
    for (const QString& cls : editor->workflows()->classes(workflow)) {
        const QString key = server + "|" + cls;
        if (info.contains(key) || waiting.contains(key)) continue;
        waiting[key] = 1;
        auto reply = net.get(editor->comfy()->request("/object_info/" + QString::fromUtf8(QUrl::toPercentEncoding(cls))));
        connect(reply, &QNetworkReply::finished, this, [this, reply, cls, key] {
            reply->deleteLater();
            waiting.remove(key);
            if (reply->error() == QNetworkReply::NoError) {
                const auto node = QJsonDocument::fromJson(reply->readAll()).object()[cls].toObject();
                const auto spec = node["input"].toObject();
                NodeInfo ni;
                ni.text = node["description"].toString();
                bool any = false;
                // Inputs a mode choice adds are named "mode.sub" in the graph: optional ones count too.
                std::function<void(const QString&, const QJsonObject&)> nested = [&](const QString& prefix, const QJsonObject& groups) {
                    for (const char* group : {"required", "optional"}) {
                        const auto inputs = groups[group].toObject();
                        for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                            const QString name = prefix + it.key();
                            if (QString(group) == "optional") ni.optional.insert(name);
                            const auto def = it.value().toArray();
                            ni.text += " " + def.at(1).toObject()["tooltip"].toString();
                            if (def.at(0).toString() == "COMFY_DYNAMICCOMBO_V3")
                                for (const auto& option : def.at(1).toObject()["options"].toArray())
                                    nested(name + ".", option.toObject()["inputs"].toObject());
                        }
                    }
                };
                nested({}, spec);
                for (const char* group : {"required", "optional"}) {
                    const auto inputs = spec[group].toObject();
                    for (auto it = inputs.begin(); it != inputs.end(); ++it) {
                        const auto def = it.value().toArray();
                        ni.text += " " + def.at(1).toObject()["tooltip"].toString();
                        // A video's length and the grid it snaps to.
                        if ((it.key() == "length" || it.key() == "num_frames" || it.key() == "video_length" || it.key() == "frames") && def.at(0).toString() == "INT") {
                            const auto o = def.at(1).toObject();
                            ni.lengthMin = o["min"].toInt(1);
                            ni.lengthStep = qMax(1, o["step"].toInt(1));
                            ni.lengthMax = o["max"].toInt(0);
                        }
                        // A growing list's template describes each entry.
                        ni.text += " " + QString::fromUtf8(QJsonDocument(def.at(1).toObject()["template"].toObject()).toJson(QJsonDocument::Compact));
                        // A combo input is [[choice, ...], {...}]; newer servers write ["COMBO", {options: [...]}].
                        QJsonArray values = def.at(0).toArray();
                        if (values.isEmpty() && def.at(0).toString() == "COMBO") values = def.at(1).toObject()["options"].toArray();
                        QVariantList choices;
                        for (const auto& c : values) if (c.isString()) choices << c.toString();
                        if (!choices.isEmpty()) { choiceMap[cls + "/" + it.key()] = choices; any = true; }
                    }
                }
                info[key] = ni;
                if (any) emit choicesChanged();
                emit changed();
            }
            // Jobs waiting for these details go on once nothing is being fetched.
            if (waiting.isEmpty()) {
                const auto calls = afterInfo;
                afterInfo.clear();
                for (const auto& call : calls) call();
            }
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
    // A workflow shaped by an aspect-ratio choice rather than width and height: the choice that
    // matches, 3:4 for people and looks, the video's aspect for places, unless the user chose.
    if (!job->aspectChosen) {
        for (const auto& sv : flows->settings(job->workflow)) {
            const QString key = sv.toMap()["key"].toString();
            const QString choice = autoAspect(job->owner, job->workflow, key);
            if (choice.isEmpty()) continue;
            job->settings[key] = choice;
            for (int i = 0; i < job->shown.size(); ++i)
                if (job->shown[i].toObject()["key"].toString() == key) { auto o = job->shown[i].toObject(); o["value"] = choice; job->shown[i] = o; }
        }
    }
    v.settings = job->settings;
    v.optional = optionalFor(job->workflow);
    v.prefix = "ChorusCut/reference";
    if (job->use == "take-face" || job->use == "take-upscale") {
        // Uploaded as: the take, the face to find (a refine), the pictures, the sound.
        QStringList rest = names;
        v.video = rest.takeFirst();
        if (job->use == "take-face") v.face = rest.takeFirst();
        v.audio = rest.takeLast();
        v.pictures = rest;
        v.prefix = job->use == "take-face" ? "ChorusCut/face" : "ChorusCut/upscale";
    } else if (job->owner == "shot") {
        // The song excerpt goes up last, after the pictures.
        if (job->use == "take-sync" && !v.pictures.isEmpty()) v.audio = v.pictures.takeLast();
        v.frames = job->frames;
        v.fps = job->takeFps;
        v.prefix = "ChorusCut/take";
    }
    // Locations take the video's frame; people and looks a portrait; props a square. The workflow's own size
    // sets the scale its model prefers.
    const QSize base = flows->size(job->workflow);
    if (base.width() > 0 && base.height() > 0 && job->use != "take-face" && job->use != "take-upscale") {
        double ratio = job->owner == "prop" ? 1.0 : 3.0 / 4.0;
        if (job->owner == "location" || job->owner == "shot") {
            const QString aspect = editor->plan()->concept()["fields"].toMap()["aspect"].toString();
            const auto parts = aspect.split(':');
            ratio = parts.size() == 2 && parts[1].toDouble() > 0 ? parts[0].toDouble() / parts[1].toDouble() : 16.0 / 9.0;
        }
        const double longSide = std::max(base.width(), base.height());
        v.width = round16(ratio >= 1 ? longSide : longSide * ratio);
        v.height = round16(ratio >= 1 ? longSide / ratio : longSide);
    }
    QString error;
    QJsonObject graph = flows->build(job->workflow, v, error);
    if (graph.isEmpty()) { failJob(id, error); return; }
    // A face refine also brings back the select node's numbered picture of the faces it found,
    // the one it refined outlined, so who was matched can be checked.
    if (job->use == "take-face") {
        int next = 0;
        for (const QString& k : graph.keys()) next = qMax(next, k.toInt());
        for (const QString& node : graph.keys())
            if (graph[node].toObject()["class_type"] == "H3FaceSelect") {
                job->previewNode = QString::number(next + 1);
                graph[job->previewNode] = QJsonObject{{"class_type", "PreviewImage"}, {"inputs", QJsonObject{{"images", QJsonArray{node, 3}}}},
                                                      {"_meta", QJsonObject{{"title", "ChorusCut: faces found"}}}};
                break;
            }
    }
    addLiveNode(*job, graph);
    job->graph = graph;
    say(*job, "queued", "Queueing…");
    listen();  // before queueing, so no preview is missed
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
                                 {"settings", job->shown}, {"graph", job->graph}, {"view", job->view}, {"instruction", job->instruction},
                                 {"inputs", [&] { QJsonArray a; for (int i : job->inputs) a.append(i); return a; }()},
                                 {"frames", job->frames}, {"takeFps", job->takeFps}, {"anchor", job->anchor}, {"locked", job->locked},
                                 {"handles", job->handles}, {"source", job->source}, {"character", job->character}, {"face", job->face},
                                 {"what", job->what}, {"width", job->width}, {"height", job->height}, {"previewNode", job->previewNode},
                                 {"fromUpscaled", job->fromUpscaled}, {"liveNode", job->liveNode}};
        for (const QVariant& value : {QVariant(job->promptId), QVariant(job->owner == "shot" ? "take" : "reference"), QVariant(editor->comfy()->url()), QVariant(""), QVariant(""),
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
    listen();
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
                if (!job->liveStep) say(*job, "running", "Generating…");
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
    if (job->owner == "shot") { collectTake(id, outputs); return; }
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
            generation["instruction"] = job->instruction;
            QVariantList inputs;
            for (int i : job->inputs) inputs << i;
            if (!inputs.isEmpty()) generation["inputs"] = inputs;
            for (const QString& f : *files)
                commands.append(QVariantMap{{"op", "ref.add"}, {"owner", job->owner}, {"ownerId", job->ownerId}, {"path", f}, {"view", plan->item("view", job->view).isEmpty() ? 0 : job->view},
                                            {"source", "generated"}, {"name", "Generated with " + job->workflowName}, {"generation", generation}});
            const auto r = plan->run(commands.size() == 1 ? commands[0].toMap()
                                     : QVariantMap{{"op", "batch"}, {"label", QString("Add %1 generated references").arg(commands.size())}, {"commands", commands}});
            if (!r.value("ok").toBool()) { failJob(id, r.value("error").toString()); return; }
            for (const QString& f : *files) QFile::remove(f);  // copied into Media/References
            record(*job, "done");
            const QString owner = job->owner, use = job->use;
            const int ownerId = job->ownerId, count = files->size();
            for (int i = 0; i < list.size(); ++i) if (list[i].id == id) { list.removeAt(i); break; }
            emit changed();
            emit finished(owner, ownerId, use, true, {}, count);
        });
    }
}

void Generator::failJob(int id, const QString& why) {
    Job* job = find(id);
    if (!job) return;
    if (!job->promptId.isEmpty()) record(*job, "failed", why);
    const bool fresh = job->state != "failed";
    const QString what = job->what;
    say(*job, "failed", why);
    if (fresh) emit finished(job->owner, job->ownerId, job->use, false, why, 0);
    // Face passes waiting on this one can't run.
    QList<int> next;
    for (const auto& j : list) if (j.after == id && j.state != "failed") next << j.id;
    for (int n : next) failJob(n, what + " failed, so this pass didn't run.");
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
    // Face passes waiting on this one go with it.
    QList<int> next;
    for (const auto& j : list) if (j.after == id) next << j.id;
    for (int i = 0; i < list.size(); ++i)
        if (list[i].id == id) { list.removeAt(i); emit changed(); break; }
    for (int n : next) dismiss(n);
}

void Generator::retry(int id) {
    Job* job = find(id);
    if (!job) return;
    const QString owner = job->owner, use = job->use;
    const int ownerId = job->ownerId;
    // The same request again: same workflow, seed and settings.
    QVariantMap settings;
    for (const auto& v : job->shown) settings[v.toObject()["key"].toString()] = v.toObject()["value"].toVariant();
    QVariantList inputs;
    for (int i : job->inputs) inputs << i;
    if (job->use == "take-upscale") {
        const QString why = upscale({job->source}, {{"workflow", job->workflow}, {"seed", QString::number(job->seed)}, {"settings", settings}});
        if (why.isEmpty()) dismiss(id); else say(*find(id), "failed", why);
        return;
    }
    if (job->use == "take-face") {
        // The same pass again, on the take it refined (or was to refine).
        const int source = job->source ? job->source : 0;
        QVariantList pictures;
        for (int i : job->inputs) pictures << i;
        const QString why = source ? refineFaces(source, {QVariantMap{{"character", job->character}, {"face", job->face}, {"pictures", pictures}, {"prompt", job->prompt}}},
                                                 {{"workflow", job->workflow}, {"seed", QString::number(job->seed)}, {"settings", settings}, {"fromUpscaled", job->fromUpscaled}})
                                   : "The take this pass was to refine was never made.";
        if (why.isEmpty()) dismiss(id); else say(*find(id), "failed", why);
        return;
    }
    if (job->owner == "shot") {
        const QString why = generateTake(ownerId, {{"workflow", job->workflow}, {"seed", QString::number(job->seed)}, {"settings", settings},
                                                   {"prompt", job->prompt}, {"handles", job->handles}, {"aspectChosen", true}});
        if (why.isEmpty()) dismiss(id); else say(*find(id), "failed", why);
        return;
    }
    QVariantMap again{{"workflow", job->workflow}, {"seed", QString::number(job->seed)}, {"settings", settings}, {"view", job->view},
                      {"instruction", job->instruction}};
    if (job->use == "ref-edit") again["inputs"] = inputs;
    const QString why = generateWith(owner, ownerId, use, again);
    if (why.isEmpty()) dismiss(id);
    else say(*find(id), "failed", why);
}

void Generator::resume() {
    detach();
    auto project = editor->current();
    if (!project || !project->db.tables().contains("jobs") || !ensureTable()) return;
    QSqlQuery q(project->db);
    if (!q.exec("SELECT prompt_id, server, detail, state, submitted, client_id FROM jobs WHERE kind IN ('reference','take') AND state IN ('queued','running') ORDER BY submitted"))
        return;
    while (q.next()) {
        const auto d = QJsonDocument::fromJson(q.value(2).toString().toUtf8()).object();
        Job job;
        job.id = ++serial;
        job.promptId = q.value(0).toString();
        job.started = q.value(4).toLongLong() * 1000;
        job.since = QDateTime::currentMSecsSinceEpoch();
        job.owner = d["owner"].toString();
        job.ownerId = d["ownerId"].toInt();
        job.use = d["use"].toString();
        job.workflow = d["workflow"].toString();
        job.workflowName = d["workflowName"].toString();
        job.prompt = d["prompt"].toString();
        job.seed = qint64(d["seed"].toDouble());
        job.hasSeed = d["hasSeed"].toBool(true);
        job.view = d["view"].toInt();
        job.instruction = d["instruction"].toString();
        for (const auto& i : d["inputs"].toArray()) job.inputs << i.toInt();
        job.shown = d["settings"].toArray();
        for (const auto& v : job.shown) job.settings[v.toObject()["key"].toString()] = v.toObject()["value"];
        job.graph = d["graph"].toObject();
        job.frames = d["frames"].toInt();
        job.takeFps = d["takeFps"].toDouble(24);
        job.anchor = d["anchor"].toInt();
        job.locked = d["locked"].toBool();
        job.handles = d["handles"].toDouble(0.5);
        job.source = d["source"].toInt();
        job.character = d["character"].toInt();
        job.face = d["face"].toInt();
        job.what = d["what"].toString();
        job.width = d["width"].toInt();
        job.height = d["height"].toInt();
        job.previewNode = d["previewNode"].toString();
        job.fromUpscaled = d["fromUpscaled"].toBool();
        // Previews go to the client ID that queued the job, so a reopened project listens as it.
        job.liveNode = d["liveNode"].toString();
        job.client = q.value(5).toString();
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
    for (auto socket : sockets) { socket->disconnect(this); socket->abort(); socket->deleteLater(); }
    sockets.clear();
    runningPrompt.clear();
    if (list.isEmpty()) return;
    list.clear();
    emit changed();
}
