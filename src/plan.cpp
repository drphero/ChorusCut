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
        {"refRole", {{"face", "Face"}, {"body", "Full body"}, {"look", "Look"}, {"setting", "Setting"}, {"object", "Object"}, {"detail", "Detail"}, {"sheet", "Sheet"}, {"other", "Other"}}},
        // How a view's pictures are made: drawn from the subject, or edited from pictures the user picks.
        {"viewFrom", {{"prompt", "From the prompt"}, {"pictures", "From pictures"}}},
        // What a look may change about its character (everything is kept unless listed).
        {"lookChanges", {{"face", "Face"}, {"hair", "Hair"}, {"build", "Build"}, {"skin", "Skin"}}},
        {"route", {{"h3-lipsync", "H3 lip-sync with the song"}, {"i2v", "Still, then image-to-video"}, {"flf", "First and last frame"},
                   {"t2v", "Text-to-video"}, {"ref2v", "Reference-to-video"}, {"still", "Still only"}}},
    };
    return table;
}
// Which option list an enum field takes its values from.
QString optionKey(const QString& kind, const QString& field) {
    if ((kind == "reference" || kind == "view") && field == "role") return "refRole";
    if (kind == "view" && field == "from") return "viewFrom";
    return field;
}
// A new item's first view, and the one older projects' pictures move into.
struct ViewSpec { const char* name; const char* framing; const char* role; const char* from; };
ViewSpec firstView(const QString& kind) {
    if (kind == "look") return {"Full body", "Full body, head to shoes, standing front-on, plain neutral grey backdrop, soft even light", "look", "pictures"};
    if (kind == "location") return {"Wide", "Wide establishing view at eye level, empty of people, in the video's aspect ratio", "setting", "prompt"};
    if (kind == "prop") return {"Object", "The object alone, centred, three-quarter view from slightly above, plain neutral grey backdrop, soft even light", "object", "prompt"};
    return {"Face", "Front view, face and shoulders, looking at the camera, plain neutral grey backdrop, soft even light", "face", "prompt"};
}
QString roleViewName(const QString& role) {
    if (role == "face") return "Face";
    if (role == "body" || role == "look") return "Full body";
    if (role == "setting") return "Wide";
    if (role == "object") return "Object";
    if (role == "detail") return "Detail";
    if (role == "sheet") return "Sheet";
    return "Other";
}
QString optionLabel(const QString& key, const QString& value) {
    for (const auto& o : optionTable().value(key)) if (value == o.value) return o.label;
    return value;
}

// The fields each kind accepts: "text", "texts" (a list of strings), "enum" (named in optionTable),
// "who", "where", "props" (prop ids), "shotref", "singers", "prompts", "aspects" (a list of lookChanges), "refs"
// (reference ids) or "views" (view ids).
const QMap<QString, QMap<QString, QString>>& fieldTable() {
    static const QMap<QString, QMap<QString, QString>> table = {
        {"concept", {{"brief", "text"}, {"logline", "text"}, {"treatment", "text"}, {"videoKind", "enum"}, {"mix", "text"}, {"aspect", "enum"},
                     {"resolution", "text"}, {"style", "text"}, {"palette", "text"}, {"lighting", "text"}, {"era", "text"},
                     {"lens", "text"}, {"grade", "text"}, {"rules", "texts"}}},
        // facePrompt: what a face refine pass tells the video model about this person's face.
        {"character", {{"name", "text"}, {"role", "enum"}, {"identity", "text"}, {"voice", "text"}, {"notes", "text"}, {"prompt", "text"}, {"facePrompt", "text"}}},
        {"look", {{"name", "text"}, {"description", "text"}, {"prompt", "text"}, {"changes", "aspects"}}},
        {"location", {{"name", "text"}, {"description", "text"}, {"timeOfDay", "enum"}, {"lighting", "text"}, {"prompt", "text"}}},
        // An object that matters on screen (a guitar, a letter, a car): seen in shots that list it.
        {"prop", {{"name", "text"}, {"description", "text"}, {"prompt", "text"}, {"notes", "text"}}},
        {"arc", {{"happens", "text"}, {"beat", "text"}, {"density", "enum"}}},
        {"singer", {{"singers", "singers"}}},
        {"reference", {{"role", "enum"}, {"approved", "bool"}, {"notes", "text"}}},
        // A video generated for a shot; the approved one is the take the cut uses.
        {"take", {{"approved", "bool"}, {"notes", "text"}}},
        // One way of seeing a character, look or location. Its pictures are its references; the
        // approved one is its chosen picture. inputs: the pictures an edited view is made from, in
        // order; panels: the views a composed sheet lays side by side.
        {"view", {{"name", "text"}, {"framing", "text"}, {"role", "enum"}, {"from", "enum"}, {"inputs", "refs"}, {"panels", "views"}}},
        {"shot", {{"type", "enum"}, {"purpose", "text"}, {"who", "who"}, {"where", "where"}, {"props", "props"}, {"timeOfDay", "enum"},
                  {"framing", "enum"}, {"angle", "enum"}, {"lens", "text"}, {"move", "enum"}, {"composition", "text"},
                  {"action", "text"}, {"emotion", "text"}, {"energy", "enum"}, {"follows", "shotref"}, {"route", "enum"},
                  {"prompts", "prompts"}, {"pictures", "refs"}, {"workflow", "text"}, {"notes", "text"}}},
    };
    return table;
}
// A shot is fully planned with these (decision 7); a deliberate gap needs only its reason.
const QStringList kRequired = {"type", "purpose", "who", "where", "framing", "move", "route"};
const QMap<QString, QString> kFieldNames = {
    {"type", "type"}, {"purpose", "purpose"}, {"who", "who is in frame"}, {"where", "location"},
    {"framing", "framing"}, {"move", "camera move"}, {"route", "route"}};

// What has views and pictures of its own.
bool hasPictures(const QString& kind) { return kind == "character" || kind == "look" || kind == "location" || kind == "prop"; }

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
// Shot numbers for a message: all of them up to five, else the first four "and N more".
QString shotNumbers(const QStringList& numbers) {
    if (numbers.size() <= 5) return joinNames(numbers);
    return numbers.mid(0, 4).join(", ") + QString(" and %1 more").arg(numbers.size() - 4);
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
    if (!migrateViews(error) || !migrateAppearance(error)) { items.clear(); project = nullptr; return false; }
    invalidate(); say({}); emit changed();
    return true;
}

// Plans from before views (2026-11-19): every character, look and location gets views, and its
// pictures move into them by role (face → Face, body and look → Full body, setting → Wide, detail
// → Detail); the first approved picture of each view stays chosen. Runs once, with a copy of the
// old rows kept in plan_items_before_views, and marks the plan with a meta row.
// One description per item (Brent, 2026-11-19): a character's appearance (field "prompt") takes
// the place of its identity, a look's and location's of their description. Where the appearance
// is empty, the older text moves into it; where both were written, the older stays as notes.
bool Plan::migrateAppearance(QString& error) {
    if (items.isEmpty() || row("meta", 1)["appearance"].toInt() >= 1) return true;
    pending.clear();
    const QList<QPair<QString, QString>> moves = {{"character", "identity"}, {"look", "description"}, {"location", "description"}};
    for (const auto& [kind, older] : moves) {
        for (int id : items.value(kind).keys()) {
            QJsonObject r = row(kind, id);
            QJsonObject f = r["fields"].toObject(), prov = r["prov"].toObject();
            if (empty(f.value(older)) || !empty(f.value("prompt"))) continue;
            f["prompt"] = f.value(older);
            f.remove(older);
            if (prov.contains(older)) { prov["prompt"] = prov.value(older); prov.remove(older); }
            r["fields"] = f;
            r["prov"] = prov;
            write(kind, id, r);
        }
    }
    QJsonObject meta = row("meta", 1);
    meta["appearance"] = 1;
    if (!meta.contains("views")) meta["views"] = 1;
    write("meta", 1, meta);
    const bool ok = persist(pending, error);
    pending.clear();
    return ok;
}

bool Plan::migrateViews(QString& error) {
    if (has("meta", 1) || items.isEmpty()) return true;
    pending.clear();
    for (const QString& kind : {QStringLiteral("character"), QStringLiteral("look"), QStringLiteral("location"), QStringLiteral("prop")}) {
        for (int id : items.value(kind).keys()) {
            const ViewSpec first = firstView(kind);
            QMap<QString, int> byRole;
            for (int v : viewIds(kind, id)) byRole[row("view", v)["fields"].toObject()["role"].toString()] = v;
            QSet<int> chosen;
            for (int ref : refIds(kind, id)) {
                QJsonObject r = row("reference", ref);
                if (r.contains("view") && has("view", r["view"].toInt())) continue;
                const QString role = r["fields"].toObject()["role"].toString();
                const QString viewRole = role == "body" && kind == "look" ? "look" : role.isEmpty() ? QString(first.role) : role;
                if (!byRole.contains(viewRole)) {
                    Result made;
                    const bool isFirst = viewRole == first.role;
                    byRole[viewRole] = addView(kind, id, {{"name", isFirst ? QString(first.name) : roleViewName(viewRole)},
                                                          {"framing", isFirst ? QString(first.framing) : QString()},
                                                          {"role", viewRole}, {"from", "prompt"}}, "user", made);
                    if (!made.ok) { error = made.error; return false; }
                }
                const int view = byRole[viewRole];
                r["view"] = view;
                if (r["fields"].toObject()["approved"].toBool()) {
                    if (chosen.contains(view)) {
                        QJsonObject f = r["fields"].toObject();
                        f["approved"] = false;
                        r["fields"] = f;
                    }
                    chosen.insert(view);
                }
                write("reference", ref, r);
            }
            if (viewIds(kind, id).isEmpty()) {
                Result made;
                addView(kind, id, {{"name", first.name}, {"framing", first.framing}, {"role", first.role}, {"from", first.from}}, "user", made);
                if (!made.ok) { error = made.error; return false; }
            }
        }
    }
    write("meta", 1, {{"views", 1}});
    QSqlQuery q(project->db);
    if (!project->db.tables().contains("plan_items_before_views") &&
        !q.exec("CREATE TABLE plan_items_before_views AS SELECT * FROM plan_items")) { error = q.lastError().text(); pending.clear(); return false; }
    const bool ok = persist(pending, error);
    pending.clear();
    return ok;
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
        // A view has one chosen picture: choosing one sets the others aside as alternatives.
        if (r.ok && kind == "reference" && c.value("fields").toMap().value("approved").toBool()) {
            const int view = row("reference", id)["view"].toInt();
            for (int other : refIds(row("reference", id)["owner"].toString(), row("reference", id)["ownerId"].toInt()))
                if (other != id && row("reference", other)["view"].toInt() == view && row("reference", other)["fields"].toObject()["approved"].toBool()) {
                    Result off = setFields("reference", other, {{"approved", false}}, "user", "replace");
                    if (!off.ok) return off;
                }
            if (c.value("fields").toMap().size() == 1) pendingLabel = "Choose " + row("reference", id)["name"].toString();
        }
        // A shot plays one take: choosing it sets the others aside.
        if (r.ok && kind == "take" && c.value("fields").toMap().value("approved").toBool()) {
            const int shot = row("take", id)["shot"].toInt();
            for (int other : takeIds(shot))
                if (other != id && row("take", other)["fields"].toObject()["approved"].toBool()) {
                    Result off = setFields("take", other, {{"approved", false}}, "user", "replace");
                    if (!off.ok) return off;
                }
            if (c.value("fields").toMap().size() == 1) pendingLabel = QString("Choose take %1 for shot %2").arg(takeIds(shot).indexOf(id) + 1).arg(shotNumber(shot));
        }
        return r;
    }
    if (op == "add" && kind == "view") {
        const QString owner = c.value("owner").toString();
        const int ownerId = c.value("ownerId").toInt();
        if (!hasPictures(owner) || !has(owner, ownerId))
            return error("A view belongs to an existing character, look, location or prop.");
        QVariantMap fields = c.value("fields").toMap();
        if (fields.value("name").toString().trimmed().isEmpty()) return error("Give the view a name, such as Profile.");
        if (!fields.contains("from")) fields["from"] = "prompt";
        if (!fields.contains("role")) fields["role"] = "other";
        label("Add view " + fields.value("name").toString().trimmed());
        Result r;
        r.id = addView(owner, ownerId, fields, by, r);
        return r;
    }
    if (op == "view.move") {
        const int id = c.value("id").toInt();
        if (!has("view", id)) return error("There is no view " + QString::number(id) + ".");
        QList<int> order = viewIds(row("view", id)["owner"].toString(), row("view", id)["ownerId"].toInt());
        const int to = std::clamp(c.value("position").toInt(), 0, int(order.size()) - 1);
        if (order.indexOf(id) == to) return {};
        order.removeAll(id);
        order.insert(to, id);
        label("Reorder views");
        for (int i = 0; i < order.size(); ++i) {
            QJsonObject x = row("view", order[i]);
            if (x["position"].toInt() != i) { x["position"] = i; write("view", order[i], x); }
        }
        return {};
    }
    if (op == "add") {
        if (!hasPictures(kind)) return error("Add a character, look, location, prop or view.");
        QJsonObject fresh{{"fields", QJsonObject{}}, {"prov", QJsonObject{}}, {"edited", ++clock}};
        if (kind == "look") {
            const int owner = c.value("character").toInt();
            if (!has("character", owner)) return error("A look belongs to an existing character.");
            fresh["character"] = owner;
        }
        const int id = nextId(kind);
        write(kind, id, fresh);
        if (!has("meta", 1)) write("meta", 1, {{"views", 1}, {"appearance", 1}});  // made with views and appearance from the start
        QVariantMap fields = c.value("fields").toMap();
        if (fields.value("name").toString().trimmed().isEmpty()) {
            const QString noun = kind == "character" ? "Character" : kind == "look" ? "Look" : kind == "prop" ? "Prop" : "Location";
            fields["name"] = noun + " " + QString::number(items.value(kind).size());
        }
        label("Add " + kind);
        Result r = setFields(kind, id, fields, by, "replace");
        r.id = id;
        // Every item starts with its first view: a character's face, a look's full body, a place's wide.
        if (r.ok) {
            const ViewSpec v = firstView(kind);
            Result made;
            addView(kind, id, {{"name", v.name}, {"framing", v.framing}, {"role", v.role}, {"from", v.from}}, by, made);
            if (!made.ok) return made;
        }
        return r;
    }
    if (op == "remove") {
        const int id = c.value("id").toInt();
        if ((!hasPictures(kind) && kind != "shot" && kind != "reference" && kind != "view" && kind != "take") || !has(kind, id))
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
        if (hasPictures(kind)) dropReferences(kind, id);
        // A view takes its pictures with it.
        if (kind == "view")
            for (int ref : refIds(row("view", id)["owner"].toString(), row("view", id)["ownerId"].toInt()))
                if (row("reference", ref)["view"].toInt() == id) write("reference", ref, {});
        // A shot takes its takes with it; a removed chosen take leaves the shot without one.
        if (kind == "shot") for (int take : takeIds(id)) write("take", take, {});
        if (kind == "take") label(QString("Remove take %1 of shot %2").arg(takeIds(row("take", id)["shot"].toInt()).indexOf(id) + 1).arg(shotNumber(row("take", id)["shot"].toInt())));
        // The image (or video) stays in Media, so undo can bring it back.
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
    if (op == "take.add") return takeAdd(c, by);
    if (op == "take.upscale") return takeUpscale(c, by);
    if (op == "take.unupscale") {
        // Takes the upscaled version away (its file stays in Media/Takes, so undo brings it back).
        const int id = c.value("id").toInt();
        if (by != "user") return error("Upscaled versions are made and removed in Chorus Cut.");
        if (!has("take", id) || !row("take", id).contains("upscaled")) return error("That take has no upscaled version.");
        QJsonObject t = row("take", id);
        t.remove("upscaled");
        t["edited"] = ++clock;
        write("take", id, t);
        const int shot = t["shot"].toInt();
        label(QString("Remove the upscaled version of take %1 of shot %2").arg(takeIds(shot).indexOf(id) + 1).arg(shotNumber(shot)));
        Result done; done.id = id; return done;
    }
    if (op == "ref.move") {
        const int id = c.value("id").toInt();
        if (!has("reference", id)) return error("There is no reference " + QString::number(id) + ".");
        auto r = row("reference", id);
        // Into another view of the same item: it arrives as an alternative there.
        if (c.contains("view") && c.value("view").toInt() != r["view"].toInt()) {
            const int view = c.value("view").toInt();
            if (!has("view", view) || row("view", view)["owner"] != r["owner"] || row("view", view)["ownerId"] != r["ownerId"])
                return error("Move a picture to another view of the same item.");
            r["view"] = view;
            write("reference", id, r);
            label("Move " + r["name"].toString() + " to " + row("view", view)["fields"].toObject()["name"].toString());
            if (r["fields"].toObject()["approved"].toBool()) {
                Result off = setFields("reference", id, {{"approved", false}}, "user", "replace");
                if (!off.ok) return off;
            }
            if (!c.contains("position")) return {};
        }
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
        for (const auto& o : optionTable().value(optionKey(kind, field))) if (v.toString() == o.value) return {};
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
    if (type == "props") {
        if (!v.isArray()) return "props is a list of prop ids.";
        for (const auto& e : v.toArray()) if (!has("prop", e.toInt())) return "props names a prop that does not exist.";
        return {};
    }
    if (type == "shotref") return has("shot", v.toInt()) ? QString() : "follows names a shot that does not exist.";
    if (type == "singers") {
        if (!v.isArray()) return "singers is a list of character ids.";
        for (const auto& e : v.toArray()) if (!has("character", e.toInt())) return "singers names a character that does not exist.";
        return {};
    }
    if (type == "aspects") {
        if (!v.isArray()) return field + " is a list: face, hair, build or skin.";
        for (const auto& e : v.toArray()) {
            bool known = false;
            for (const auto& o : optionTable().value("lookChanges")) known |= e.toString() == o.value;
            if (!known) return QString("\"%1\" is not something a look can change (face, hair, build, skin).").arg(e.toString());
        }
        return {};
    }
    if (type == "refs") {
        if (!v.isArray()) return field + " is a list of reference picture ids.";
        for (const auto& e : v.toArray()) if (!has("reference", e.toInt())) return field + " names a picture that does not exist.";
        return {};
    }
    if (type == "views") {
        if (!v.isArray()) return field + " is a list of view ids.";
        for (const auto& e : v.toArray()) if (!has("view", e.toInt())) return field + " names a view that does not exist.";
        return {};
    }
    if (type == "prompts") {
        if (!v.isObject()) return "prompts maps a route to its prompt.";
        const QJsonObject prompts = v.toObject();  // one object to walk, not a temporary per call
        for (auto it = prompts.begin(); it != prompts.end(); ++it) {
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
        // Choosing a view's picture is the user's: an agent can add pictures, not choose them.
        if (by == "agent" && kind == "reference" && it.key() == "approved" && v.toBool() && !values.value("approved").toBool()) {
            r.ok = false; r.error = "Only the user chooses pictures; add it and tell the user which one you suggest."; return r;
        }
        if (by == "agent" && kind == "take" && it.key() == "approved" && v.toBool() && !values.value("approved").toBool()) {
            r.ok = false; r.error = "Only the user chooses takes; tell the user which one you suggest."; return r;
        }
        const QJsonObject p = prov[it.key()].toObject();
        const QJsonValue old = values.value(it.key());
        // The brief is the user's direction. An agent may add to it (the user's answers from a
        // conversation) but never rewrite or remove what is there, in any mode.
        if (by == "agent" && kind == "concept" && it.key() == "brief") {
            const QString before = old.toString().trimmed(), after = v.toString().trimmed();
            if (p["locked"].toBool()) { if (old != v) r.skipped.append(it.key()); continue; }
            if (!before.isEmpty() && !after.startsWith(before)) {
                r.ok = false;
                r.error = "Agents can only add to the brief, after what is already there; ask the user to change the rest.";
                return r;
            }
        } else if (by == "agent") {
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
    if (hasPictures(kind))
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

QList<int> Plan::takeIds(int shot) const {
    QList<int> ids;
    const auto& takes = items.value("take");
    for (auto it = takes.begin(); it != takes.end(); ++it)
        if (it.value()["shot"].toInt() == shot) ids.append(it.key());
    std::sort(ids.begin(), ids.end());
    return ids;
}

// The project frames a take covers: its first frame at anchor, its length in the project's rate.
QPair<int, int> Plan::takeSpan(int take) const {
    const auto t = row("take", take);
    const double rate = t["fps"].toDouble() > 0 ? t["fps"].toDouble() : 24;
    const int anchor = t["anchor"].toInt();
    return {anchor, anchor + int(std::floor(t["frames"].toInt() * fps() / rate + 1e-6))};
}

QVariantList Plan::takesFor(int shot) const {
    QVariantList out;
    const auto ids = takeIds(shot);
    for (int i = 0; i < ids.size(); ++i) {
        QVariantMap m = exported("take", ids[i]);
        m["number"] = i + 1;
        if (m.contains("refinedFrom")) m["refinedFromNumber"] = ids.indexOf(m["refinedFrom"].toInt()) + 1;
        out.append(m);
    }
    return out;
}

QVariantList Plan::timelineTakes(const QHash<int, int>& pick, bool upscaled) const {
    // Each shot's chosen take, as the part of it the cut plays: [{track, start, end, file, in}].
    QVariantList out;
    if (!project) return out;
    for (int shot : shotOrder()) {
        const auto s = row("shot", shot);
        const int picked = pick.value(shot);
        const bool usePick = picked && takeIds(shot).contains(picked);
        for (int take : takeIds(shot)) {
            const auto t = row("take", take);
            if (usePick ? take != picked : !t["fields"].toObject()["approved"].toBool()) continue;
            const auto [from, to] = takeSpan(take);
            const int a = qMax(s["start"].toInt(), from), b = qMin(s["end"].toInt(), to);
            QString file = project->dir.filePath(t["file"].toString());
            int width = t["width"].toInt(), height = t["height"].toInt();
            // The upscaled version where there is one, when asked (Render; the viewer switched to it).
            const auto up = t["upscaled"].toObject();
            bool usedUp = false;
            if (upscaled && !up.isEmpty() && QFileInfo::exists(project->dir.filePath(up["file"].toString()))) {
                file = project->dir.filePath(up["file"].toString());
                width = up["width"].toInt(); height = up["height"].toInt();
                usedUp = true;
            }
            if (a < b && QFileInfo::exists(file))
                out.append(QVariantMap{{"track", s["track"].toInt()}, {"start", a}, {"end", b}, {"file", file}, {"in", a - from}, {"take", take}, {"shot", shot},
                                       {"width", width}, {"height", height}, {"upscaled", usedUp}});
            break;
        }
    }
    return out;
}

Plan::Result Plan::takeUpscale(const QVariantMap& c, const QString& by) {
    Result r;
    auto fail = [&](const QString& e) { r.ok = false; r.error = e; return r; };
    // Upscaled versions come from the app's own jobs, never from an agent's command.
    if (by != "user") return fail("Upscaled versions are made in Chorus Cut; an agent can't add them.");
    const int id = c.value("id").toInt();
    if (!has("take", id)) return fail("The take is gone.");
    const QString source = c.value("path").toString();
    QFile file(source);
    if (!QFileInfo(source).isFile() || !file.open(QIODevice::ReadOnly)) return fail("Cannot read " + QFileInfo(source).fileName() + ".");
    QCryptographicHash hash(QCryptographicHash::Sha1);
    if (!hash.addData(&file)) return fail("Cannot read " + QFileInfo(source).fileName() + ".");
    file.close();
    const QString base = "Media/Takes/" + QString::fromLatin1(hash.result().toHex().left(20));
    const QString relative = base + "." + QFileInfo(source).suffix().toLower();
    if (!project->dir.mkpath("Media/Takes")) return fail("Cannot create Media/Takes in the project.");
    auto copyIn = [&](const QString& from, const QString& to) {
        const QString target = project->dir.filePath(to);
        if (QFileInfo::exists(target)) return true;
        QFile::remove(target + ".part");
        if (QFile::copy(from, target + ".part") && QFile::rename(target + ".part", target)) return true;
        QFile::remove(target + ".part");
        return false;
    };
    if (!copyIn(source, relative)) return fail("Could not copy the upscaled take into the project.");
    QString thumb;
    if (const QString t = c.value("thumb").toString(); QFileInfo(t).isFile()) {
        thumb = base + "." + QFileInfo(t).suffix().toLower();
        if (!copyIn(t, thumb)) thumb.clear();
    }
    QJsonObject t = row("take", id);
    t["upscaled"] = QJsonObject{{"file", relative}, {"thumb", thumb}, {"width", c.value("width").toInt()}, {"height", c.value("height").toInt()},
                                {"frames", c.value("frames").toInt()}, {"generation", QJsonObject::fromVariantMap(c.value("generation").toMap())}};
    t["edited"] = ++clock;
    write("take", id, t);
    const int shot = t["shot"].toInt();
    if (pendingLabel.isEmpty()) pendingLabel = QString("Upscale take %1 of shot %2").arg(takeIds(shot).indexOf(id) + 1).arg(shotNumber(shot));
    r.id = id;
    return r;
}

QVariantList Plan::placeTakes(const QList<int>& takes) const {
    QVariantList out;
    const auto order = shotOrder();
    for (int shot : order) {
        const auto ids = takeIds(shot);
        for (int i = 0; i < ids.size(); ++i)
            if (takes.contains(ids[i]))
                out.append(QVariantMap{{"take", ids[i]}, {"shot", shot}, {"shotNumber", order.indexOf(shot) + 1}, {"number", i + 1}});
    }
    return out;
}

Plan::Result Plan::takeAdd(const QVariantMap& c, const QString& by) {
    Result r;
    auto fail = [&](const QString& e) { r.ok = false; r.error = e; return r; };
    // Takes come from the app's own generation jobs, never from an agent's command.
    if (by != "user") return fail("Takes are generated in Chorus Cut; an agent can't add them.");
    const int shot = c.value("shot").toInt();
    if (!has("shot", shot)) return fail("The shot is gone.");
    const QString source = c.value("path").toString();
    QFile file(source);
    if (!QFileInfo(source).isFile() || !file.open(QIODevice::ReadOnly)) return fail("Cannot read " + QFileInfo(source).fileName() + ".");
    QCryptographicHash hash(QCryptographicHash::Sha1);
    if (!hash.addData(&file)) return fail("Cannot read " + QFileInfo(source).fileName() + ".");
    file.close();
    const QString base = "Media/Takes/" + QString::fromLatin1(hash.result().toHex().left(20));
    const QString relative = base + "." + QFileInfo(source).suffix().toLower();
    if (!project->dir.mkpath("Media/Takes")) return fail("Cannot create Media/Takes in the project.");
    auto copyIn = [&](const QString& from, const QString& to) {
        const QString target = project->dir.filePath(to);
        if (QFileInfo::exists(target)) return true;
        QFile::remove(target + ".part");
        if (QFile::copy(from, target + ".part") && QFile::rename(target + ".part", target)) return true;
        QFile::remove(target + ".part");
        return false;
    };
    if (!copyIn(source, relative)) return fail("Could not copy the take into the project.");
    QString thumb;
    if (const QString t = c.value("thumb").toString(); QFileInfo(t).isFile()) {
        thumb = base + "." + QFileInfo(t).suffix().toLower();
        if (!copyIn(t, thumb)) thumb.clear();
    }
    // A refined take keeps the picture of the faces found, the one refined outlined.
    QString faces;
    if (const QString f = c.value("faces").toString(); QFileInfo(f).isFile()) {
        faces = base + "-faces." + QFileInfo(f).suffix().toLower();
        if (!copyIn(f, faces)) faces.clear();
    }
    for (int existing : takeIds(shot))
        if (row("take", existing)["file"].toString() == relative) return fail("That take is already on the shot.");
    bool hasChoice = false;
    for (int other : takeIds(shot)) hasChoice |= row("take", other)["fields"].toObject()["approved"].toBool();
    const int id = nextId("take");
    QJsonObject entry{{"shot", shot}, {"file", relative}, {"thumb", thumb}, {"frames", c.value("frames").toInt()},
                      {"fps", c.value("fps", 24).toDouble()}, {"anchor", c.value("anchor").toInt()}, {"locked", c.value("locked").toBool()},
                      {"width", c.value("width").toInt()}, {"height", c.value("height").toInt()},
                      {"generation", QJsonObject::fromVariantMap(c.value("generation").toMap())}, {"faces", faces},
                      {"fields", QJsonObject{}}, {"prov", QJsonObject{}}, {"edited", ++clock}};
    write("take", id, entry);
    // The shot's first take plays at once; later ones wait to be chosen.
    if (!hasChoice) {
        Result set = setFields("take", id, {{"approved", true}}, "user", "replace");
        if (!set.ok) return set;
    }
    if (pendingLabel.isEmpty()) pendingLabel = QString("Add a take to shot %1").arg(shotNumber(shot));
    r.id = id;
    return r;
}

void Plan::dropReferences(const QString& owner, int id) {
    for (int ref : refIds(owner, id)) write("reference", ref, {});
    for (int view : viewIds(owner, id)) write("view", view, {});
}

QList<int> Plan::viewIds(const QString& owner, int id) const {
    QList<int> ids;
    const auto& views = items.value("view");
    for (auto it = views.begin(); it != views.end(); ++it)
        if (it.value()["owner"].toString() == owner && it.value()["ownerId"].toInt() == id) ids.append(it.key());
    std::sort(ids.begin(), ids.end(), [&](int a, int b) {
        const int pa = views[a]["position"].toInt(), pb = views[b]["position"].toInt();
        return pa != pb ? pa < pb : a < b;
    });
    return ids;
}

int Plan::addView(const QString& owner, int ownerId, const QVariantMap& fields, const QString& by, Result& r) {
    const QList<int> siblings = viewIds(owner, ownerId);
    const int id = nextId("view");
    write("view", id, {{"owner", owner}, {"ownerId", ownerId}, {"position", siblings.isEmpty() ? 0 : row("view", siblings.last())["position"].toInt() + 1},
                       {"fields", QJsonObject{}}, {"prov", QJsonObject{}}, {"edited", ++clock}});
    Result set = setFields("view", id, fields, by, "replace");
    if (!set.ok) { r = set; return 0; }
    r.skipped += set.skipped;
    return id;
}

Plan::Result Plan::refAdd(const QVariantMap& c, const QString& by) {
    Result r;
    auto fail = [&](const QString& e) { r.ok = false; r.error = e; return r; };
    const QString owner = c.value("owner").toString();
    const int ownerId = c.value("ownerId").toInt();
    if (!hasPictures(owner) || !has(owner, ownerId))
        return fail("A reference belongs to an existing character, look, location or prop.");
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
    // Its view: the one named, else the item's first (made if it has none yet).
    int view = c.value("view").toInt();
    if (view) {
        if (!has("view", view) || row("view", view)["owner"].toString() != owner || row("view", view)["ownerId"].toInt() != ownerId)
            return fail("That view belongs to something else.");
    } else if (const auto views = viewIds(owner, ownerId); !views.isEmpty()) {
        view = views.first();
    } else {
        const ViewSpec v = firstView(owner);
        view = addView(owner, ownerId, {{"name", v.name}, {"framing", v.framing}, {"role", v.role}, {"from", v.from}}, by, r);
        if (!r.ok) return r;
    }
    QString role = c.value("role").toString();
    if (role.isEmpty()) role = row("view", view)["fields"].toObject()["role"].toString();
    if (role.isEmpty() || role == "sheet") role = owner == "location" ? "setting" : owner == "prop" ? "object" : owner == "look" ? "look" : "face";
    bool viewHasChoice = false;
    for (int other : siblings) viewHasChoice |= row("reference", other)["view"].toInt() == view && row("reference", other)["fields"].toObject()["approved"].toBool();
    const int id = nextId("reference");
    const int position = siblings.isEmpty() ? 0 : row("reference", siblings.last())["position"].toInt() + 1;
    // Generated pictures come from the app's own jobs (never an agent's command) and keep how
    // they were made: the workflow, prompt and seed.
    const bool generated = by == "user" && c.value("source").toString() == "generated";
    // A sheet Chorus Cut composed from the user's chosen pictures is chosen at once.
    const bool composed = by == "user" && c.value("source").toString() == "composed";
    QJsonObject entry{{"owner", owner}, {"ownerId", ownerId}, {"view", view}, {"file", relative},
                      {"name", c.value("name").toString().isEmpty() ? QFileInfo(source).fileName() : c.value("name").toString()},
                      {"width", size.width()}, {"height", size.height()}, {"position", position},
                      {"source", generated ? "generated" : composed ? "composed" : by == "user" ? "imported" : "agent"}, {"fields", QJsonObject{}}, {"prov", QJsonObject{}}, {"edited", ++clock}};
    if (generated || composed) entry["generation"] = QJsonObject::fromVariantMap(c.value("generation").toMap());
    write("reference", id, entry);
    if (pendingLabel.isEmpty()) pendingLabel = generated ? "Add generated reference" : composed ? "Compose sheet" : "Add reference";
    // A picture the user brings becomes the view's chosen one if it has none; generated ones and
    // ones an agent adds wait for the user to choose them. A composed sheet replaces the choice.
    if (composed)
        for (int other : siblings)
            if (row("reference", other)["view"].toInt() == view && row("reference", other)["fields"].toObject()["approved"].toBool()) {
                Result off = setFields("reference", other, {{"approved", false}}, "user", "replace");
                if (!off.ok) return off;
            }
    const bool choose = composed || (by == "user" && !generated && !viewHasChoice);
    Result set = setFields("reference", id, {{"role", role}, {"approved", choose}}, (generated || composed) ? "user" : by, "replace");
    if (!set.ok) return set;
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
    // A moved shot carries its takes along; a lip-sync take stays on the song, so moving its shot
    // shows another part of it. A trim keeps every take where it is, like trimming a clip.
    const int delta = start - s["start"].toInt();
    if (moved && delta)
        for (int take : takeIds(id)) {
            QJsonObject t = row("take", take);
            if (t["locked"].toBool()) continue;
            t["anchor"] = t["anchor"].toInt() + delta;
            write("take", take, t);
        }
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
        // A copy has no continuity of its own yet. Each piece plays its part of the same takes.
        const int pieceId = nextId("shot");
        write("shot", pieceId, piece);
        for (int take : takeIds(id)) {
            QJsonObject t = row("take", take);
            t["shot"] = pieceId;
            write("take", nextId("take"), t);
        }
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
    if (kind == "view") return nameOf(r["owner"].toString(), r["ownerId"].toInt()) + " · " + name;
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
        m["viewName"] = row("view", r["view"].toInt())["fields"].toObject()["name"].toString();
        // Whose picture, in full: a look's says whose look it is.
        const QString owner = r["owner"].toString();
        const int ownerId = r["ownerId"].toInt();
        m["whose"] = owner == "look" ? nameOf("character", row("look", ownerId)["character"].toInt()) + " · " + nameOf("look", ownerId) : nameOf(owner, ownerId);
    }
    if (kind == "take" && project) {
        const QString path = project->dir.filePath(r["file"].toString());
        m["url"] = QUrl::fromLocalFile(path).toString();
        m["missing"] = !QFileInfo::exists(path);
        m["thumbUrl"] = r["thumb"].toString().isEmpty() ? QString() : QUrl::fromLocalFile(project->dir.filePath(r["thumb"].toString())).toString();
        m["chosen"] = r["fields"].toObject()["approved"].toBool();
        m["lipSync"] = r["locked"].toBool();
        // An upscaled version; a take refined from one is already at that size.
        const auto up = r["upscaled"].toObject();
        if (!up.isEmpty()) {
            m["upscaledUrl"] = QUrl::fromLocalFile(project->dir.filePath(up["file"].toString())).toString();
            m["upscaledWidth"] = up["width"].toInt();
            m["upscaledHeight"] = up["height"].toInt();
            m["upscaledMissing"] = !QFileInfo::exists(project->dir.filePath(up["file"].toString()));
        }
        m["hd"] = !up.isEmpty() || r["generation"].toObject()["fromUpscaled"].toBool();
        m["facesUrl"] = r["faces"].toString().isEmpty() ? QString() : QUrl::fromLocalFile(project->dir.filePath(r["faces"].toString())).toString();
        // A face refine: whose face, and the take it was made from.
        const auto g = r["generation"].toObject();
        if (g["use"] == "take-face") {
            m["refined"] = nameOf("character", g["character"].toInt());
            m["refinedFrom"] = g["source"].toInt();
        }
        const double rate = r["fps"].toDouble() > 0 ? r["fps"].toDouble() : 24;
        m["seconds"] = r["frames"].toInt() / rate;
        const auto [from, to] = takeSpan(id);
        m["from"] = from;
        m["to"] = to;
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
    const auto takes = takeIds(id);
    m["takes"] = int(takes.size());
    for (int t : takes)
        if (row("take", t)["fields"].toObject()["approved"].toBool()) {
            m["chosenTake"] = t;
            m["chosenThumb"] = row("take", t)["thumb"].toString().isEmpty() ? QString()
                               : QUrl::fromLocalFile(project->dir.filePath(row("take", t)["thumb"].toString())).toString();
        }
    return m;
}

QVariantList Plan::shots() const {
    QVariantList out;
    for (int id : shotOrder()) out.append(shot(id));
    return out;
}

QVariantList Plan::references() const {
    QVariantList out;
    for (const QString owner : {"character", "look", "location", "prop"})
        for (int id : items.value(owner).keys())
            for (int ref : refIds(owner, id)) out.append(exported("reference", ref));
    return out;
}

QVariantList Plan::referencesFor(const QString& owner, int id) const {
    QVariantList out;
    for (int ref : refIds(owner, id)) out.append(exported("reference", ref));
    return out;
}

QVariantList Plan::viewsFor(const QString& owner, int id) const {
    QVariantList out;
    const QList<int> refs = refIds(owner, id);
    for (int v : viewIds(owner, id)) {
        QVariantMap m = exported("view", v);
        const auto f = row("view", v)["fields"].toObject();
        m["name"] = f["name"].toString();
        m["framing"] = f["framing"].toString();
        m["role"] = f["role"].toString();
        m["from"] = f["from"].toString().isEmpty() ? QString("prompt") : f["from"].toString();
        QVariantList pictures;
        QVariantMap chosen;
        for (int ref : refs) {
            if (row("reference", ref)["view"].toInt() != v) continue;
            const QVariantMap r = exported("reference", ref);
            pictures.append(r);
            if (r["approved"].toBool()) chosen = r;
        }
        m["pictures"] = pictures;
        m["chosen"] = chosen;
        // Picked inputs that were removed since.
        int gone = 0;
        for (const auto& i : f["inputs"].toArray()) gone += !has("reference", i.toInt());
        m["missingInputs"] = gone;
        out.append(m);
    }
    return out;
}

namespace {
// How much a shot's framing wants a view of this role: 1 first, 3 still useful, 4+ left out.
// H3 holds nine pictures, so a close-up still sends the outfit and the body, after the face.
int framingWants(const QString& framing, const QString& role) {
    const bool close = framing == "ecu" || framing == "cu" || framing == "mcu";
    const bool wide = framing == "ws" || framing == "ews";
    if (role == "face") return close ? 1 : wide ? 3 : 1;
    if (role == "body" || role == "look") return close ? 3 : wide ? 1 : 2;
    if (role == "sheet") return close ? 2 : wide ? 2 : 3;
    if (role == "setting") return close ? 3 : wide ? 1 : 2;
    if (role == "detail") return close ? 2 : wide ? 5 : 4;
    if (role == "object") return close ? 2 : wide ? 3 : 2;  // a prop is in frame because it matters
    return 3;  // the user's own kind of view: worth sending
}
// A view the user named without a role ("Your own…", or older projects): its role from its name.
QString roleFromName(const QString& role, const QString& name) {
    if (!role.isEmpty() && role != "other") return role;
    const QString n = name.toLower();
    if (n.contains("sheet") || n.contains("turnaround")) return "sheet";
    if (n.contains("face") || n.contains("profile") || n.contains("three-quarter") || n.contains("portrait") || n.contains("head")) return "face";
    if (n.contains("full body") || n.contains("body") || n.contains("outfit")) return "body";
    if (n.contains("wide") || n.contains("establishing") || n.contains("reverse")) return "setting";
    if (n.contains("detail")) return "detail";
    if (n.contains("object") || n.contains("prop") || n.contains("product")) return "object";
    return role.isEmpty() ? QString("other") : role;
}
}

QVariantMap Plan::shotPictures(int id) const {
    constexpr int kLimit = 9;  // H3 reference-to-video takes nine pictures
    QVariantList candidates;
    if (!has("shot", id)) return {{"picked", QVariantList{}}, {"candidates", candidates}, {"overridden", false}, {"limit", kLimit}};
    const auto f = row("shot", id)["fields"].toObject();
    const QString framing = f["framing"].toString();
    struct Pick { QVariantMap ref; int score; int order; QList<int> panels; };
    QList<Pick> all;
    int order = 0;
    auto add = [&](const QString& owner, int ownerId, const QSet<QString>& leaveOut, const QString& why) {
        for (const auto& v : viewsFor(owner, ownerId)) {
            const auto view = v.toMap();
            QVariantMap ref = view["chosen"].toMap();
            if (ref.isEmpty()) continue;
            const QString role = roleFromName(view["role"].toString(), view["name"].toString());
            ref["ownerName"] = nameOf(owner, ownerId);
            ref["viewName"] = view["name"];
            ref["viewRole"] = role;
            int score = framingWants(framing, role);
            if (leaveOut.contains(role)) { score = 9; ref["note"] = why; }
            QList<int> panels;
            for (const auto& p : row("view", view["id"].toInt())["fields"].toObject()["panels"].toArray()) panels << p.toInt();
            all.append({ref, score, order++, panels});
        }
    };
    for (const auto& e : f["who"].toArray()) {
        const auto o = e.toObject();
        QSet<QString> leaveOut;
        QString why;
        if (o.contains("look") && !o["look"].isNull() && has("look", o["look"].toInt())) {
            const int look = o["look"].toInt();
            add("look", look, {}, {});
            // What the look changes makes the character's own views of it wrong for this shot.
            QStringList changed;
            for (const auto& c : row("look", look)["fields"].toObject()["changes"].toArray()) changed << c.toString();
            if (!changed.isEmpty()) {
                leaveOut = {"face", "body", "sheet", "look"};
                if (changed == QStringList{"build"}) leaveOut = {"body", "sheet", "look"};
                why = "The look \u201c" + nameOf("look", look) + "\u201d changes " + changed.join(", ") + ".";
            }
        }
        add("character", o["character"].toInt(), leaveOut, why);
    }
    if (f["where"].isDouble()) add("location", f["where"].toInt(), {}, {});
    for (const auto& p : f["props"].toArray()) if (has("prop", p.toInt())) add("prop", p.toInt(), {}, {});
    // The framing's picks: wanted views in order of how much the framing wants them; over the
    // limit, a sheet stands in for the views it lays out, then the least wanted go.
    QList<Pick> picks;
    for (const auto& p : all) if (p.score <= 3) picks.append(p);
    std::stable_sort(picks.begin(), picks.end(), [](const Pick& a, const Pick& b) { return a.score != b.score ? a.score < b.score : a.order < b.order; });
    if (picks.size() > kLimit) {
        QSet<int> covered;
        for (const auto& p : all) if (p.ref["viewRole"] == "sheet") for (int v : p.panels) covered.insert(v);
        QList<Pick> kept;
        for (const auto& p : picks) if (!covered.contains(p.ref["view"].toInt())) kept.append(p);
        bool hasSheet = false;
        for (const auto& p : kept) hasSheet |= p.ref["viewRole"] == "sheet";
        if (!covered.isEmpty() && !hasSheet)
            for (const auto& p : all) if (p.ref["viewRole"] == "sheet") kept.prepend(p);
        picks = kept;
    }
    while (picks.size() > kLimit) picks.removeLast();
    QSet<int> auto_;
    for (const auto& p : picks) auto_.insert(p.ref["id"].toInt());
    // The user's own choice for this shot, when there is one.
    const QJsonArray override_ = f["pictures"].toArray();
    QSet<int> chosen;
    for (const auto& v : override_) chosen.insert(v.toInt());
    const bool overridden = !override_.isEmpty();
    QVariantList picked;
    for (const auto& p : all) {
        QVariantMap ref = p.ref;
        const bool in = overridden ? chosen.contains(ref["id"].toInt()) : auto_.contains(ref["id"].toInt());
        ref["picked"] = in;
        candidates.append(ref);
    }
    if (overridden) {
        // Exactly what the user picked, in their order: any picture in the plan, not only the
        // chosen ones of who and where (an alternative, another character's, a prop's place).
        for (const auto& v : override_) {
            bool found = false;
            for (const auto& c : candidates)
                if (c.toMap()["id"].toInt() == v.toInt()) { picked.append(c); found = true; break; }
            if (found || !has("reference", v.toInt())) continue;
            QVariantMap ref = exported("reference", v.toInt());
            const auto r = row("reference", v.toInt());
            ref["ownerName"] = nameOf(r["owner"].toString(), r["ownerId"].toInt());
            ref["viewRole"] = roleFromName(row("view", r["view"].toInt())["fields"].toObject()["role"].toString(), ref["viewName"].toString());
            ref["picked"] = true;
            ref["extra"] = true;  // not one of who and where's chosen pictures
            picked.append(ref);
            candidates.append(ref);
        }
    } else {
        for (const auto& p : picks) { QVariantMap ref = p.ref; ref["picked"] = true; picked.append(ref); }
    }
    return {{"picked", picked}, {"candidates", candidates}, {"overridden", overridden}, {"limit", kLimit}};
}

QVariantList Plan::shotReferences(int id) const { return shotPictures(id)["picked"].toList(); }

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

bool Plan::inSection(const QJsonObject& shot, int a, int b) const {
    const int start = shot["start"].toInt(), end = shot["end"].toInt();
    const int inside = std::min(end, b) - std::max(start, a);
    return inside > 0 && (2 * inside >= end - start || 2 * inside >= int(std::lround(fps())));
}

QVariantMap Plan::links(const QString& kind, int id) const {
    const auto sectionList = sections();
    QVariantMap out{{"kind", kind}, {"id", id}, {"songFrames", songFrames()}};
    int a = 0, b = 0;
    if (kind == "section") {
        if (id < 0 || id >= sectionList.size()) return {};
        const auto s = sectionList[id].toMap();
        out["name"] = s["label"]; out["start"] = s["start"]; out["end"] = s["end"];
        a = msToFrame(s["start"].toInt()); b = msToFrame(s["end"].toInt());
    } else if (kind == "character" || kind == "location" || kind == "prop") {
        if (!has(kind, id)) return {};
        out["name"] = nameOf(kind, id);
    } else {
        return {};
    }
    const auto appears = [&](const QJsonObject& f) {
        if (kind == "location") return f["where"].isDouble() && f["where"].toInt() == id;
        if (kind == "prop") { for (const auto& p : f["props"].toArray()) if (p.toInt() == id) return true; return false; }
        for (const auto& e : f["who"].toArray()) if (e.toObject()["character"].toInt() == id) return true;
        return false;
    };
    // Counts keyed by id, kept in first-seen order.
    struct Tally { QList<int> order; QMap<int, int> count; QMap<int, QStringList> extra;
                   void add(int k) { if (!count.contains(k)) order << k; ++count[k]; } };
    Tally cast, places, looks, things;
    QVariantList shots, marks;
    QList<int> sectionCount(sectionList.size(), 0);
    int empty = 0;
    for (int shotId : shotOrder()) {
        const auto r = row("shot", shotId);
        const auto f = r["fields"].toObject();
        if (kind == "section" ? !inSection(r, a, b) : !appears(f)) continue;
        QStringList who;
        for (const auto& e : f["who"].toArray()) {
            const auto o = e.toObject();
            const int c = o["character"].toInt();
            if (!has("character", c)) continue;
            who << nameOf("character", c);
            const bool worn = o.contains("look") && !o["look"].isNull() && has("look", o["look"].toInt());
            if (kind == "section") {
                cast.add(c);
                if (worn && !cast.extra[c].contains(nameOf("look", o["look"].toInt()))) cast.extra[c] << nameOf("look", o["look"].toInt());
            } else if (kind == "character" && c == id) {
                looks.add(worn ? o["look"].toInt() : 0);
            } else if (kind == "location" || kind == "prop") {
                cast.add(c);
            }
        }
        QStringList propNames;
        for (const auto& p : f["props"].toArray()) {
            if (!has("prop", p.toInt())) continue;
            propNames << nameOf("prop", p.toInt());
            if (kind != "prop") things.add(p.toInt());
        }
        if (kind == "section" && who.isEmpty()) ++empty;
        const bool placed = f["where"].isDouble() && has("location", f["where"].toInt());
        if (placed && kind != "location") places.add(f["where"].toInt());
        if (kind != "section")
            for (int i = 0; i < sectionList.size(); ++i) {
                const auto s = sectionList[i].toMap();
                if (inSection(r, msToFrame(s["start"].toInt()), msToFrame(s["end"].toInt()))) ++sectionCount[i];
            }
        const int start = r["start"].toInt(), end = r["end"].toInt();
        shots.append(QVariantMap{{"id", shotId}, {"number", shotNumber(shotId)}, {"track", r["track"].toInt()},
                                 {"start", start}, {"end", end}, {"startMs", frameToMs(start)}, {"endMs", frameToMs(end)},
                                 {"type", f["type"].toString()}, {"framing", f["framing"].toString().toUpper()},
                                 {"who", who.join(", ")}, {"where", placed ? nameOf("location", f["where"].toInt()) : QString()},
                                 {"props", propNames.join(", ")},
                                 {"purpose", f["purpose"].toString()}});
        marks.append(QVariantMap{{"start", start}, {"end", end}});
    }
    auto list = [](const Tally& t, const std::function<QVariantMap(int)>& entry) {
        QVariantList l;
        for (int k : t.order) { QVariantMap m = entry(k); m["shots"] = t.count[k]; l.append(m); }
        return l;
    };
    auto named = [this](const QString& k) { return [this, k](int i) { return QVariantMap{{"id", i}, {"name", nameOf(k, i)}}; }; };
    out["shots"] = shots;
    out["marks"] = marks;
    if (kind != "location") out["locations"] = list(places, named("location"));
    if (kind != "prop") out["props"] = list(things, named("prop"));
    if (kind == "section") {
        out["characters"] = list(cast, [&](int c) { return QVariantMap{{"id", c}, {"name", nameOf("character", c)}, {"looks", cast.extra.value(c)}}; });
        out["empty"] = empty;
        const int sa = out["start"].toInt(), sb = out["end"].toInt();
        QVariantList lines;
        for (const auto& v : linesIn(msToFrame(sa), msToFrame(sb))) {
            const auto l = v.toMap();
            const int inside = std::min(l["end"].toInt(), sb) - std::max(l["start"].toInt(), sa);
            if (2 * inside >= l["end"].toInt() - l["start"].toInt()) lines.append(l);
        }
        out["lines"] = lines;
    } else {
        QVariantList secs;
        for (int i = 0; i < sectionList.size(); ++i)
            if (sectionCount[i]) {
                const auto s = sectionList[i].toMap();
                secs.append(QVariantMap{{"ordinal", i}, {"name", s["label"]}, {"start", s["start"]}, {"end", s["end"]}, {"shots", sectionCount[i]}});
            }
        out["sections"] = secs;
    }
    if (kind == "character") {
        out["looks"] = list(looks, [this](int l) { return QVariantMap{{"id", l}, {"name", l ? nameOf("look", l) : QString("No look set")}}; });
        int sung = 0;
        for (const auto& v : singers()) if (v.toMap()["singers"].toList().contains(id)) ++sung;
        out["sung"] = sung;
    }
    if (kind == "location" || kind == "prop") out["characters"] = list(cast, named("character"));
    return out;
}

QVariantMap Plan::overview() const {
    const auto sectionList = sections();
    QVector<QPair<int, int>> spans;
    for (const auto& v : sectionList) {
        const auto s = v.toMap();
        spans.append({msToFrame(s["start"].toInt()), msToFrame(s["end"].toInt())});
    }
    QVariantList rows;
    for (const QString kind : {"character", "location", "prop"})
        for (int id : items.value(kind).keys()) {
            QVariantList cells;
            int total = 0;
            for (const auto& span : spans) {
                int n = 0;
                for (const auto& r : items.value("shot")) {
                    const auto f = r["fields"].toObject();
                    bool in = false;
                    if (kind == "location") in = f["where"].isDouble() && f["where"].toInt() == id;
                    else if (kind == "prop") { for (const auto& p : f["props"].toArray()) in = in || p.toInt() == id; }
                    else for (const auto& e : f["who"].toArray()) in = in || e.toObject()["character"].toInt() == id;
                    if (in && inSection(r, span.first, span.second)) ++n;
                }
                cells.append(n);
                total += n;
            }
            rows.append(QVariantMap{{"kind", kind}, {"id", id}, {"name", nameOf(kind, id)}, {"cells", cells}, {"total", total}});
        }
    return {{"sections", sectionList}, {"songFrames", songFrames()}, {"rows", rows}};
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
                for (const auto& o : optionTable().value(optionKey(k.key(), f.key())))
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
    // owner: the character, look or location the check is about, so the UI can open it.
    auto add = [&](const QString& level, const QString& gate, const QString& code, const QString& text,
                   int startMs, int endMs, const QList<int>& shots = {}, const QString& fix = {}, const QVariantMap& owner = {}) {
        QVariantList ids;
        for (int s : shots) ids.append(s);
        QVariantMap check{{"level", level}, {"gate", gate}, {"code", code}, {"message", text},
                          {"start", startMs}, {"end", endMs}, {"shots", ids}, {"fix", fix}};
        if (!owner.isEmpty()) check["owner"] = owner;
        out.append(check);
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
            for (const auto& p : f["props"].toArray()) if (has("prop", p.toInt())) uses[{"prop", p.toInt()}] << id;
        }
        for (auto it = uses.begin(); it != uses.end(); ++it) {
            const auto& [kind, id] = it.key();
            bool approved = false;
            for (int ref : refIds(kind, id)) approved |= row("reference", ref)["fields"].toObject()["approved"].toBool();
            if (approved) continue;
            QList<int> shots = it.value();
            shots.erase(std::unique(shots.begin(), shots.end()), shots.end());
            // The fix is on the card, not the shots, so the message counts them; the ids stay on the check.
            const QString what = kind == "look" ? QString("The look \u201c%1\u201d").arg(nameOf(kind, id)) : nameOf(kind, id);
            const QString used = shots.size() == 1 ? QString("shot %1").arg(number(shots.first())) : QString("%1 shots").arg(shots.size());
            add("todo", "G3", "reference", QString("%1 has no approved reference picture yet (used in %2).").arg(what, used),
                shotMs(shots.first()).first, shotMs(shots.last()).second, shots, {}, QVariantMap{{"kind", kind}, {"id", id}});
        }
        for (int ref : items.value("reference").keys()) {
            const auto r = row("reference", ref);
            if (!QFileInfo::exists(project->dir.filePath(r["file"].toString())))
                add("error", "G3", "referenceMissing", QString("The reference picture %1 for %2 is missing from the project's Media/References folder.")
                    .arg(r["name"].toString(), nameOf(r["owner"].toString(), r["ownerId"].toInt())), 0, 0, {}, {},
                    QVariantMap{{"kind", r["owner"].toString()}, {"id", r["ownerId"].toInt()}});
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
        for (const auto& p : f["props"].toArray())
            if (!has("prop", p.toInt())) { add("error", "G4", "missingRef", QString("Shot %1 shows a prop that was removed.").arg(n), a, b, {id}); break; }
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
                .arg(line["text"].toString(), joinNames(names), shotNumbers(shotNames)), a, b, over);
        }
    }
    // A chosen take that doesn't reach the shot's edges (the shot was trimmed or moved past it).
    for (int id : order) {
        for (int take : takeIds(id)) {
            if (!row("take", take)["fields"].toObject()["approved"].toBool()) continue;
            const auto [from, to] = takeSpan(take);
            const int a = shotRows[id]["start"].toInt(), b = shotRows[id]["end"].toInt();
            const int missing = qMax(0, from - a) + qMax(0, b - to);
            if (missing > 0)
                add("warning", "G4", "takeShort", QString("Shot %1's take is %2 s short %3: trim the shot or generate it again.")
                    .arg(number(id)).arg(QString::number(missing / fps(), 'f', 1))
                    .arg(from > a && to < b ? "at both ends" : from > a ? "at its start" : "at its end"),
                    frameToMs(a), frameToMs(b), {id});
            break;
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
        for (const auto& p : f["props"].toArray()) upstream = std::max(upstream, row("prop", p.toInt())["edited"].toInt());
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
        add("warning", "G4", "stale", QString("The concept, cast, locations, props or arc changed after %1 %2 %3 written. Review %4, then mark %4 current.")
            .arg(stale.size() == 1 ? "shot" : "shots", shotNumbers(names), stale.size() == 1 ? "was" : "were", stale.size() == 1 ? "it" : "them"),
            shotMs(stale.first()).first, shotMs(stale.last()).second, stale, "markCurrent");
    }
    return out;
}
