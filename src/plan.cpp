#include "plan.h"
#include "lyrics.h"
#include "project.h"
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImageReader>
#include <QJsonDocument>
#include <QUrl>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QUndoStack>
#include <algorithm>
#include <functional>
#include <cmath>

namespace {
// H3 renders at most 362 frames at 24 fps in one generation.
constexpr double kH3MaxSeconds = 362.0 / 24.0;

struct Option { const char* value; const char* label; };
const QMap<QString, QVector<Option>>& optionTable() {
    static const QMap<QString, QVector<Option>> table = {
        {"videoKind", {{"performance", "Performance"}, {"narrative", "Narrative"}, {"abstract", "Abstract"}, {"mixed", "Mixed"}}},
        {"aspect", {{"16:9", "16:9 landscape"}, {"9:16", "9:16 vertical"}, {"1:1", "1:1 square"}, {"4:5", "4:5 portrait"}, {"2.39:1", "2.39:1 scope"}}},
        {"role", {{"singer", "Singer / performer"}, {"band", "Band member"}, {"actor", "Actor"}, {"extra", "Extra"}}},
        {"timeOfDay", {{"dawn", "Dawn"}, {"day", "Day"}, {"golden", "Golden hour"}, {"dusk", "Dusk"}, {"night", "Night"}, {"interior", "Interior, any time"}}},
        {"density", {{"slow", "Slow cutting"}, {"medium", "Medium cutting"}, {"fast", "Fast cutting"}}},
        {"type", {{"lipsync", "Lip-sync performance"}, {"performance", "Performance, no sync"}, {"story", "Story action"},
                  {"broll", "B-roll"}, {"insert", "Insert"}, {"abstract", "Abstract"}, {"transition", "Transition"},
                  {"title", "Title"}, {"gap", "Deliberate gap"}}},
        {"framing", {{"ecu", "Extreme close-up"}, {"cu", "Close-up"}, {"mcu", "Medium close-up"}, {"ms", "Medium"},
                     {"mls", "Medium long"}, {"ws", "Wide"}, {"ews", "Extreme wide"}}},
        {"angle", {{"eye", "Eye level"}, {"low", "Low angle"}, {"high", "High angle"}, {"overhead", "Overhead"},
                   {"dutch", "Dutch angle"}, {"pov", "Point of view"}}},
        {"move", {{"static", "Static"}, {"pan", "Pan"}, {"tilt", "Tilt"}, {"push", "Push in"}, {"pull", "Pull out"},
                  {"track", "Tracking"}, {"crane", "Crane"}, {"handheld", "Handheld"}, {"orbit", "Orbit"}, {"zoom", "Zoom"}}},
        {"energy", {{"low", "Low"}, {"medium", "Medium"}, {"high", "High"}}},
        {"refRole", {{"face", "Face"}, {"body", "Full body"}, {"look", "Look"}, {"setting", "Setting"}, {"detail", "Detail"}, {"other", "Other"}}},
        {"route", {{"h3-lipsync", "H3 lip-sync with the song"}, {"i2v", "Still, then image-to-video"}, {"flf", "First and last frame"},
                   {"t2v", "Text-to-video"}, {"ref2v", "Reference-to-video"}, {"still", "Still only"}}},
    };
    return table;
}
QString optionLabel(const QString& key, const QString& value) {
    for (const auto& o : optionTable().value(key)) if (value == o.value) return o.label;
    return value;
}

// The fields each kind accepts: "text", "texts" (a list of strings), "enum" (named in optionTable),
// "who", "where", "shotref", "singers" or "prompts".
const QMap<QString, QMap<QString, QString>>& fieldTable() {
    static const QMap<QString, QMap<QString, QString>> table = {
        {"concept", {{"logline", "text"}, {"treatment", "text"}, {"videoKind", "enum"}, {"mix", "text"}, {"aspect", "enum"},
                     {"resolution", "text"}, {"style", "text"}, {"palette", "text"}, {"lighting", "text"}, {"era", "text"},
                     {"lens", "text"}, {"grade", "text"}, {"rules", "texts"}}},
        {"character", {{"name", "text"}, {"role", "enum"}, {"identity", "text"}, {"voice", "text"}, {"notes", "text"}}},
        {"look", {{"name", "text"}, {"description", "text"}}},
        {"location", {{"name", "text"}, {"description", "text"}, {"timeOfDay", "enum"}, {"lighting", "text"}}},
        {"arc", {{"happens", "text"}, {"beat", "text"}, {"density", "enum"}}},
        {"singer", {{"singers", "singers"}}},
        {"reference", {{"role", "enum"}, {"approved", "bool"}, {"notes", "text"}}},
        {"shot", {{"type", "enum"}, {"purpose", "text"}, {"who", "who"}, {"where", "where"}, {"timeOfDay", "enum"},
                  {"framing", "enum"}, {"angle", "enum"}, {"lens", "text"}, {"move", "enum"}, {"composition", "text"},
                  {"action", "text"}, {"emotion", "text"}, {"energy", "enum"}, {"follows", "shotref"}, {"route", "enum"},
                  {"prompts", "prompts"}, {"notes", "text"}}},
    };
    return table;
}
// A shot is fully planned with these (decision 7); a deliberate gap needs only its reason.
const QStringList kRequired = {"type", "purpose", "who", "where", "framing", "move", "route"};
const QMap<QString, QString> kFieldNames = {
    {"type", "type"}, {"purpose", "purpose"}, {"who", "who is in frame"}, {"where", "location"},
    {"framing", "framing"}, {"move", "camera move"}, {"route", "route"}};

bool empty(const QJsonValue& v) {
    return v.isUndefined() || v.isNull() || (v.isString() && v.toString().trimmed().isEmpty()) ||
           (v.isArray() && v.toArray().isEmpty()) || (v.isObject() && v.toObject().isEmpty());
}

class PlanCommand : public QUndoCommand {
public:
    PlanCommand(Plan* plan, QVector<Plan::Change> changes, const QString& label)
        : QUndoCommand(label), plan(plan), changes(std::move(changes)) {}
    void redo() override {
        if (skip) { skip = false; return; }
        plan->apply(changes, true, "Redid " + text().toLower() + ".");
    }
    void undo() override { plan->apply(changes, false, "Undid " + text().toLower() + "."); }
private:
    Plan* plan;
    QVector<Plan::Change> changes;
    bool skip = true;  // Already applied when pushed.
};

QString seconds(double s) { return QString::number(s, 'f', 1) + " s"; }
QString joinNames(const QStringList& names) {
    if (names.size() <= 1) return names.join("");
    return names.mid(0, names.size() - 1).join(", ") + " and " + names.last();
}
}

Plan::Plan(Lyrics* lyrics, QObject* parent) : QObject(parent), lyrics(lyrics) {
    connect(lyrics, &Lyrics::changed, this, [this] { invalidate(); emit changed(); });
}

void Plan::say(const QString& text) { notice = text; emit messageChanged(); }
void Plan::invalidate() { checksValid = false; }
int Plan::songFrames() const { return project ? project->master.frames : 0; }
double Plan::fps() const { return project && project->fpsNum ? double(project->fpsNum) / project->fpsDen : 24; }
int Plan::msToFrame(int ms) const { return int(std::lround(ms * fps() / 1000.0)); }
int Plan::frameToMs(int frame) const { return int(std::lround(frame * 1000.0 / fps())); }
QString Plan::clockText(int frame) const {
    const double s = frame / fps();
    const int m = int(s / 60);
    return QString("%1:%2").arg(m).arg(s - 60 * m, 4, 'f', 1, QChar('0'));
}

bool Plan::load(Project& next, QString& error) {
    items.clear(); clock = 0;
    // The table is created on the first write, so opening a project leaves the file untouched.
    if (next.db.tables().contains("plan_items")) {
        QSqlQuery q(next.db);
        if (!q.exec("SELECT kind,id,data FROM plan_items")) { error = q.lastError().text(); return false; }
        while (q.next()) {
            const auto doc = QJsonDocument::fromJson(q.value(2).toByteArray());
            if (!doc.isObject()) { error = "The project contains an unreadable plan"; items.clear(); return false; }
            QJsonObject row = doc.object();
            // Older builds kept provenance for fields that were unlocked while empty; drop it.
            QJsonObject prov = row["prov"].toObject();
            const QJsonObject fields = row["fields"].toObject();
            for (const QString& key : prov.keys()) {
                QJsonObject p = prov[key].toObject();
                if (p["locked"].toBool()) continue;
                if (empty(fields.value(key))) prov.remove(key);
                else if (p.contains("locked")) { p.remove("locked"); prov[key] = p; }
            }
            if (row.contains("prov")) row["prov"] = prov;
            items[q.value(0).toString()][q.value(1).toInt()] = row;
            clock = std::max({clock, row["edited"].toInt(), row["written"].toInt()});
        }
    }
    project = &next;
    invalidate(); say({}); emit changed();
    return true;
}

void Plan::unload() {
    project = nullptr; items.clear(); clock = 0;
    invalidate(); say({}); emit changed();
}

bool Plan::persist(const QVector<Change>& changes, QString& error) {
    QSqlDatabase& db = project->db;
    QSqlQuery q(db);
    if (!q.exec("CREATE TABLE IF NOT EXISTS plan_items(kind TEXT NOT NULL, id INTEGER NOT NULL, data TEXT NOT NULL, "
                "PRIMARY KEY(kind,id))")) { error = q.lastError().text(); return false; }
    if (!db.transaction()) { error = db.lastError().text(); return false; }
    for (const auto& c : changes) {
        const QJsonObject& value = c.after;
        if (value.isEmpty()) {
            q.prepare("DELETE FROM plan_items WHERE kind=? AND id=?");
            q.addBindValue(c.kind); q.addBindValue(c.id);
        } else {
            q.prepare("INSERT OR REPLACE INTO plan_items(kind,id,data) VALUES(?,?,?)");
            q.addBindValue(c.kind); q.addBindValue(c.id);
            q.addBindValue(QString::fromUtf8(QJsonDocument(value).toJson(QJsonDocument::Compact)));
        }
        if (!q.exec()) { error = q.lastError().text(); db.rollback(); return false; }
    }
    return db.commit() || (error = db.lastError().text(), false);
}

void Plan::apply(const QVector<Change>& changes, bool forward, const QString& done) {
    if (!project) return;
    QVector<Change> writes;
    for (const auto& c : changes) {
        const QJsonObject& value = forward ? c.after : c.before;
        if (value.isEmpty()) items[c.kind].remove(c.id); else items[c.kind][c.id] = value;
        writes.append({c.kind, c.id, {}, value});
    }
    QString error;
    if (!persist(writes, error)) say(error); else say(done);
    invalidate(); emit changed();
}

void Plan::write(const QString& kind, int id, const QJsonObject& after) {
    auto existing = std::find_if(pending.begin(), pending.end(), [&](const Change& c) { return c.kind == kind && c.id == id; });
    if (existing == pending.end()) pending.append({kind, id, row(kind, id), after});
    else existing->after = after;
    if (after.isEmpty()) items[kind].remove(id); else items[kind][id] = after;
}

int Plan::nextId(const QString& kind) const {
    const auto& rows = items.value(kind);
    return rows.isEmpty() ? 1 : rows.lastKey() + 1;
}

QVariantMap Plan::run(const QVariantMap& command) {
    auto failed = [&](const QString& error) {
        say(error);
        return QVariantMap{{"ok", false}, {"error", error}};
    };
    if (!project) return failed("Open a project first.");
    const QString by = command.value("by", "user").toString();
    const QString mode = command.value("mode", "agent").toString();
    if (by != "user" && by != "agent") return failed("by must be \"user\" or \"agent\".");
    if (mode != "agent" && mode != "fill" && mode != "replace") return failed("mode must be \"agent\", \"fill\" or \"replace\".");
    pending.clear(); pendingLabel.clear();
    Result r = dispatch(command, by, mode);
    QString error = r.error;
    if (r.ok && !pending.isEmpty() && !persist(pending, error)) r.ok = false;
    if (!r.ok) {
        // All or nothing: put back every row this command touched.
        for (auto c = pending.rbegin(); c != pending.rend(); ++c)
            if (c->before.isEmpty()) items[c->kind].remove(c->id); else items[c->kind][c->id] = c->before;
        pending.clear();
        invalidate(); emit changed();
        return failed(error);
    }
    QVariantMap result{{"ok", true}, {"id", r.id}, {"skipped", r.skipped}, {"ids", r.ids}, {"refs", r.refs}, {"changes", int(pending.size())}};
    if (pending.isEmpty()) return result;
    const QString label = command.value("label").toString().isEmpty() ? pendingLabel : command.value("label").toString();
    if (history) history->push(new PlanCommand(this, pending, label.isEmpty() ? "Plan edit" : label));
    pending.clear();
    invalidate(); emit changed();
    say(label + ".");
    return result;
}

Plan::Result Plan::dispatch(const QVariantMap& c, const QString& by, const QString& mode) {
    const QString op = c.value("op").toString();
    const QString kind = c.value("kind").toString();
    auto label = [&](const QString& text) { if (pendingLabel.isEmpty()) pendingLabel = text; };
    auto error = [](const QString& text) { Result r; r.ok = false; r.error = text; return r; };
    if (op == "batch") {
        Result total;
        label(c.value("label", "Plan changes").toString());
        // "$name" stands for the id an earlier command in this batch created with "ref": "name".
        std::function<QVariant(const QVariant&)> resolve = [&](const QVariant& v) -> QVariant {
            if (v.typeId() == QMetaType::QString && v.toString().startsWith('$') && total.refs.contains(v.toString().mid(1)))
                return total.refs.value(v.toString().mid(1));
            if (v.typeId() == QMetaType::QVariantList) { QVariantList l = v.toList(); for (auto& x : l) x = resolve(x); return l; }
            if (v.typeId() == QMetaType::QVariantMap) { QVariantMap m = v.toMap(); for (auto& x : m) x = resolve(x); return m; }
            return v;
        };
        const QVariantList commands = c.value("commands").toList();
        for (int i = 0; i < commands.size(); ++i) {
            QVariantMap sub = resolve(commands[i]).toMap();
            const QString subBy = by == "agent" ? QStringLiteral("agent") : sub.value("by", by).toString();
            if (subBy != "user" && subBy != "agent") return error("by must be \"user\" or \"agent\".");
            Result r = dispatch(sub, subBy, sub.value("mode", mode).toString());
            if (!r.ok) {
                if (commands.size() > 1) r.error = QString("Command %1 (%2): %3").arg(i + 1).arg(sub.value("op").toString(), r.error);
                return r;
            }
            total.skipped += r.skipped;
            total.ids.append(r.id);
            if (r.id) total.id = r.id;
            if (sub.contains("ref")) total.refs[sub.value("ref").toString()] = r.id;
            for (auto it = r.refs.begin(); it != r.refs.end(); ++it) total.refs[it.key()] = it.value();
        }
        return total;
    }
    if (op == "set") {
        int id = kind == "concept" ? 1 : c.value("id").toInt();
        if (kind == "arc") {
            const auto list = sections();
            if (id < 0 || id >= list.size()) return error("There is no song section " + QString::number(id) + ".");
            const QString expected = list[id].toMap()["label"].toString();
            if (c.contains("section") && c.value("section").toString() != expected)
                return error(QString("The arc follows the song's section order: section %1 is \"%2\".").arg(id).arg(expected));
        } else if (kind != "concept" && !has(kind, id)) {
            return error("There is no " + kind + " " + QString::number(id) + ".");
        }
        if (!fieldTable().contains(kind) || kind == "singer") return error("Unknown kind \"" + kind + "\".");
        label("Edit " + nameOf(kind, id));
        Result r = setFields(kind, id, c.value("fields").toMap(), by, mode);
        r.id = id;
        return r;
    }
    if (op == "add") {
        if (kind != "character" && kind != "look" && kind != "location") return error("Add a character, look or location.");
        QJsonObject fresh{{"fields", QJsonObject{}}, {"prov", QJsonObject{}}, {"edited", ++clock}};
        if (kind == "look") {
            const int owner = c.value("character").toInt();
            if (!has("character", owner)) return error("A look belongs to an existing character.");
            fresh["character"] = owner;
        }
        const int id = nextId(kind);
        write(kind, id, fresh);
        QVariantMap fields = c.value("fields").toMap();
        if (fields.value("name").toString().trimmed().isEmpty()) {
            const QString noun = kind == "character" ? "Character" : kind == "look" ? "Look" : "Location";
            fields["name"] = noun + " " + QString::number(items.value(kind).size());
        }
        label("Add " + kind);
        Result r = setFields(kind, id, fields, by, "replace");
        r.id = id;
        return r;
    }
    if (op == "remove") {
        const int id = c.value("id").toInt();
        if ((kind != "character" && kind != "look" && kind != "location" && kind != "shot" && kind != "reference") || !has(kind, id))
            return error("There is no " + kind + " " + QString::number(id) + " to remove.");
        if (by == "agent") {
            const auto prov = row(kind, id)["prov"].toObject();
            for (const auto& p : prov) if (p.toObject()["locked"].toBool()) return error("The user locked " + nameOf(kind, id) + ".");
        }
        label("Remove " + nameOf(kind, id));
        if (kind == "character") {
            const auto looks = items.value("look");
            for (auto it = looks.begin(); it != looks.end(); ++it)
                if (it.value()["character"].toInt() == id) { dropReferences("look", it.key()); write("look", it.key(), {}); }
        }
        if (kind == "character" || kind == "look" || kind == "location") dropReferences(kind, id);
        // The image stays in Media/References, so undo can bring the reference back.
        write(kind, id, {});
        return {};
    }
    if (op == "lock") {
        if (by != "user") return error("Only the user can lock or unlock fields.");
        int id = kind == "concept" ? 1 : c.value("id").toInt();
        const QString field = c.value("field").toString();
        if (!fieldTable().value(kind).contains(field)) return error("Unknown field \"" + field + "\".");
        if (kind != "concept" && kind != "arc" && kind != "singer" && !has(kind, id)) return error("There is no " + kind + " " + QString::number(id) + ".");
        QJsonObject r = row(kind, id);
        QJsonObject prov = r["prov"].toObject();
        QJsonObject p = prov[field].toObject();
        const bool locked = c.value("locked", true).toBool();
        if (p["locked"].toBool() == locked) return {};
        if (!r.contains("fields")) r["fields"] = QJsonObject{};
        if (locked) {
            p["locked"] = true;
            if (!p.contains("by")) p["by"] = "user";
            prov[field] = p;
        } else if (empty(r["fields"].toObject().value(field))) {
            prov.remove(field);  // nothing written and nothing locked: no provenance left
        } else {
            p.remove("locked");
            prov[field] = p;
        }
        r["prov"] = prov;
        if (kind == "arc" && !r.contains("section")) r["section"] = sections().value(id).toMap()["label"].toString();
        // A concept, arc or singer row that only existed for the lock goes away with it.
        const bool bare = r["fields"].toObject().isEmpty() && prov.isEmpty();
        write(kind, id, bare && (kind == "concept" || kind == "arc" || kind == "singer") ? QJsonObject{} : r);
        label((locked ? "Lock " : "Unlock ") + nameOf(kind, id) + " " + field);
        return {};
    }
    if (op == "singers") {
        QVariantList rows = c.value("rows").toList();
        const auto ls = lyrics->lines();
        if (rows.isEmpty()) return error("Choose the lyric lines.");
        label(rows.size() == 1 ? "Set singer" : "Set singers");
        Result total;
        for (const auto& v : rows) {
            const int lyricRow = v.toInt();
            if (lyricRow < 0 || lyricRow >= ls.size()) return error("There is no lyric line " + QString::number(lyricRow) + ".");
            QJsonObject r = row("singer", lyricRow);
            if (r.isEmpty()) r = {{"fields", QJsonObject{}}, {"prov", QJsonObject{}}};
            r["line"] = ls[lyricRow].toMap()["text"].toString();
            write("singer", lyricRow, r);
            Result one = setFields("singer", lyricRow, {{"singers", c.value("singers")}}, by, mode);
            if (!one.ok) return one;
            total.skipped += one.skipped;
            QJsonObject now = row("singer", lyricRow);
            if (now["fields"].toObject().isEmpty() && now["prov"].toObject().isEmpty()) write("singer", lyricRow, {});
        }
        return total;
    }
    if (op == "ref.add") return refAdd(c, by);
    if (op == "ref.move") {
        const int id = c.value("id").toInt();
        if (!has("reference", id)) return error("There is no reference " + QString::number(id) + ".");
        const auto r = row("reference", id);
        QList<int> order = refIds(r["owner"].toString(), r["ownerId"].toInt());
        const int to = std::clamp(c.value("position").toInt(), 0, int(order.size()) - 1);
        if (order.indexOf(id) == to) return {};
        order.removeAll(id);
        order.insert(to, id);
        label("Reorder references");
        for (int i = 0; i < order.size(); ++i) {
            QJsonObject x = row("reference", order[i]);
            if (x["position"].toInt() != i) { x["position"] = i; write("reference", order[i], x); }
        }
        return {};
    }
    if (op == "shot.add") return shotAdd(c, by);
    if (op == "shot.retime") return shotRetime(c);
    if (op == "shot.split") {
        const int id = c.value("id").toInt();
        if (!has("shot", id)) return error("There is no shot " + QString::number(id) + ".");
        const auto s = row("shot", id);
        const int at = c.value("at").toInt();
        if (at <= s["start"].toInt() || at >= s["end"].toInt()) return error("Split inside the shot.");
        label("Split shot " + QString::number(shotNumber(id)));
        return shotSplit(id, {at});
    }
    if (op == "shot.splitLines") {
        const int id = c.value("id").toInt();
        if (!has("shot", id)) return error("There is no shot " + QString::number(id) + ".");
        const auto s = row("shot", id);
        const int start = s["start"].toInt(), end = s["end"].toInt();
        const QString route = s["fields"].toObject()["route"].toString();
        const int limit = maxFrames(route);
        if (route.isEmpty()) return error("Choose a route first; the split follows its length limit.");
        if (!limit || end - start <= limit) return error("This shot already fits its route.");
        // Candidate cuts: between consecutive lyric lines, in the middle of any pause.
        QVector<int> candidates;
        const auto ls = lyrics->lines();
        for (int i = 1; i < ls.size(); ++i) {
            const int a = ls[i - 1].toMap()["end"].toInt(), b = ls[i].toMap()["start"].toInt();
            const int f = msToFrame(b > a ? (a + b) / 2 : b);
            if (f > start && f < end) candidates.append(f);
        }
        const int pieces = (end - start + limit - 1) / limit;
        QVector<int> cuts;
        int pos = start;
        for (int k = 1; k < pieces; ++k) {
            const double ideal = start + double(k) * (end - start) / pieces;
            const int lo = std::max(pos + 1, end - (pieces - k) * limit), hi = std::min(pos + limit, end - 1);
            int best = -1;
            for (int f : candidates)
                if (f >= lo && f <= hi && (best < 0 || std::abs(f - ideal) < std::abs(best - ideal))) best = f;
            if (best < 0) best = std::clamp(int(std::lround(ideal)), lo, hi);
            cuts.append(best);
            pos = best;
        }
        label("Split shot " + QString::number(shotNumber(id)) + " at lyric lines");
        return shotSplit(id, cuts);
    }
    if (op == "shot.markCurrent") {
        QVariantList ids = c.value("ids").toList();
        label("Mark shots current");
        const int now = ++clock;
        for (const auto& v : ids) {
            const int id = v.toInt();
            if (!has("shot", id)) return error("There is no shot " + QString::number(id) + ".");
            QJsonObject r = row("shot", id);
            r["written"] = now;
            write("shot", id, r);
        }
        return {};
    }
    return error("Unknown plan command \"" + op + "\".");
}

QString Plan::checkField(const QString& kind, const QString& field, const QJsonValue& v) const {
    const QString type = fieldTable().value(kind).value(field);
    if (type.isEmpty()) return QString("Unknown %1 field \"%2\".").arg(kind, field);
    if (empty(v)) return {};
    if (type == "text") return v.isString() ? QString() : field + " must be text.";
    if (type == "bool") return v.isBool() ? QString() : field + " must be true or false.";
    if (type == "enum") {
        for (const auto& o : optionTable().value(kind == "reference" && field == "role" ? "refRole" : field)) if (v.toString() == o.value) return {};
        return QString("\"%1\" is not a known %2.").arg(v.toString(), field);
    }
    if (type == "texts") {
        if (!v.isArray()) return field + " must be a list of text.";
        for (const auto& t : v.toArray()) if (!t.isString()) return field + " must be a list of text.";
        return {};
    }
    if (type == "who") {
        if (v.toString() == "none") return {};
        if (!v.isArray()) return "who is a list of {character, look}, or \"none\".";
        for (const auto& e : v.toArray()) {
            const auto o = e.toObject();
            if (!has("character", o["character"].toInt())) return "who names a character that does not exist.";
            if (o.contains("look") && !o["look"].isNull()) {
                const auto look = row("look", o["look"].toInt());
                if (look.isEmpty() || look["character"].toInt() != o["character"].toInt())
                    return "A look in who belongs to another character.";
            }
        }
        return {};
    }
    if (type == "where") {
        if (v.toString() == "none") return {};
        return has("location", v.toInt()) ? QString() : "where names a location that does not exist.";
    }
    if (type == "shotref") return has("shot", v.toInt()) ? QString() : "follows names a shot that does not exist.";
    if (type == "singers") {
        if (!v.isArray()) return "singers is a list of character ids.";
        for (const auto& e : v.toArray()) if (!has("character", e.toInt())) return "singers names a character that does not exist.";
        return {};
    }
    if (type == "prompts") {
        if (!v.isObject()) return "prompts maps a route to its prompt.";
        for (auto it = v.toObject().begin(); it != v.toObject().end(); ++it) {
            if (checkField("shot", "route", it.key()).size()) return QString("\"%1\" is not a known route.").arg(it.key());
            if (!it.value().isString()) return "Each prompt is text.";
        }
        return {};
    }
    return {};
}

Plan::Result Plan::setFields(const QString& kind, int id, const QVariantMap& fields, const QString& by, const QString& mode) {
    Result r;
    QJsonObject current = row(kind, id);
    if (current.isEmpty()) current = {{"fields", QJsonObject{}}, {"prov", QJsonObject{}}};
    if (kind == "arc" && !current.contains("section")) current["section"] = sections().value(id).toMap()["label"].toString();
    QJsonObject values = current["fields"].toObject(), prov = current["prov"].toObject();
    bool touched = false;
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        QJsonValue v = QJsonValue::fromVariant(it.value());
        // Ids from QML arrive as doubles; store whole numbers.
        if (v.isDouble()) v = int(std::lround(v.toDouble()));
        if (v.isArray()) {
            QJsonArray a = v.toArray();
            for (int i = 0; i < a.size(); ++i) {
                if (a[i].isDouble()) a[i] = int(std::lround(a[i].toDouble()));
                if (a[i].isObject()) {
                    QJsonObject o = a[i].toObject();
                    for (auto k = o.begin(); k != o.end(); ++k) if (k->isDouble()) *k = int(std::lround(k->toDouble()));
                    a[i] = o;
                }
            }
            v = a;
        }
        if (QString problem = checkField(kind, it.key(), v); !problem.isEmpty()) { r.ok = false; r.error = problem; return r; }
        const QJsonObject p = prov[it.key()].toObject();
        const QJsonValue old = values.value(it.key());
        if (by == "agent") {
            const bool filled = !empty(old);
            if (p["locked"].toBool() || (mode == "fill" && filled) || (mode == "agent" && filled && p["by"].toString() == "user")) {
                if (old != v) r.skipped.append(it.key());
                continue;
            }
        }
        if (empty(v) ? empty(old) : old == v) continue;
        if (empty(v)) values.remove(it.key()); else values[it.key()] = v;
        QJsonObject np{{"by", by}};
        if (p["locked"].toBool()) np["locked"] = true;
        if (empty(v) && !p["locked"].toBool()) prov.remove(it.key()); else prov[it.key()] = np;
        touched = true;
    }
    if (!touched) return r;
    if (kind == "character" || kind == "look" || kind == "location")
        if (values["name"].toString().trimmed().isEmpty()) { r.ok = false; r.error = "Give the " + kind + " a name."; return r; }
    current["fields"] = values;
    current["prov"] = prov;
    current["edited"] = ++clock;
    if (kind == "shot") current["written"] = clock;
    write(kind, id, current);
    return r;
}

QList<int> Plan::refIds(const QString& owner, int id) const {
    QList<int> ids;
    const auto& refs = items.value("reference");
    for (auto it = refs.begin(); it != refs.end(); ++it)
        if (it.value()["owner"].toString() == owner && it.value()["ownerId"].toInt() == id) ids.append(it.key());
    std::sort(ids.begin(), ids.end(), [&](int a, int b) {
        const int pa = refs[a]["position"].toInt(), pb = refs[b]["position"].toInt();
        return pa != pb ? pa < pb : a < b;
    });
    return ids;
}

void Plan::dropReferences(const QString& owner, int id) {
    for (int ref : refIds(owner, id)) write("reference", ref, {});
}

Plan::Result Plan::refAdd(const QVariantMap& c, const QString& by) {
    Result r;
    auto fail = [&](const QString& e) { r.ok = false; r.error = e; return r; };
    const QString owner = c.value("owner").toString();
    const int ownerId = c.value("ownerId").toInt();
    if ((owner != "character" && owner != "look" && owner != "location") || !has(owner, ownerId))
        return fail("A reference belongs to an existing character, look or location.");
    const QString given = c.value("path").toString();
    const QString source = given.startsWith("file:") ? QUrl(given).toLocalFile() : given;
    QFile file(source);
    if (!QFileInfo(source).isFile() || !file.open(QIODevice::ReadOnly)) return fail("Cannot read " + QFileInfo(source).fileName() + ".");
    QImageReader reader(source);
    const QSize size = reader.size();
    if (!reader.canRead() || size.isEmpty()) return fail(QFileInfo(source).fileName() + " is not an image Chorus Cut can read.");
    // Copied in under a name from its contents, so the project stays self-contained and the
    // same picture added twice is stored once.
    QCryptographicHash hash(QCryptographicHash::Sha1);
    if (!hash.addData(&file)) return fail("Cannot read " + QFileInfo(source).fileName() + ".");
    file.close();
    QString suffix = QFileInfo(source).suffix().toLower();
    if (suffix.isEmpty()) suffix = QString::fromLatin1(reader.format()).toLower();
    const QString relative = "Media/References/" + QString::fromLatin1(hash.result().toHex().left(20)) + "." + suffix;
    const QString target = project->dir.filePath(relative);
    if (!QFileInfo::exists(target)) {
        if (!project->dir.mkpath("Media/References")) return fail("Cannot create Media/References in the project.");
        const QString partial = target + ".part";
        QFile::remove(partial);
        if (!QFile::copy(source, partial) || !QFile::rename(partial, target)) {
            QFile::remove(partial);
            return fail("Could not copy " + QFileInfo(source).fileName() + " into the project.");
        }
    }
    const QList<int> siblings = refIds(owner, ownerId);
    for (int existing : siblings)
        if (row("reference", existing)["file"].toString() == relative) return fail(QFileInfo(source).fileName() + " is already a reference here.");
    QString role = c.value("role").toString();
    if (role.isEmpty()) role = owner == "location" ? "setting" : owner == "look" ? "look" : siblings.isEmpty() ? "face" : "body";
    const int id = nextId("reference");
    const int position = siblings.isEmpty() ? 0 : row("reference", siblings.last())["position"].toInt() + 1;
    write("reference", id, {{"owner", owner}, {"ownerId", ownerId}, {"file", relative}, {"name", QFileInfo(source).fileName()},
                            {"width", size.width()}, {"height", size.height()}, {"position", position},
                            {"source", by == "user" ? "imported" : "agent"}, {"fields", QJsonObject{}}, {"prov", QJsonObject{}}, {"edited", ++clock}});
    if (pendingLabel.isEmpty()) pendingLabel = "Add reference";
    // Pictures the user brings are approved; ones an agent adds wait for the user.
    r = setFields("reference", id, {{"role", role}, {"approved", by == "user"}}, by, "replace");
    r.id = id;
    return r;
}

QString Plan::overlap(int track, int start, int end, int except) const {
    const auto& shots = items.value("shot");
    for (auto it = shots.begin(); it != shots.end(); ++it) {
        if (it.key() == except || it.value()["track"].toInt() != track) continue;
        if (start < it.value()["end"].toInt() && it.value()["start"].toInt() < end)
            return QString("That overlaps shot %1 on V%2.").arg(shotNumber(it.key())).arg(track);
    }
    return {};
}

Plan::Result Plan::shotAdd(const QVariantMap& c, const QString& by) {
    Result r;
    const int track = c.value("track", 1).toInt(), start = c.value("start").toInt(), end = c.value("end").toInt();
    auto fail = [&](const QString& e) { r.ok = false; r.error = e; return r; };
    if (track != 1 && track != 2) return fail("Shots go on V1 or V2.");
    if (start < 0 || end <= start || end > songFrames()) return fail("A shot needs a start before its end, within the song.");
    if (QString e = overlap(track, start, end, -1); !e.isEmpty()) return fail(e);
    const int id = nextId("shot");
    write("shot", id, {{"track", track}, {"start", start}, {"end", end}, {"fields", QJsonObject{}}, {"prov", QJsonObject{}},
                       {"edited", ++clock}, {"written", clock}});
    if (pendingLabel.isEmpty()) pendingLabel = "Add shot";
    r = setFields("shot", id, c.value("fields").toMap(), by, "replace");
    r.id = id;
    return r;
}

Plan::Result Plan::shotRetime(const QVariantMap& c) {
    Result r;
    auto fail = [&](const QString& e) { r.ok = false; r.error = e; return r; };
    const int id = c.value("id").toInt();
    if (!has("shot", id)) return fail("There is no shot " + QString::number(id) + ".");
    QJsonObject s = row("shot", id);
    const int track = c.value("track", s["track"].toInt()).toInt();
    const int start = c.value("start", s["start"].toInt()).toInt(), end = c.value("end", s["end"].toInt()).toInt();
    if (track != 1 && track != 2) return fail("Shots go on V1 or V2.");
    if (start < 0 || end <= start || end > songFrames()) return fail("A shot needs a start before its end, within the song.");
    if (QString e = overlap(track, start, end, id); !e.isEmpty()) return fail(e);
    const bool moved = end - start == s["end"].toInt() - s["start"].toInt();
    if (pendingLabel.isEmpty()) pendingLabel = (moved ? "Move shot " : "Trim shot ") + QString::number(shotNumber(id));
    if (track == s["track"].toInt() && start == s["start"].toInt() && end == s["end"].toInt()) return r;
    s["track"] = track; s["start"] = start; s["end"] = end;
    write("shot", id, s);
    r.id = id;
    return r;
}

Plan::Result Plan::shotSplit(int id, const QVector<int>& cuts) {
    QJsonObject s = row("shot", id);
    const int end = s["end"].toInt();
    QVector<int> bounds = cuts;
    bounds.append(end);
    s["end"] = bounds.first();
    write("shot", id, s);
    Result r;
    r.id = id;
    for (int i = 0; i + 1 < bounds.size(); ++i) {
        QJsonObject piece = s;
        piece["start"] = bounds[i];
        piece["end"] = bounds[i + 1];
        // A copy has no continuity of its own yet.
        write("shot", nextId("shot"), piece);
    }
    return r;
}

int Plan::maxFrames(const QString& route) const {
    if (route.isEmpty() || route == "still") return 0;
    return int(std::floor(kH3MaxSeconds * fps() + 1e-6));
}

QList<int> Plan::shotOrder() const {
    QList<int> ids = items.value("shot").keys();
    const auto& shots = items.value("shot");
    std::sort(ids.begin(), ids.end(), [&](int a, int b) {
        const auto& x = shots[a]; const auto& y = shots[b];
        if (x["start"].toInt() != y["start"].toInt()) return x["start"].toInt() < y["start"].toInt();
        if (x["track"].toInt() != y["track"].toInt()) return x["track"].toInt() < y["track"].toInt();
        return a < b;
    });
    return ids;
}

int Plan::shotNumber(int id) const { return shotOrder().indexOf(id) + 1; }

QString Plan::nameOf(const QString& kind, int id) const {
    const auto r = row(kind, id);
    const QString name = r["fields"].toObject()["name"].toString();
    if (kind == "concept") return "concept";
    if (kind == "shot") return "shot " + QString::number(shotNumber(id));
    if (kind == "arc") return "arc for " + sections().value(id).toMap()["label"].toString();
    if (kind == "singer") return "singer";
    if (kind == "reference") return "reference " + r["name"].toString();
    return name.isEmpty() ? kind : name;
}

QVariantMap Plan::exported(const QString& kind, int id) const {
    const auto r = row(kind, id);
    QVariantMap m = r.toVariantMap();
    m["id"] = id;
    if (!m.contains("fields")) m["fields"] = QVariantMap{};
    if (!m.contains("prov")) m["prov"] = QVariantMap{};
    if (kind == "reference" && project) {
        const QString path = project->dir.filePath(r["file"].toString());
        m["url"] = QUrl::fromLocalFile(path).toString();
        m["missing"] = !QFileInfo::exists(path);
        m["approved"] = r["fields"].toObject()["approved"].toBool();
        m["role"] = r["fields"].toObject()["role"].toString();
        m["roleLabel"] = optionLabel("refRole", m["role"].toString());
    }
    return m;
}

QVariantList Plan::listOf(const QString& kind) const {
    QVariantList out;
    for (int id : items.value(kind).keys()) out.append(exported(kind, id));
    return out;
}

QVariantMap Plan::concept() const { return exported("concept", 1); }

QVariantList Plan::sections() const {
    // Song sections in order: the lyric [Heading] sections and the detected instrumental parts.
    QVariantList list;
    for (const auto& v : lyrics->regions()) {
        auto m = v.toMap();
        list.append(QVariantMap{{"label", m["label"]}, {"start", m["start"]}, {"end", m["end"]}, {"kind", "section"}});
    }
    for (const auto& v : lyrics->instrumental()) {
        auto m = v.toMap();
        const QString k = m["kind"].toString();
        list.append(QVariantMap{{"label", k == "intro" ? "Instrumental intro" : k == "outro" ? "Instrumental outro" : "Instrumental"},
                                {"start", m["start"]}, {"end", m["end"]}, {"kind", "instrumental"}});
    }
    std::stable_sort(list.begin(), list.end(), [](const QVariant& a, const QVariant& b) {
        return a.toMap()["start"].toInt() < b.toMap()["start"].toInt();
    });
    if (list.isEmpty() && project && songFrames() > 0)
        list.append(QVariantMap{{"label", "Whole song"}, {"start", 0}, {"end", frameToMs(songFrames())}, {"kind", "song"}});
    for (int i = 0; i < list.size(); ++i) { auto m = list[i].toMap(); m["ordinal"] = i; list[i] = m; }
    return list;
}

QVariantList Plan::arc() const {
    QVariantList out;
    const auto list = sections();
    for (const auto& v : list) {
        QVariantMap m = v.toMap();
        const int ordinal = m["ordinal"].toInt();
        const auto r = row("arc", ordinal);
        m["fields"] = r["fields"].toObject().toVariantMap();
        m["prov"] = r["prov"].toObject().toVariantMap();
        m["written"] = r["section"].toString();
        m["mismatch"] = !r.isEmpty() && r["section"].toString() != m["label"].toString();
        out.append(m);
    }
    return out;
}

QVariantList Plan::singers() const {
    QVariantList out;
    const auto ls = lyrics->lines();
    for (int i = 0; i < ls.size(); ++i) {
        const auto r = row("singer", i);
        const auto line = ls[i].toMap();
        out.append(QVariantMap{{"row", i}, {"text", line["text"]},
                               {"singers", r["fields"].toObject()["singers"].toArray().toVariantList()},
                               {"prov", r["prov"].toObject().toVariantMap()},
                               {"stale", !r.isEmpty() && r["line"].toString() != line["text"].toString()}});
    }
    return out;
}

QVariantMap Plan::shot(int id) const {
    if (!has("shot", id)) return {};
    QVariantMap m = exported("shot", id);
    const auto r = row("shot", id);
    const auto f = r["fields"].toObject();
    m["number"] = shotNumber(id);
    m["startMs"] = frameToMs(r["start"].toInt());
    m["endMs"] = frameToMs(r["end"].toInt());
    QStringList who;
    if (f["who"].toString() == "none") who << "No one";
    for (const auto& e : f["who"].toArray()) who << nameOf("character", e.toObject()["character"].toInt());
    QStringList parts{QString::number(m["number"].toInt())};
    if (f.contains("framing")) parts << f["framing"].toString().toUpper();
    if (!who.isEmpty()) parts << who.join(", ");
    m["label"] = parts.join(" · ");
    m["typeLabel"] = optionLabel("type", f["type"].toString());
    int errors = 0, warnings = 0;
    bool stale = false;
    for (const auto& v : checks()) {
        const auto c = v.toMap();
        if (!c["shots"].toList().contains(id)) continue;
        if (c["code"] == "stale") stale = true;
        if (c["level"] == "warning") ++warnings; else ++errors;
    }
    m["problems"] = errors;
    m["advice"] = warnings;
    m["stale"] = stale;
    return m;
}

QVariantList Plan::shots() const {
    QVariantList out;
    for (int id : shotOrder()) out.append(shot(id));
    return out;
}

QVariantList Plan::references() const {
    QVariantList out;
    for (const QString owner : {"character", "look", "location"})
        for (int id : items.value(owner).keys())
            for (int ref : refIds(owner, id)) out.append(exported("reference", ref));
    return out;
}

QVariantList Plan::referencesFor(const QString& owner, int id) const {
    QVariantList out;
    for (int ref : refIds(owner, id)) out.append(exported("reference", ref));
    return out;
}

QVariantList Plan::shotReferences(int id) const {
    QVariantList out;
    if (!has("shot", id)) return out;
    const auto f = row("shot", id)["fields"].toObject();
    auto add = [&](const QString& owner, int ownerId) {
        for (int ref : refIds(owner, ownerId)) {
            QVariantMap m = exported("reference", ref);
            if (!m["approved"].toBool()) continue;
            m["ownerName"] = nameOf(owner, ownerId);
            out.append(m);
        }
    };
    for (const auto& e : f["who"].toArray()) {
        const auto o = e.toObject();
        if (o.contains("look") && !o["look"].isNull()) add("look", o["look"].toInt());
        add("character", o["character"].toInt());
    }
    if (f["where"].isDouble()) add("location", f["where"].toInt());
    return out;
}

QVariantList Plan::linesIn(int start, int end) const {
    QVariantList out;
    const int a = frameToMs(start), b = frameToMs(end);
    const auto ls = lyrics->lines();
    const auto who = singers();
    for (int i = 0; i < ls.size(); ++i) {
        const auto l = ls[i].toMap();
        if (l["start"].toInt() < b && a < l["end"].toInt()) {
            QStringList names;
            for (const auto& s : who[i].toMap()["singers"].toList()) names << nameOf("character", s.toInt());
            out.append(QVariantMap{{"row", i}, {"text", l["text"]}, {"start", l["start"]}, {"end", l["end"]}, {"singers", names}});
        }
    }
    return out;
}

QVariantMap Plan::options() const {
    QVariantMap out;
    for (auto it = optionTable().begin(); it != optionTable().end(); ++it) {
        QVariantList list;
        for (const auto& o : it.value()) {
            QVariantMap m{{"value", o.value}, {"label", o.label}};
            if (it.key() == "route") m["maxSeconds"] = QString(o.value) == "still" ? 0.0 : kH3MaxSeconds;
            list.append(m);
        }
        out[it.key()] = list;
    }
    out["required"] = kRequired;
    return out;
}

QVariantMap Plan::schema() const {
    QVariantMap kinds;
    for (auto k = fieldTable().begin(); k != fieldTable().end(); ++k) {
        QVariantMap fields;
        for (auto f = k.value().begin(); f != k.value().end(); ++f) {
            QVariantMap spec{{"type", f.value()}};
            if (f.value() == "enum") {
                QVariantList values;
                for (const auto& o : optionTable().value(k.key() == "reference" && f.key() == "role" ? "refRole" : f.key()))
                    values.append(QVariantMap{{"value", o.value}, {"label", o.label}});
                spec["options"] = values;
            }
            fields[f.key()] = spec;
        }
        kinds[k.key()] = fields;
    }
    return kinds;
}

QVariantList Plan::checks() const {
    if (!checksValid) { cachedChecks = computeChecks(); checksValid = true; }
    return cachedChecks;
}
int Plan::errorCount() const {
    int n = 0;
    for (const auto& c : checks()) n += c.toMap()["level"] != "warning";
    return n;
}
int Plan::warningCount() const {
    int n = 0;
    for (const auto& c : checks()) n += c.toMap()["level"] == "warning";
    return n;
}
QVariantList Plan::checksFor(int id) const {
    QVariantList out;
    for (const auto& c : checks()) if (c.toMap()["shots"].toList().contains(id)) out.append(c);
    return out;
}

QVariantList Plan::gates() const {
    struct Gate { const char* key; const char* label; };
    const Gate list[] = {{"G1", "Song"}, {"G2", "Concept"}, {"G3", "References"}, {"G4", "Shot list"}};
    QVariantList out;
    const auto all = checks();
    for (const auto& g : list) {
        int open = 0;
        for (const auto& c : all) {
            const auto m = c.toMap();
            open += m["gate"] == g.key && m["level"] != "warning";
        }
        out.append(QVariantMap{{"gate", g.key}, {"label", g.label}, {"pass", open == 0}, {"open", open}});
    }
    return out;
}

QVariantList Plan::coverage() const {
    // Covered and uncovered spans of the song, in ms, for the ruler. Empty until there is a shot.
    QVariantList out;
    const auto& shots = items.value("shot");
    if (shots.isEmpty() || !project) return out;
    QVector<QPair<int, int>> spans;
    for (const auto& s : shots) spans.append({s["start"].toInt(), s["end"].toInt()});
    std::sort(spans.begin(), spans.end());
    int pos = 0;
    auto add = [&](int a, int b, bool covered) {
        if (b > a) out.append(QVariantMap{{"start", frameToMs(a)}, {"end", frameToMs(b)}, {"covered", covered}});
    };
    for (const auto& s : spans) {
        if (s.first > pos) add(pos, s.first, false);
        if (s.second > pos) { add(std::max(pos, s.first), s.second, true); pos = s.second; }
    }
    add(pos, songFrames(), false);
    return out;
}

QVariantList Plan::computeChecks() const {
    QVariantList out;
    if (!project) return out;
    // level: "error" (wrong), "todo" (unfinished; holds its gate) or "warning" (advice).
    auto add = [&](const QString& level, const QString& gate, const QString& code, const QString& text,
                   int startMs, int endMs, const QList<int>& shots = {}, const QString& fix = {}) {
        QVariantList ids;
        for (int s : shots) ids.append(s);
        out.append(QVariantMap{{"level", level}, {"gate", gate}, {"code", code}, {"message", text},
                               {"start", startMs}, {"end", endMs}, {"shots", ids}, {"fix", fix}});
    };
    const auto ls = lyrics->lines();
    const auto who = singers();
    const auto& shotRows = items.value("shot");
    const QList<int> order = shotOrder();
    auto number = [&](int id) { return order.indexOf(id) + 1; };
    auto shotMs = [&](int id) { return QPair<int, int>{frameToMs(shotRows[id]["start"].toInt()), frameToMs(shotRows[id]["end"].toInt())}; };
    auto characterName = [&](int id) { return nameOf("character", id); };
    auto lipsync = [&](int id) { return shotRows[id]["fields"].toObject()["type"].toString() == "lipsync"; };

    // ---- G1 Song
    if (ls.isEmpty())
        add("todo", "G1", "lyrics", lyrics->text().trimmed().isEmpty() ? "The song has no lyrics yet." : "The lyrics are not aligned yet.", 0, 0);
    int flagged = 0, firstFlag = -1;
    for (int i = 0; i < ls.size(); ++i) if (ls[i].toMap()["flag"].toBool()) { ++flagged; if (firstFlag < 0) firstFlag = i; }
    if (flagged)
        add("todo", "G1", "flagged", QString("%1 lyric %2 still %3 a listen.").arg(flagged).arg(flagged == 1 ? "line" : "lines").arg(flagged == 1 ? "needs" : "need"),
            ls[firstFlag].toMap()["start"].toInt(), ls[firstFlag].toMap()["end"].toInt());
    int unsung = 0, firstUnsung = -1;
    for (int i = 0; i < ls.size(); ++i) {
        const auto s = who[i].toMap();
        if (s["stale"].toBool())
            add("todo", "G1", "singerStale", QString("The singer of line %1 was set before its words changed: \"%2\".").arg(i + 1).arg(s["text"].toString()),
                ls[i].toMap()["start"].toInt(), ls[i].toMap()["end"].toInt());
        else if (s["singers"].toList().isEmpty()) { ++unsung; if (firstUnsung < 0) firstUnsung = i; }
    }
    if (unsung)
        add("todo", "G1", "singers", unsung == ls.size() ? "No lyric line has a singer yet."
                                     : QString("%1 sung %2 no singer.").arg(unsung).arg(unsung == 1 ? "line has" : "lines have"),
            ls[firstUnsung].toMap()["start"].toInt(), ls[firstUnsung].toMap()["end"].toInt());

    // ---- G2 Concept
    const auto concept = row("concept", 1)["fields"].toObject();
    QStringList missing;
    if (empty(concept["videoKind"])) missing << "the kind of video";
    if (empty(concept["aspect"])) missing << "the aspect ratio";
    if (empty(concept["logline"])) missing << "a logline";
    if (empty(concept["style"])) missing << "a visual style";
    if (!missing.isEmpty()) add("todo", "G2", "concept", "The concept needs " + joinNames(missing) + ".", 0, 0);
    QStringList noArc;
    int firstNoArc = -1;
    for (const auto& v : arc()) {
        const auto a = v.toMap();
        if (a["mismatch"].toBool())
            add("error", "G2", "arcOrder", QString("The arc entry written for \"%1\" now falls on \"%2\"; the song's sections changed.")
                .arg(a["written"].toString(), a["label"].toString()), a["start"].toInt(), a["end"].toInt());
        else if (a["fields"].toMap().value("happens").toString().trimmed().isEmpty()) {
            noArc << a["label"].toString();
            if (firstNoArc < 0) firstNoArc = a["ordinal"].toInt();
        }
    }
    if (!noArc.isEmpty()) {
        const auto first = sections().value(firstNoArc).toMap();
        add("todo", "G2", "arc", noArc.size() == sections().size() ? "The arc is empty; say what happens in each section."
                                 : "The arc has nothing for " + joinNames(noArc.mid(0, 4)) + (noArc.size() > 4 ? QString(" and %1 more").arg(noArc.size() - 4) : "") + ".",
            first["start"].toInt(), first["end"].toInt());
    }

    // ---- G3 References: everyone and everywhere a shot uses has an approved picture.
    {
        QMap<QPair<QString, int>, QList<int>> uses;  // (kind, id) -> shots
        for (int id : order) {
            const auto f = shotRows[id]["fields"].toObject();
            for (const auto& e : f["who"].toArray()) {
                const auto o = e.toObject();
                if (has("character", o["character"].toInt())) uses[{"character", o["character"].toInt()}] << id;
                if (o.contains("look") && !o["look"].isNull() && has("look", o["look"].toInt())) uses[{"look", o["look"].toInt()}] << id;
            }
            if (f["where"].isDouble() && has("location", f["where"].toInt())) uses[{"location", f["where"].toInt()}] << id;
        }
        for (auto it = uses.begin(); it != uses.end(); ++it) {
            const auto& [kind, id] = it.key();
            bool approved = false;
            for (int ref : refIds(kind, id)) approved |= row("reference", ref)["fields"].toObject()["approved"].toBool();
            if (approved) continue;
            QList<int> shots = it.value();
            shots.erase(std::unique(shots.begin(), shots.end()), shots.end());
            QStringList numbers;
            for (int s : shots) numbers << QString::number(number(s));
            const QString what = kind == "look" ? QString("The look \u201c%1\u201d").arg(nameOf(kind, id)) : nameOf(kind, id);
            add("todo", "G3", "reference", QString("%1 has no approved reference picture; %2 %3 %4 it.")
                .arg(what, shots.size() == 1 ? "shot" : "shots", joinNames(numbers), shots.size() == 1 ? "uses" : "use"),
                shotMs(shots.first()).first, shotMs(shots.last()).second, shots);
        }
        for (int ref : items.value("reference").keys()) {
            const auto r = row("reference", ref);
            if (!QFileInfo::exists(project->dir.filePath(r["file"].toString())))
                add("error", "G3", "referenceMissing", QString("The reference picture %1 for %2 is missing from the project's Media/References folder.")
                    .arg(r["name"].toString(), nameOf(r["owner"].toString(), r["ownerId"].toInt())), 0, 0);
        }
    }

    // ---- G4 Shot list
    if (shotRows.isEmpty()) add("todo", "G4", "noShots", "No shots yet. Select a lyric line or range and add a shot.", 0, 0);
    int gaps = 0;
    for (const auto& v : coverage()) {
        const auto g = v.toMap();
        if (g["covered"].toBool()) continue;
        if (++gaps > 12) continue;
        const int a = msToFrame(g["start"].toInt()), b = msToFrame(g["end"].toInt());
        add("todo", "G4", "gap", QString("No shot covers %1 to %2.").arg(clockText(a), clockText(b)), g["start"].toInt(), g["end"].toInt());
    }
    if (gaps > 12) add("todo", "G4", "gap", QString("%1 more gaps in the picture.").arg(gaps - 12), 0, 0);

    for (int id : order) {
        const auto s = shotRows[id];
        const auto f = s["fields"].toObject();
        const auto [a, b] = shotMs(id);
        const int n = number(id);
        const QString type = f["type"].toString();
        // Required core.
        QStringList need;
        for (const auto& key : (type == "gap" ? QStringList{"type", "purpose"} : kRequired))
            if (empty(f[key])) need << kFieldNames.value(key);
        if (!need.isEmpty())
            add("todo", "G4", "fields", QString("Shot %1 needs %2.").arg(n).arg(joinNames(need)), a, b, {id});
        // Length against the route.
        const int limit = maxFrames(f["route"].toString());
        const int frames = s["end"].toInt() - s["start"].toInt();
        if (limit && frames > limit)
            add("error", "G4", "tooLong", QString("Shot %1 is %2, but %3 renders at most %4.").arg(n).arg(seconds(frames / fps()))
                .arg(optionLabel("route", f["route"].toString()).startsWith("H3") ? "H3" : "the route").arg(seconds(limit / fps())),
                a, b, {id}, "splitLines");
        // H3 reference-to-video takes at most nine pictures.
        if (f["route"].toString() == "ref2v" && shotReferences(id).size() > 9)
            add("error", "G4", "tooManyRefs", QString("Shot %1 has %2 reference pictures, but reference-to-video takes 9.").arg(n).arg(shotReferences(id).size()), a, b, {id});
        // References that no longer exist.
        for (const auto& e : f["who"].toArray()) {
            const auto o = e.toObject();
            if (!has("character", o["character"].toInt()))
                add("error", "G4", "missingRef", QString("Shot %1 shows a character that was removed.").arg(n), a, b, {id});
            else if (o.contains("look") && !o["look"].isNull() &&
                     (!has("look", o["look"].toInt()) || row("look", o["look"].toInt())["character"].toInt() != o["character"].toInt()))
                add("error", "G4", "missingRef", QString("Shot %1 gives %2 a look that was removed.").arg(n).arg(characterName(o["character"].toInt())), a, b, {id});
        }
        if (f["where"].isDouble() && !has("location", f["where"].toInt()))
            add("error", "G4", "missingRef", QString("Shot %1 is set in a location that was removed.").arg(n), a, b, {id});
        if (f.contains("follows") && !has("shot", f["follows"].toInt()))
            add("warning", "G4", "missingRef", QString("Shot %1 follows a shot that was removed.").arg(n), a, b, {id});
        // Lip-sync needs words to sync to, and someone to sing them.
        if (type == "lipsync") {
            bool sung = false;
            for (const auto& lv : ls)
                for (const auto& wv : lv.toMap()["words"].toList()) {
                    const auto w = wv.toMap();
                    sung |= w["start"].toInt() < b && a < w["end"].toInt();
                }
            if (!sung)
                add("error", "G4", "lipsyncSilent", QString("Shot %1 is lip-sync, but nothing is sung during it.").arg(n), a, b, {id});
            if (f["who"].toString() == "none")
                add("error", "G4", "lipsyncEmpty", QString("Lip-sync shot %1 has no one in frame.").arg(n), a, b, {id});
        }
    }
    // A line's singer must appear in at least one lip-sync shot over it.
    for (int i = 0; i < ls.size(); ++i) {
        const auto line = ls[i].toMap();
        const QVariantList sing = who[i].toMap()["singers"].toList();
        if (sing.isEmpty() || who[i].toMap()["stale"].toBool()) continue;
        const int a = line["start"].toInt(), b = line["end"].toInt();
        QList<int> over;
        bool shown = false;
        for (int id : order) {
            if (!lipsync(id)) continue;
            const auto [sa, sb] = shotMs(id);
            if (!(sa < b && a < sb)) continue;
            over << id;
            for (const auto& e : shotRows[id]["fields"].toObject()["who"].toArray())
                shown |= sing.contains(e.toObject()["character"].toInt());
        }
        if (!over.isEmpty() && !shown) {
            QStringList names, shotNames;
            for (const auto& s : sing) names << characterName(s.toInt());
            for (int id : over) shotNames << QString::number(number(id));
            add("error", "G4", "singer", QString(over.size() == 1 ? "\"%1\" is sung by %2, but lip-sync shot %3 shows someone else."
                                                                  : "\"%1\" is sung by %2, but lip-sync shots %3 show someone else.")
                .arg(line["text"].toString(), joinNames(names), joinNames(shotNames)), a, b, over);
        }
    }
    // Advice: long runs of one framing, in picture order.
    for (int i = 0; i < order.size();) {
        const QString framing = shotRows[order[i]]["fields"].toObject()["framing"].toString();
        int j = i + 1;
        while (j < order.size() && !framing.isEmpty() && shotRows[order[j]]["fields"].toObject()["framing"].toString() == framing) ++j;
        if (!framing.isEmpty() && j - i >= 4) {
            QList<int> run = order.mid(i, j - i);
            add("warning", "G4", "framing", QString("Shots %1 to %2 are all %3s.").arg(i + 1).arg(j).arg(optionLabel("framing", framing).toLower()),
                shotMs(order[i]).first, shotMs(order[j - 1]).second, run);
        }
        i = j;
    }
    // Advice: shots written before something upstream changed.
    const int conceptEdited = row("concept", 1)["edited"].toInt();
    QList<int> stale;
    for (int id : order) {
        const auto s = shotRows[id];
        const int written = s["written"].toInt();
        if (s["fields"].toObject().isEmpty()) continue;
        int upstream = conceptEdited;
        const auto f = s["fields"].toObject();
        for (const auto& e : f["who"].toArray()) {
            upstream = std::max(upstream, row("character", e.toObject()["character"].toInt())["edited"].toInt());
            if (e.toObject().contains("look")) upstream = std::max(upstream, row("look", e.toObject()["look"].toInt())["edited"].toInt());
        }
        if (f["where"].isDouble()) upstream = std::max(upstream, row("location", f["where"].toInt())["edited"].toInt());
        const auto [a, b] = shotMs(id);
        for (const auto& v : sections()) {
            const auto sec = v.toMap();
            if (sec["start"].toInt() < b && a < sec["end"].toInt())
                upstream = std::max(upstream, row("arc", sec["ordinal"].toInt())["edited"].toInt());
        }
        if (upstream > written) stale << id;
    }
    if (!stale.isEmpty()) {
        QStringList names;
        for (int id : stale) names << QString::number(number(id));
        add("warning", "G4", "stale", QString("The concept, cast, locations or arc changed after %1 %2 %3 written. Review %4, then mark %4 current.")
            .arg(stale.size() == 1 ? "shot" : "shots", joinNames(names), stale.size() == 1 ? "was" : "were", stale.size() == 1 ? "it" : "them"),
            shotMs(stale.first()).first, shotMs(stale.last()).second, stale, "markCurrent");
    }
    return out;
}
