#include "requests.h"
#include "editor.h"
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSqlQuery>
#include <algorithm>

namespace {
qint64 now() { return QDateTime::currentSecsSinceEpoch(); }
bool known(const QString& kind) {
    return kind.isEmpty() || kind == "concept" || kind == "character" || kind == "look" || kind == "location" || kind == "arc" || kind == "shot" || kind == "view";
}
}

Requests::Requests(Editor* editor) : QObject(editor), editor(editor) {}

bool Requests::ensureTable() {
    auto project = editor->current();
    if (!project) return false;
    QSqlQuery q(project->db);
    return q.exec("CREATE TABLE IF NOT EXISTS requests(id INTEGER PRIMARY KEY, data TEXT NOT NULL)");
}

void Requests::load() {
    threads.clear();
    auto project = editor->current();
    // Opening a project never writes to it: the table appears with the first request.
    if (project && project->db.tables().contains("requests")) {
        QSqlQuery q(project->db);
        if (q.exec("SELECT id, data FROM requests"))
            while (q.next()) threads[q.value(0).toInt()] = QJsonDocument::fromJson(q.value(1).toString().toUtf8()).object();
    }
    emit changed();
}

void Requests::unload() {
    threads.clear();
    emit changed();
}

bool Requests::save(int id) {
    if (!ensureTable()) return false;
    QSqlQuery q(editor->current()->db);
    q.prepare("INSERT OR REPLACE INTO requests(id, data) VALUES(?, ?)");
    q.addBindValue(id);
    q.addBindValue(QString::fromUtf8(QJsonDocument(threads[id]).toJson(QJsonDocument::Compact)));
    const bool ok = q.exec();
    emit changed();
    return ok;
}

QString Requests::currentLabel(const QJsonObject& t) const {
    const auto about = t["about"].toObject();
    const QString kind = about["kind"].toString(), label = t["label"].toString();
    if ((kind == "character" || kind == "look" || kind == "location" || kind == "shot" || kind == "view") &&
        editor->plan()->item(kind, about["id"].toInt()).isEmpty())
        return label + " (removed)";
    return label;
}

QVariantList Requests::list() const {
    QList<int> ids = threads.keys();
    std::sort(ids.begin(), ids.end(), [&](int a, int b) {
        const bool doneA = threads[a]["turn"] == "done", doneB = threads[b]["turn"] == "done";
        if (doneA != doneB) return doneB;
        return threads[a]["updated"].toInteger() != threads[b]["updated"].toInteger()
                   ? threads[a]["updated"].toInteger() > threads[b]["updated"].toInteger() : a > b;
    });
    QVariantList out;
    for (int id : ids) {
        QVariantMap m = threads[id].toVariantMap();
        m["id"] = id;
        m["label"] = currentLabel(threads[id]);
        out.append(m);
    }
    return out;
}

int Requests::waitingOnUser() const {
    int n = 0;
    for (const auto& t : threads) n += t["turn"] == "user";
    return n;
}

int Requests::waitingOnAgent() const {
    int n = 0;
    for (const auto& t : threads) n += t["turn"] == "agent";
    return n;
}

QJsonObject Requests::eventFor(int id, const QString& text, const QString& kind) const {
    const auto& t = threads[id];
    return {{"thread", id}, {"text", text}, {"about", t["about"]}, {"label", currentLabel(t)}, {"kind", kind}};
}

int Requests::ask(const QString& kind, int id, const QString& field, const QString& label, const QString& text) {
    const QString body = text.trimmed();
    if (!editor->current() || body.isEmpty() || !known(kind)) return 0;
    const int thread = threads.isEmpty() ? 1 : threads.lastKey() + 1;
    QJsonObject about;
    if (!kind.isEmpty()) about = {{"kind", kind}, {"id", id}, {"field", field}};
    threads[thread] = {{"about", about}, {"label", label.trimmed().isEmpty() ? QString("The plan") : label.trimmed()},
                       {"origin", "user"}, {"turn", "agent"}, {"seen", false}, {"sent", false}, {"created", now()}, {"updated", now()},
                       {"messages", QJsonArray{QJsonObject{{"from", "user"}, {"text", body}, {"at", now()}}}}};
    if (!save(thread)) { threads.remove(thread); emit changed(); return 0; }
    emit posted(eventFor(thread, body, "request"));
    return thread;
}

bool Requests::reply(int thread, const QString& text) {
    const QString body = text.trimmed();
    if (!threads.contains(thread) || body.isEmpty()) return false;
    auto& t = threads[thread];
    auto messages = t["messages"].toArray();
    messages.append(QJsonObject{{"from", "user"}, {"text", body}, {"at", now()}});
    t["messages"] = messages;
    t["turn"] = "agent";
    t["seen"] = false;
    t["sent"] = false;
    t["updated"] = now();
    if (!save(thread)) return false;
    emit posted(eventFor(thread, body, t["origin"] == "agent" && messages.size() == 2 ? "answer" : "request"));
    return true;
}

bool Requests::close(int thread) {
    if (!threads.contains(thread)) return false;
    threads[thread]["turn"] = "done";
    threads[thread]["updated"] = now();
    return save(thread);
}

bool Requests::reopen(int thread) {
    if (!threads.contains(thread) || threads[thread]["turn"] != "done") return false;
    const auto messages = threads[thread]["messages"].toArray();
    threads[thread]["turn"] = !messages.isEmpty() && messages.last().toObject()["from"] == "agent" ? "user" : "agent";
    threads[thread]["updated"] = now();
    return save(thread);
}

bool Requests::remove(int thread) {
    if (!threads.contains(thread) || !ensureTable()) return false;
    QSqlQuery q(editor->current()->db);
    q.prepare("DELETE FROM requests WHERE id=?");
    q.addBindValue(thread);
    if (!q.exec()) return false;
    threads.remove(thread);
    emit changed();
    return true;
}

QVariantList Requests::about(const QString& kind, int id, const QString& field) const {
    QVariantList out;
    for (auto it = threads.begin(); it != threads.end(); ++it) {
        const auto a = it.value()["about"].toObject();
        if (it.value()["turn"] == "done" || a["kind"].toString() != kind || a["id"].toInt() != id) continue;
        if (!field.isEmpty() && a["field"].toString() != field) continue;
        QVariantMap m = it.value().toVariantMap();
        m["id"] = it.key();
        out.append(m);
    }
    return out;
}

// What the agent needs to write a view's framing or instruction: how it is made, the pictures it
// is made from in order with what its workflow calls them (to look at with view_reference), and
// what the app sends after the instruction.
QJsonObject Requests::viewContext(int view) const {
    Plan* plan = editor->plan();
    const auto v = plan->item("view", view);
    if (v.isEmpty()) return {};
    auto nameOf = [&](const QString& kind, int id) {
        const auto item = plan->item(kind, id);
        const QString name = item["fields"].toMap()["name"].toString();
        if (kind != "look") return name;
        return plan->item("character", item["character"].toInt())["fields"].toMap()["name"].toString() + " · " + name;
    };
    const auto f = v["fields"].toMap();
    const bool edited = f["from"].toString() == "pictures";
    const auto opts = editor->generator()->viewOptions(view, edited ? "ref-edit" : "ref-text");
    const QString workflow = opts["workflow"].toString();
    QJsonObject o{{"name", f["name"].toString()}, {"of", nameOf(v["owner"].toString(), v["ownerId"].toInt())},
                  {"madeFrom", edited ? "pictures the user picked" : "the appearance alone"},
                  {"sentAs", "this field, a blank line, then the appearance below; don't repeat the appearance in it"},
                  {"appearance", opts["subject"].toString()}};
    if (!workflow.isEmpty()) o["workflow"] = opts["workflowName"].toString();
    if (!edited) return o;
    QJsonArray pictures;
    int n = 0;
    for (const auto& i : opts["inputs"].toList()) {
        const auto r = plan->item("reference", i.toInt());
        if (r.isEmpty()) continue;
        ++n;
        QJsonObject p{{"id", i.toInt()}, {"of", nameOf(r["owner"].toString(), r["ownerId"].toInt())},
                      {"view", r["viewName"].toString()}, {"chosen", r["approved"].toBool()}};
        if (!workflow.isEmpty()) p["calledInPrompt"] = editor->generator()->pictureLabel(workflow, n);
        pictures.append(p);
    }
    o["pictures"] = pictures;
    o["pictureNote"] = pictures.isEmpty() ? QString("No pictures are picked yet; ask the user to pick them in the Generate dialog.")
                     : opts["guessed"].toBool() ? QString("The user hasn't picked these; they are the app's first guess.")
                     : QString("The user picked these, in this order. Look at each with view_reference before writing.");
    return o;
}

// What the agent needs to write a shot's video prompt: the route and workflow, the pictures in
// order with what the prompt calls them, and the take's timeline: it starts before the cut by
// its handle, so the shot's lines are given in seconds from the take's first frame.
QJsonObject Requests::takeContext(int shot) const {
    Plan* plan = editor->plan();
    const auto s = plan->shot(shot);
    if (s.isEmpty()) return {};
    const auto opts = editor->generator()->takeOptions(shot);
    const double fps = editor->fps();
    const double takeStart = s["start"].toInt() / fps - opts["handles"].toDouble();
    auto at = [&](double seconds) { return std::round((seconds - takeStart) * 100) / 100; };
    QJsonObject o{{"route", opts["route"].toString()}, {"writeTo", QString("fields.prompts[\"%1\"] of shot %2").arg(opts["route"].toString()).arg(shot)},
                  {"lipSync", opts["lipSync"].toBool()}, {"seconds", opts["seconds"].toDouble()}, {"frames", opts["frames"].toInt()},
                  {"cutIn", at(s["start"].toInt() / fps)}, {"cutOut", at(s["end"].toInt() / fps)}};
    if (!opts["workflowName"].toString().isEmpty()) o["workflow"] = opts["workflowName"].toString();
    if (!opts["why"].toString().isEmpty()) o["notReadyBecause"] = opts["why"].toString();
    QJsonArray pictures;
    for (const auto& pv : opts["pictures"].toList()) {
        const auto p = pv.toMap();
        const auto ref = plan->item("reference", p["id"].toInt());
        QString whose = ref["owner"].toString() == "look"
            ? plan->item("character", plan->item("look", ref["ownerId"].toInt())["character"].toInt())["fields"].toMap()["name"].toString() + " · " +
              plan->item("look", ref["ownerId"].toInt())["fields"].toMap()["name"].toString()
            : plan->item(ref["owner"].toString(), ref["ownerId"].toInt())["fields"].toMap()["name"].toString();
        pictures.append(QJsonObject{{"id", p["id"].toInt()}, {"calledInPrompt", p["label"].toString()}, {"of", whose}, {"view", p["viewName"].toString()}});
    }
    o["pictures"] = pictures;
    QJsonArray lines;
    const double from = takeStart, to = takeStart + opts["seconds"].toDouble();
    for (const auto& lv : editor->lyrics()->lines()) {
        const auto l = lv.toMap();
        const double a = l["start"].toInt() / 1000.0, b = l["end"].toInt() / 1000.0;
        if (b <= from || a >= to) continue;
        lines.append(QJsonObject{{"text", l["text"].toString()}, {"start", at(a)}, {"end", at(b)}});
    }
    o["lyrics"] = lines;
    o["timing"] = "Times are seconds from the take's first frame. The cut uses cutIn to cutOut; the rest is handle for trimming, so keep the action continuous through it.";
    o["format"] = opts["lipSync"].toBool()
        ? "MiniMax H3 reference-to-video with the song locked in: subject_definitions (each <Picture n>), summary, retention_analysis, detailed_description (what we see, second by second, with the singer's mouth on the sung words), overall_soundscape (the song), non_diegetic_music."
        : "MiniMax H3 reference-to-video: subject_definitions (each <Picture n>), summary, retention_analysis, detailed_description (what we see, second by second), overall_soundscape, non_diegetic_music.";
    return o;
}

QJsonArray Requests::forAgent(bool includeDone) {
    QJsonArray out;
    Plan* plan = editor->plan();
    bool marked = false;
    for (auto it = threads.begin(); it != threads.end(); ++it) {
        auto& t = it.value();
        if (!includeDone && t["turn"] == "done") continue;
        QJsonObject o{{"id", it.key()}, {"about", t["about"]}, {"label", currentLabel(t)}, {"startedBy", t["origin"]},
                      {"waitingFor", t["turn"] == "done" ? "nobody (done)" : t["turn"].toString()}, {"messages", t["messages"]}};
        // The value as it stands now, so the agent need not look it up.
        const auto about = t["about"].toObject();
        const QString kind = about["kind"].toString(), field = about["field"].toString();
        if (!kind.isEmpty() && !field.isEmpty()) {
            QVariantMap fields;
            if (kind == "concept") fields = plan->concept()["fields"].toMap();
            else if (kind == "arc") fields = plan->arc().value(about["id"].toInt()).toMap()["fields"].toMap();
            else fields = plan->item(kind, about["id"].toInt())["fields"].toMap();
            o["currentValue"] = QJsonValue::fromVariant(fields.value(field));
        }
        if (kind == "view") o["view"] = viewContext(about["id"].toInt());
        if (kind == "shot" && (field == "prompts" || field.isEmpty())) o["take"] = takeContext(about["id"].toInt());
        out.append(o);
        if (t["turn"] == "agent" && !t["seen"].toBool()) { t["seen"] = true; save(it.key()); marked = true; }
    }
    if (marked) emit changed();
    return out;
}

bool Requests::agentReply(int thread, const QString& text, bool done, QString& error) {
    if (!threads.contains(thread)) { error = QString("There is no request %1.").arg(thread); return false; }
    if (text.trimmed().isEmpty()) { error = "Write a reply for the user."; return false; }
    auto& t = threads[thread];
    auto messages = t["messages"].toArray();
    messages.append(QJsonObject{{"from", "agent"}, {"text", text.trimmed()}, {"at", now()}});
    t["messages"] = messages;
    t["turn"] = done ? "done" : "user";
    t["seen"] = true;
    t["updated"] = now();
    return save(thread);
}

int Requests::agentAsk(const QString& question, const QJsonObject& about, const QStringList& options, QString& error) {
    if (!editor->current()) { error = "No project is open in Chorus Cut."; return 0; }
    if (question.trimmed().isEmpty()) { error = "Write the question."; return 0; }
    const QString kind = about["kind"].toString();
    if (!known(kind)) { error = "about.kind must be concept, character, look, location, view, arc or shot."; return 0; }
    const int id = about["id"].toInt();
    QString label = "The plan";
    Plan* plan = editor->plan();
    if (kind == "concept") label = "Concept";
    else if (kind == "arc") label = plan->sections().value(id).toMap()["label"].toString();
    else if (kind == "shot") label = "Shot " + QString::number(plan->shot(id)["number"].toInt());
    else if (kind == "view") {
        const auto v = plan->item("view", id);
        if (v.isEmpty()) { error = QString("There is no view %1.").arg(id); return 0; }
        label = plan->item(v["owner"].toString(), v["ownerId"].toInt())["fields"].toMap()["name"].toString() + " · " + v["fields"].toMap()["name"].toString();
    }
    else if (!kind.isEmpty()) {
        const auto item = plan->item(kind, id);
        if (item.isEmpty()) { error = QString("There is no %1 %2.").arg(kind).arg(id); return 0; }
        label = item["fields"].toMap()["name"].toString();
        if (kind == "look") label = plan->item("character", item["character"].toInt())["fields"].toMap()["name"].toString() + " · " + label;
    }
    const QString field = about["field"].toString();
    if (!field.isEmpty()) label += " · " + field;
    const int thread = threads.isEmpty() ? 1 : threads.lastKey() + 1;
    QJsonObject message{{"from", "agent"}, {"text", question.trimmed()}, {"at", now()}};
    if (!options.isEmpty()) message["options"] = QJsonArray::fromStringList(options);
    threads[thread] = {{"about", kind.isEmpty() ? QJsonObject{} : QJsonObject{{"kind", kind}, {"id", id}, {"field", field}}},
                       {"label", label}, {"origin", "agent"}, {"turn", "user"}, {"seen", true}, {"sent", false},
                       {"created", now()}, {"updated", now()}, {"messages", QJsonArray{message}}};
    if (!save(thread)) { threads.remove(thread); error = "Could not save the question in the project."; return 0; }
    return thread;
}

void Requests::markSent(int thread, bool sent) {
    if (!threads.contains(thread) || threads[thread]["sent"].toBool() == sent) return;
    threads[thread]["sent"] = sent;
    save(thread);
}
