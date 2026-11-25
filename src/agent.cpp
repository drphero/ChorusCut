#include "agent.h"
#include "agent_tools.h"
#include "editor.h"
#include <QBuffer>
#include <QClipboard>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSaveFile>
#include <cmath>

namespace {
QJsonObject text(const QJsonObject& value) {
    return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact))}}}},
            {"isError", false}};
}
QJsonObject failure(const QString& message) {
    return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", message}}}}, {"isError", true}};
}
QJsonObject json(const QVariant& v) { return QJsonObject::fromVariantMap(v.toMap()); }
QString quoted(const QString& path) { return "\"" + QString(path).replace("\"", "\\\"") + "\""; }

// Antigravity's config: an absent or empty file is an empty object; anything else must parse to
// an object whose mcpServers, if present, is an object. ok is false when it does not.
QJsonObject readMcpConfig(const QString& path, bool* ok) {
    *ok = true;
    QFile file(path);
    if (!file.exists()) return {};
    if (!file.open(QIODevice::ReadOnly)) { *ok = false; return {}; }
    const QByteArray bytes = file.readAll().trimmed();
    if (bytes.isEmpty()) return {};
    QJsonParseError error;
    const auto doc = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) { *ok = false; return {}; }
    const auto config = doc.object();
    if (config.contains("mcpServers") && !config["mcpServers"].isObject()) *ok = false;
    return config;
}
}

AgentBridge::AgentBridge(Editor* editor, QObject* parent) : QObject(parent), editor(editor) {
    connect(&server, &QLocalServer::newConnection, this, &AgentBridge::accept);
    connect(editor->requests(), &Requests::posted, this, [this, editor](const QJsonObject& event) {
        editor->requests()->markSent(event["thread"].toInt(), push(event));
    });
}

// The agents' sockets are deleted only after this body, once the members are gone, and closing
// one emits disconnected, whose handler edits peers. That crashed every quit with an agent
// connected. Drop them here, silently, while everything is still alive.
AgentBridge::~AgentBridge() {
    server.close();
    const auto sockets = peers.keys();
    peers.clear();
    for (QLocalSocket* socket : sockets) {
        socket->disconnect(this);
        delete socket;
    }
}

bool AgentBridge::liveClient() const {
    for (const QString& name : peers) if (name.contains("claude", Qt::CaseInsensitive)) return true;
    return false;
}

bool AgentBridge::push(const QJsonObject& event) {
    const int thread = event["thread"].toInt();
    const auto about = event["about"].toObject();
    const QString label = event["label"].toString();
    QString content;
    if (event["kind"] == "answer")
        content = QString("The user answered your question about %1 (request %2): %3").arg(label).arg(thread).arg(event["text"].toString());
    else
        content = QString("The user asks, about %1 (request %2): %3").arg(label).arg(thread).arg(event["text"].toString());
    QJsonObject meta{{"request_id", QString::number(thread)}};
    if (!about["kind"].toString().isEmpty()) {
        meta["about_kind"] = about["kind"].toString();
        meta["about_id"] = QString::number(about["id"].toInt());
        if (!about["field"].toString().isEmpty()) meta["field"] = about["field"].toString();
    }
    const QByteArray line = QJsonDocument(QJsonObject{{"event", QJsonObject{{"content", content}, {"meta", meta}}}}).toJson(QJsonDocument::Compact) + '\n';
    bool reached = false;
    for (auto it = peers.begin(); it != peers.end(); ++it) {
        if (!it.value().contains("claude", Qt::CaseInsensitive)) continue;
        it.key()->write(line);
        it.key()->flush();
        reached = true;
    }
    return reached;
}

bool AgentBridge::start() {
    const QString name = AgentTools::socketName();
    server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!server.listen(name)) {
        // A socket left by a crash, unless another ChorusCut is answering on it.
        QLocalSocket probe;
        probe.connectToServer(name);
        if (probe.waitForConnected(300)) {
            notice = "Another ChorusCut window is already serving agents.";
            emit stateChanged();
            return false;
        }
        QLocalServer::removeServer(name);
        if (!server.listen(name)) {
            notice = "Agents cannot connect: " + server.errorString();
            emit stateChanged();
            return false;
        }
    }
    notice.clear();
    emit stateChanged();
    return true;
}

void AgentBridge::stop() {
    server.close();
    for (QLocalSocket* socket : peers.keys()) socket->abort();
    emit stateChanged();
}

QStringList AgentBridge::clients() const { return peers.values(); }

QString AgentBridge::shimPath() const {
#ifdef Q_OS_WIN
    const QString exe = QStringLiteral(".exe");
#else
    const QString exe;
#endif
    return QDir(QCoreApplication::applicationDirPath()).absoluteFilePath(QStringLiteral("choruscut-mcp") + exe);
}
QString AgentBridge::claudeCommand() const { return "claude mcp add --scope user ChorusCut -- " + quoted(shimPath()); }
QString AgentBridge::codexCommand() const { return "codex mcp add ChorusCut -- " + quoted(shimPath()); }
QString AgentBridge::antigravityConfig() const {
    const QString custom = qEnvironmentVariable("CHORUS_ANTIGRAVITY_CONFIG");
    return custom.isEmpty() ? QDir::home().filePath(".gemini/config/mcp_config.json") : custom;
}

QString AgentBridge::antigravityState() const {
    bool ok;
    const auto config = readMcpConfig(antigravityConfig(), &ok);
    if (!ok) return "unreadable";
    const QJsonValue entry = config["mcpServers"].toObject().value("ChorusCut");
    if (entry.isUndefined()) return "missing";
    return entry.toObject()["command"].toString() == shimPath() ? "added" : "other";
}

bool AgentBridge::addToAntigravity() {
    const QString path = antigravityConfig();
    bool ok;
    QJsonObject config = readMcpConfig(path, &ok);
    if (!ok) { emit antigravityChanged(); return false; }
    QJsonObject servers = config["mcpServers"].toObject();
    servers["ChorusCut"] = QJsonObject{{"command", shimPath()}};
    config["mcpServers"] = servers;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    const bool written = file.open(QIODevice::WriteOnly) && file.write(QJsonDocument(config).toJson(QJsonDocument::Indented)) >= 0 && file.commit();
    emit antigravityChanged();
    return written;
}

void AgentBridge::copy(const QString& value) const { if (auto board = QGuiApplication::clipboard()) board->setText(value); }

void AgentBridge::accept() {
    while (QLocalSocket* socket = server.nextPendingConnection()) {
        peers.insert(socket, "Agent");
        connect(socket, &QLocalSocket::readyRead, this, [this, socket] { read(socket); });
        connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
            peers.remove(socket);
            socket->deleteLater();
            emit stateChanged();
        });
        emit stateChanged();
    }
}

void AgentBridge::read(QLocalSocket* socket) {
    while (socket->canReadLine()) {
        const auto message = QJsonDocument::fromJson(socket->readLine()).object();
        if (message.contains("hello")) {
            peers[socket] = message.value("hello").toString("Agent");
            emit stateChanged();
            continue;
        }
        if (!message.contains("tool")) continue;
        currentClient = peers.value(socket);
        const QJsonObject result = call(message.value("tool").toString(), message.value("arguments").toObject());
        socket->write(QJsonDocument(QJsonObject{{"id", message.value("id")}, {"result", result}}).toJson(QJsonDocument::Compact) + '\n');
        socket->flush();
    }
}

QJsonObject AgentBridge::call(const QString& tool, const QJsonObject& arguments) {
    if (tool == "get_guide")
        return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", AgentTools::guide()}}}}, {"isError", false}};
    if (tool == "get_project") return text(project());
    if (!editor->ready()) return failure("No project is open in ChorusCut. Ask the user to open one.");
    if (tool == "get_song") return text(song(arguments.value("include_words").toBool()));
    if (tool == "get_plan") return text(planView());
    if (tool == "get_checks") return text(checks());
    if (tool == "get_schema") return text(schema());
    if (tool == "view_reference") return reference(arguments.value("id").toInt());
    if (tool == "apply_plan") return apply(arguments);
    if (tool == "get_requests") return requestsTool(arguments);
    if (tool == "reply_request") return replyTool(arguments);
    if (tool == "ask_user") return askTool(arguments);
    return failure("Unknown tool: " + tool);
}

QJsonObject AgentBridge::project() const {
    if (!editor->ready()) return {{"open", false}, {"message", "No project is open in ChorusCut. Ask the user to open or create one."}};
    Plan* plan = editor->plan();
    Lyrics* lyrics = editor->lyrics();
    const int lines = lyrics->lines().size();
    QJsonArray gates;
    for (const auto& g : plan->gates()) gates.append(json(g));
    return {{"open", true}, {"name", editor->projectName()}, {"folder", editor->projectPath()},
            {"fps", editor->fps()}, {"frames", editor->length()}, {"durationMs", qRound(1000.0 * editor->length() / editor->fps())},
            {"lyrics", QJsonObject{{"lines", lines}, {"aligned", lines > 0}, {"hasText", !lyrics->text().trimmed().isEmpty()},
                                   {"flaggedLines", lyrics->flaggedLines()}}},
            {"plan", QJsonObject{{"characters", plan->characters().size()}, {"locations", plan->locations().size()}, {"props", plan->props().size()},
                                 {"shots", plan->shots().size()}, {"references", plan->references().size()}}},
            {"gates", gates}, {"openChecks", plan->errorCount()},
            {"requests", QJsonObject{{"waitingForYou", editor->requests()->waitingOnAgent()}, {"waitingForUser", editor->requests()->waitingOnUser()}}}};
}

QJsonObject AgentBridge::song(bool words) const {
    Plan* plan = editor->plan();
    const double fps = editor->fps();
    auto frame = [&](int ms) { return int(std::lround(ms * fps / 1000.0)); };
    QMap<int, QString> names;
    for (const auto& c : plan->characters()) names[c.toMap()["id"].toInt()] = c.toMap()["fields"].toMap()["name"].toString();
    QJsonArray sections;
    const auto list = plan->sections();
    for (const auto& v : list) {
        const auto s = v.toMap();
        sections.append(QJsonObject{{"ordinal", s["ordinal"].toInt()}, {"label", s["label"].toString()}, {"kind", s["kind"].toString()},
                                    {"startMs", s["start"].toInt()}, {"endMs", s["end"].toInt()},
                                    {"startFrame", frame(s["start"].toInt())}, {"endFrame", frame(s["end"].toInt())}});
    }
    QJsonArray lines;
    const auto ls = editor->lyrics()->lines();
    const auto singers = plan->singers();
    for (int i = 0; i < ls.size(); ++i) {
        const auto l = ls[i].toMap();
        const int start = l["start"].toInt(), end = l["end"].toInt();
        QString section;
        for (const auto& v : list) if (v.toMap()["start"].toInt() <= start && start < v.toMap()["end"].toInt()) section = v.toMap()["label"].toString();
        QJsonArray who;
        for (const auto& s : singers[i].toMap()["singers"].toList()) who.append(QJsonObject{{"id", s.toInt()}, {"name", names.value(s.toInt())}});
        QJsonObject line{{"row", i}, {"text", l["text"].toString()}, {"section", section}, {"startMs", start}, {"endMs", end},
                         {"startFrame", frame(start)}, {"endFrame", frame(end)}, {"flagged", l["flag"].toBool()}, {"singers", who}};
        if (singers[i].toMap()["stale"].toBool()) line["singerStale"] = true;
        if (words) {
            QJsonArray ws;
            for (const auto& w : l["words"].toList())
                ws.append(QJsonObject{{"word", w.toMap()["word"].toString()}, {"startMs", w.toMap()["start"].toInt()}, {"endMs", w.toMap()["end"].toInt()}});
            line["words"] = ws;
        }
        lines.append(line);
    }
    QJsonObject out{{"fps", fps}, {"frames", editor->length()}, {"sections", sections}, {"lines", lines}};
    if (ls.isEmpty())
        out["note"] = editor->lyrics()->text().trimmed().isEmpty() ? "The song has no lyrics yet." : "The lyrics are not aligned yet; times are unknown. Lyrics:\n" + editor->lyrics()->text();
    return out;
}

QJsonObject AgentBridge::planView() const {
    Plan* plan = editor->plan();
    auto fieldsOf = [](const QVariantMap& row) {
        return QJsonObject{{"fields", json(row["fields"])}, {"provenance", json(row["prov"])}};
    };
    auto refsOf = [&](const QString& owner, int id) {
        QJsonArray out;
        for (const auto& v : plan->referencesFor(owner, id)) {
            const auto r = v.toMap();
            QJsonObject ref{{"id", r["id"].toInt()}, {"view", r["view"].toInt()}, {"role", r["role"].toString()}, {"chosen", r["approved"].toBool()},
                            {"name", r["name"].toString()}, {"width", r["width"].toInt()}, {"height", r["height"].toInt()},
                            {"addedBy", r["source"].toString() == "agent" ? "agent" : "user"}};
            if (r["missing"].toBool()) ref["missing"] = true;
            if (!r["fields"].toMap().value("notes").toString().isEmpty()) ref["notes"] = r["fields"].toMap()["notes"].toString();
            out.append(ref);
        }
        return out;
    };
    // Each view: one way of seeing the item, with the pictures it is made from and its chosen one.
    auto viewsOf = [&](const QString& owner, int id) {
        QJsonArray out;
        for (const auto& v : plan->viewsFor(owner, id)) {
            const auto m = v.toMap();
            QJsonObject o = fieldsOf(m);
            o["id"] = m["id"].toInt();
            const auto chosen = m["chosen"].toMap();
            o["chosen"] = chosen.isEmpty() ? QJsonValue() : QJsonValue(chosen["id"].toInt());
            QJsonArray pictures;
            for (const auto& p : m["pictures"].toList()) pictures.append(p.toMap()["id"].toInt());
            o["pictures"] = pictures;
            out.append(o);
        }
        return out;
    };
    QJsonObject concept = fieldsOf(plan->concept());
    QJsonArray characters;
    const auto looks = plan->looks();
    for (const auto& v : plan->characters()) {
        const auto c = v.toMap();
        const int id = c["id"].toInt();
        QJsonObject o = fieldsOf(c);
        o["id"] = id;
        QJsonArray myLooks;
        for (const auto& lv : looks) {
            const auto l = lv.toMap();
            if (l["character"].toInt() != id) continue;
            QJsonObject lo = fieldsOf(l);
            lo["id"] = l["id"].toInt();
            lo["references"] = refsOf("look", l["id"].toInt());
            lo["views"] = viewsOf("look", l["id"].toInt());
            myLooks.append(lo);
        }
        o["looks"] = myLooks;
        o["references"] = refsOf("character", id);
        o["views"] = viewsOf("character", id);
        characters.append(o);
    }
    QJsonArray locations;
    for (const auto& v : plan->locations()) {
        const auto l = v.toMap();
        QJsonObject o = fieldsOf(l);
        o["id"] = l["id"].toInt();
        o["references"] = refsOf("location", l["id"].toInt());
        o["views"] = viewsOf("location", l["id"].toInt());
        locations.append(o);
    }
    QJsonArray props;
    for (const auto& v : plan->props()) {
        const auto p = v.toMap();
        QJsonObject o = fieldsOf(p);
        o["id"] = p["id"].toInt();
        o["references"] = refsOf("prop", p["id"].toInt());
        o["views"] = viewsOf("prop", p["id"].toInt());
        props.append(o);
    }
    QJsonArray arc;
    for (const auto& v : plan->arc()) {
        const auto a = v.toMap();
        QJsonObject o = fieldsOf(a);
        o["ordinal"] = a["ordinal"].toInt();
        o["section"] = a["label"].toString();
        if (a["mismatch"].toBool()) o["writtenFor"] = a["written"].toString();
        arc.append(o);
    }
    QJsonArray shots;
    for (const auto& v : plan->shots()) {
        const auto s = v.toMap();
        QJsonObject o = fieldsOf(s);
        o["id"] = s["id"].toInt();
        o["number"] = s["number"].toInt();
        o["track"] = s["track"].toInt();
        o["start"] = s["start"].toInt();
        o["end"] = s["end"].toInt();
        o["startMs"] = s["startMs"].toInt();
        o["endMs"] = s["endMs"].toInt();
        QJsonArray rows;
        for (const auto& l : plan->linesIn(s["start"].toInt(), s["end"].toInt())) rows.append(l.toMap()["row"].toInt());
        o["lyricRows"] = rows;
        if (s["stale"].toBool()) o["stale"] = true;
        if (s["problems"].toInt()) o["openChecks"] = s["problems"].toInt();
        // Its takes (videos made for it); the user chooses which one the cut plays.
        QJsonArray takes;
        for (const auto& tv : plan->takesFor(s["id"].toInt())) {
            const auto t = tv.toMap();
            takes.append(QJsonObject{{"id", t["id"].toInt()}, {"number", t["number"].toInt()}, {"chosen", t["chosen"].toBool()},
                                     {"lipSync", t["lipSync"].toBool()}, {"seconds", t["seconds"].toDouble()},
                                     {"prompt", t["generation"].toMap()["prompt"].toString()}});
        }
        if (!takes.isEmpty()) o["takes"] = takes;
        shots.append(o);
    }
    // The user's image workflows, so prompts and instructions suit the model that will read them.
    QJsonArray workflows, videoWorkflows;
    auto flows = editor->workflows();
    for (const auto& v : flows->list()) {
        const auto w = v.toMap();
        if (!w["ready"].toBool()) continue;
        const bool video = w["use"].toString().startsWith("take-");
        QJsonObject o{{"name", w["name"].toString()}, {"use", w["useLabel"].toString()}, {"isDefault", w["isDefault"].toBool()},
                      {"maxPictures", flows->pictureCount(w["id"].toString())},
                      {"picturesAreCalled", editor->generator()->pictureLabel(w["id"].toString(), 1) + ", " + editor->generator()->pictureLabel(w["id"].toString(), 2) + ", …"}};
        if (!flows->notes(w["id"].toString()).isEmpty()) o["userNotes"] = flows->notes(w["id"].toString());
        (video ? videoWorkflows : workflows).append(o);
    }
    return {{"concept", concept}, {"characters", characters}, {"locations", locations}, {"props", props}, {"arc", arc}, {"shots", shots}, {"imageWorkflows", workflows},
            {"videoWorkflows", videoWorkflows}};
}

QJsonObject AgentBridge::checks() const {
    Plan* plan = editor->plan();
    QJsonArray gates, list;
    for (const auto& g : plan->gates()) gates.append(json(g));
    for (const auto& v : plan->checks()) {
        const auto c = v.toMap();
        QJsonObject o{{"level", c["level"].toString()}, {"gate", c["gate"].toString()}, {"code", c["code"].toString()},
                      {"message", c["message"].toString()}};
        if (c["end"].toInt() > c["start"].toInt()) { o["startMs"] = c["start"].toInt(); o["endMs"] = c["end"].toInt(); }
        if (!c["shots"].toList().isEmpty()) o["shots"] = QJsonArray::fromVariantList(c["shots"].toList());
        if (c.contains("owner")) o["about"] = json(c["owner"]);
        list.append(o);
    }
    return {{"gates", gates}, {"checks", list}};
}

QJsonObject AgentBridge::schema() const {
    Plan* plan = editor->plan();
    QJsonArray routes;
    for (const auto& v : plan->options()["route"].toList()) {
        const auto r = v.toMap();
        routes.append(QJsonObject{{"value", r["value"].toString()}, {"label", r["label"].toString()},
                                  {"maxFrames", plan->maxFrames(r["value"].toString())}});
    }
    const QJsonArray ops{
        QJsonObject{{"op", "set"}, {"params", "{kind, id, fields, section?}"},
                    {"notes", "kind: concept (no id), character, look, location, prop, arc (id = section ordinal; give \"section\": its label as a guard), shot or reference. "
                              "A field set to \"\" or null is cleared."}},
        QJsonObject{{"op", "add"}, {"params", "{kind, fields, character?}"}, {"notes", "kind: character, look (needs character id), location or prop. Returns the new id."}},
        QJsonObject{{"op", "remove"}, {"params", "{kind, id}"}, {"notes", "character (with its looks), look, location, prop, shot or reference."}},
        QJsonObject{{"op", "singers"}, {"params", "{rows: [lyric rows], singers: [character ids]}"}, {"notes", "Who sings those lines; [] clears."}},
        QJsonObject{{"op", "shot.add"}, {"params", "{track: 1|2, start, end, fields}"}, {"notes", "Frames, end exclusive, within the song; shots on one track never overlap."}},
        QJsonObject{{"op", "shot.retime"}, {"params", "{id, track?, start?, end?}"}},
        QJsonObject{{"op", "shot.split"}, {"params", "{id, at}"}, {"notes", "Splits at a frame inside the shot; both parts keep the fields."}},
        QJsonObject{{"op", "shot.splitLines"}, {"params", "{id}"}, {"notes", "Splits a shot that is too long for its route between lyric lines."}},
        QJsonObject{{"op", "shot.markCurrent"}, {"params", "{ids}"}, {"notes", "After reviewing shots flagged stale."}},
        QJsonObject{{"op", "ref.add"}, {"params", "{owner: character|look|location|prop, ownerId, path, role?}"},
                    {"notes", "Copies a local image into the project. Pictures an agent adds wait for the user's approval."}},
        QJsonObject{{"op", "ref.move"}, {"params", "{id, position}"}},
    };
    return {{"fps", editor->fps()}, {"songFrames", editor->length()},
            {"times", "Shots: project frames, start inclusive, end exclusive. Lyrics and sections: milliseconds, with frame equivalents in get_song."},
            {"values", QJsonObject{{"who", "a list of {character: id, look?: id}, or \"none\" for no one in frame"},
                                   {"where", "a location id, or \"none\" (abstract, titles)"},
                                   {"props", "a list of prop ids: the objects that matter in the shot (optional)"},
                                   {"workflow", "shot field set by the user when choosing a take workflow; leave it alone"},
                                   {"follows", "a shot id this shot continues from"},
                                   {"prompts", "{route: prompt text}; leave empty, prompts are written when generating"},
                                   {"rules", "concept rules: a list of strings"},
                                   {"brief", "concept brief: the user's direction in their words; agents may only add to the end of it"}}},
            {"requiredShotFields", QJsonArray{"type", "purpose", "who", "where", "framing", "move", "route"}},
            {"gapShot", "type \"gap\" needs only purpose, the reason for the gap"},
            {"routes", routes}, {"kinds", QJsonObject::fromVariantMap(plan->schema())}, {"ops", ops},
            {"provenance", "Every field records by (user or agent) and locked. You cannot lock or unlock; the user can."}};
}

QJsonObject AgentBridge::apply(const QJsonObject& arguments) {
    const QString label = arguments.value("label").toString().trimmed();
    const QJsonArray commands = arguments.value("commands").toArray();
    if (label.isEmpty()) return failure("Give the pass a short label, e.g. \"Draft shot list\".");
    if (commands.isEmpty()) return failure("commands is an empty list.");
    for (const auto& c : commands) if (c.toObject().value("op").toString() == "lock") return failure("Only the user can lock or unlock fields.");
    Plan* plan = editor->plan();
    const QString undoLabel = "Agent: " + label;
    const QVariantMap result = plan->run({{"op", "batch"}, {"label", undoLabel}, {"by", "agent"},
                                          {"mode", arguments.value("mode").toString("agent")}, {"commands", commands.toVariantList()}});
    if (!result.value("ok").toBool()) return failure("Nothing was applied. " + result.value("error").toString());
    const int changes = result.value("changes").toInt();
    int errors = 0, todos = 0, advice = 0;
    for (const auto& c : plan->checks()) {
        const QString level = c.toMap()["level"].toString();
        errors += level == "error"; todos += level == "todo"; advice += level == "warning";
    }
    QJsonObject out{{"applied", changes > 0}, {"changedRows", changes}, {"undo", undoLabel},
                    {"ids", QJsonArray::fromVariantList(result.value("ids").toList())},
                    {"refs", QJsonObject::fromVariantMap(result.value("refs").toMap())},
                    {"checks", QJsonObject{{"errors", errors}, {"todos", todos}, {"advice", advice}}}};
    if (!result.value("skipped").toStringList().isEmpty())
        out["skipped"] = QJsonArray::fromStringList(result.value("skipped").toStringList());
    if (!changes) out["note"] = "Nothing changed: every field already had that value or is the user's.";
    else emit passApplied(undoLabel, changes, currentClient);
    return text(out);
}

QJsonObject AgentBridge::requestsTool(const QJsonObject& arguments) {
    const QJsonArray threads = editor->requests()->forAgent(arguments.value("include_done").toBool());
    return text({{"requests", threads},
                 {"note", threads.isEmpty() ? "No open requests." : "Do what each request waiting for you asks (apply_plan, labelled after it), then answer it with reply_request."}});
}

QJsonObject AgentBridge::replyTool(const QJsonObject& arguments) {
    QString error;
    const int id = arguments.value("request_id").toVariant().toInt();
    if (!editor->requests()->agentReply(id, arguments.value("text").toString(), arguments.value("done").toBool(), error)) return failure(error);
    return text({{"sent", true}, {"request", id}, {"waitingFor", arguments.value("done").toBool() ? "nobody (done)" : "user"}});
}

QJsonObject AgentBridge::askTool(const QJsonObject& arguments) {
    QString error;
    QStringList options;
    for (const auto& o : arguments.value("options").toArray()) if (!o.toString().trimmed().isEmpty()) options << o.toString().trimmed();
    const int id = editor->requests()->agentAsk(arguments.value("question").toString(), arguments.value("about").toObject(), options, error);
    if (!id) return failure(error);
    return text({{"asked", true}, {"request", id},
                 {"note", "The question is shown in ChorusCut. The answer arrives as a channel event if this session has ChorusCut as a channel; otherwise read it later with get_requests. Don't wait for it: carry on or end your turn."}});
}

QJsonObject AgentBridge::reference(int id) const {
    for (const auto& v : editor->plan()->references()) {
        const auto r = v.toMap();
        if (r["id"].toInt() != id) continue;
        QImageReader reader(QDir(editor->projectPath()).filePath(r["file"].toString()));
        reader.setAutoTransform(true);
        QSize size = reader.size();
        if (size.width() > 1024 || size.height() > 1024) { size.scale(1024, 1024, Qt::KeepAspectRatio); reader.setScaledSize(size); }
        const QImage image = reader.read();
        if (image.isNull()) return failure("The picture file is missing or unreadable: " + r["name"].toString());
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "JPG", 85);
        const QString owner = r["owner"].toString();
        QString ownerName;
        const auto rows = owner == "character" ? editor->plan()->characters() : owner == "look" ? editor->plan()->looks()
                        : owner == "prop" ? editor->plan()->props() : editor->plan()->locations();
        for (const auto& o : rows) if (o.toMap()["id"].toInt() == r["ownerId"].toInt()) ownerName = o.toMap()["fields"].toMap()["name"].toString();
        const QString caption = QString("%1 (%2) · %3 · %4 · %5, %6×%7").arg(ownerName, owner, r["roleLabel"].toString(),
            r["approved"].toBool() ? "approved" : "not approved yet", r["name"].toString()).arg(r["width"].toInt()).arg(r["height"].toInt());
        return {{"content", QJsonArray{QJsonObject{{"type", "image"}, {"data", QString::fromLatin1(bytes.toBase64())}, {"mimeType", "image/jpeg"}},
                                       QJsonObject{{"type", "text"}, {"text", caption}}}},
                {"isError", false}};
    }
    return failure("There is no reference " + QString::number(id) + "; see get_plan for ids.");
}
