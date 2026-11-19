// Pre-production planning: the command layer first (provenance, locks, agent modes, batches,
// undo, checks, splitting, persistence), then the Plan workspace, shot cards and the checks
// panel with real mouse and keyboard input at on-screen positions.
// usage: ChorusCutPlanCheck PROJECT_COPY ALIGNMENT_JSON
#include "editor.h"
#include "waveform.h"
#include "library.h"
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickItem>
#include <QQuickWindow>
#include <QStyleHints>
#include <QTest>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QDir>
#include <QDropEvent>
#include <QImage>
#include <QMimeData>
#include <QUrl>
#include <QTemporaryDir>
#include <QFile>
#include <QJsonDocument>
#include <QWheelEvent>
#include <QDebug>
#include <cmath>
#include <functional>

static int fail(int code, const QString& why) { qCritical().noquote() << "FAIL" << code << why; return code; }
static bool waitFor(const std::function<bool()>& ready, int timeout = 10000) {
    QElapsedTimer t; t.start();
    while (t.elapsed() < timeout) { if (ready()) return true; QTest::qWait(20); }
    return ready();
}
static QQuickItem* findItem(QQuickItem* root, const QString& name) {
    if (root->objectName() == name && root->isVisible()) return root;
    for (auto child : root->childItems())
        if (auto found = findItem(child, name)) return found;
    return nullptr;
}
static QQuickItem* find(QQuickWindow* window, const QString& name) {
    if (auto item = findItem(window->contentItem(), name)) return item;
    for (auto item : window->findChildren<QQuickItem*>(name))
        if (item->isVisible()) return item;
    return nullptr;
}
static QVariantMap cmd(std::initializer_list<std::pair<QString, QVariant>> values) {
    QVariantMap m;
    for (const auto& [k, v] : values) m[k] = v;
    return m;
}
static QVariantList checksWith(Plan* plan, const QString& code) {
    QVariantList out;
    for (const auto& c : plan->checks()) if (c.toMap()["code"] == code) out.append(c);
    return out;
}

int main(int argc, char** argv) {
    if (argc != 3) { qCritical() << "usage: ChorusCutPlanCheck PROJECT_COPY ALIGNMENT_JSON"; return 1; }
    qputenv("QT_QPA_PLATFORM", "offscreen");
    qputenv("QT_QUICK_CONTROLS_STYLE", "Basic");
    qputenv("SDL_AUDIODRIVER", "disk"); qputenv("SDL_AUDIO_DRIVER", "disk");
    QTemporaryDir audioDir;
    qputenv("SDL_AUDIO_DISK_OUTPUT_FILE", QFile::encodeName(audioDir.filePath("capture.raw")));
    QGuiApplication app(argc, argv);
    app.setOrganizationName("Chorus Cut Tests");
    app.setApplicationName("Plan Check");
    Mlt::Factory::init();
    const QUrl project = QUrl::fromLocalFile(QFileInfo(argv[1]).absoluteFilePath());
    {
        // A concept row as older builds left it after locking and unlocking an empty logline.
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "seed");
        db.setDatabaseName(QDir(project.toLocalFile()).filePath("project.sqlite"));
        QSqlQuery q(db.open() ? db : QSqlDatabase());
        if (!q.exec("CREATE TABLE IF NOT EXISTS plan_items(kind TEXT NOT NULL, id INTEGER NOT NULL, data TEXT NOT NULL, PRIMARY KEY(kind,id))") ||
            !q.exec("INSERT OR REPLACE INTO plan_items VALUES('concept',1,'{\"fields\":{},\"prov\":{\"logline\":{\"by\":\"user\",\"locked\":false}}}')"))
            return fail(2, "could not seed the plan");
        q.finish(); db.close();
    }
    QSqlDatabase::removeDatabase("seed");
    Editor editor;
    if (!editor.openProject(project)) return fail(2, editor.status());
    if (!editor.plan()->concept()["prov"].toMap().isEmpty()) return fail(2, "provenance of an empty, unlocked field survived opening the project");
    if (!editor.lyrics()->importAlignment(QUrl::fromLocalFile(QFileInfo(argv[2]).absoluteFilePath())))
        return fail(2, editor.lyrics()->message());
    Plan* plan = editor.plan();
    auto run = [&](const QVariantMap& c) { return plan->run(c); };
    auto ok = [&](const QVariantMap& r) { return r.value("ok").toBool(); };
    const double fps = editor.fps();
    auto frame = [&](int ms) { return int(std::lround(ms * fps / 1000.0)); };
    const auto lines = editor.lyrics()->lines();
    auto line = [&](int row) { return lines[row].toMap(); };

    // ---- A. Commands, provenance and undo
    if (!ok(run(cmd({{"op", "set"}, {"kind", "concept"}, {"fields", QVariantMap{{"logline", "A radio DJ falls for a voice in the static."}}}}))))
        return fail(10, plan->message());
    if (plan->concept()["prov"].toMap()["logline"].toMap()["by"] != "user") return fail(10, "user write not marked as the user's");
    editor.undo();
    if (plan->concept()["fields"].toMap().contains("logline")) return fail(11, "undo kept the logline");
    editor.redo();
    if (plan->concept()["fields"].toMap()["logline"].toString().isEmpty()) return fail(11, "redo lost the logline");

    // Agent passes: fill empty or agent-owned fields, never user-owned ones unless replacing, never locked ones.
    auto r = run(cmd({{"op", "set"}, {"kind", "concept"}, {"by", "agent"},
                      {"fields", QVariantMap{{"logline", "Agent logline"}, {"style", "Grainy 16 mm night radio"}}}}));
    if (!ok(r) || r["skipped"].toStringList() != QStringList{"logline"}) return fail(12, "agent should skip the user's logline");
    const auto concept = plan->concept();
    if (concept["fields"].toMap()["style"] != "Grainy 16 mm night radio" || concept["prov"].toMap()["style"].toMap()["by"] != "agent")
        return fail(12, "agent style not written as the agent's");
    if (!ok(run(cmd({{"op", "lock"}, {"kind", "concept"}, {"field", "style"}, {"locked", true}})))) return fail(13, plan->message());
    r = run(cmd({{"op", "set"}, {"kind", "concept"}, {"by", "agent"}, {"mode", "replace"},
                 {"fields", QVariantMap{{"style", "Neon"}, {"logline", "Replaced logline"}}}}));
    if (!ok(r) || r["skipped"].toStringList() != QStringList{"style"} || plan->concept()["fields"].toMap()["logline"] != "Replaced logline")
        return fail(13, "replace mode should write the unlocked user field and skip the locked one");
    if (ok(run(cmd({{"op", "lock"}, {"kind", "concept"}, {"field", "style"}, {"locked", false}, {"by", "agent"}}))))
        return fail(13, "an agent unlocked a field");
    // The brief: agents may add the user's answers to the end, never rewrite or remove it.
    run(cmd({{"op", "set"}, {"kind", "concept"}, {"fields", QVariantMap{{"brief", "Moody, nocturnal, one singer."}}}}));
    r = run(cmd({{"op", "set"}, {"kind", "concept"}, {"by", "agent"}, {"mode", "fill"},
                 {"fields", QVariantMap{{"brief", "Moody, nocturnal, one singer.\n- Format: 16:9"}}}}));
    if (!ok(r) || !plan->concept()["fields"].toMap()["brief"].toString().endsWith("16:9")) return fail(13, "an agent could not add to the brief");
    if (ok(run(cmd({{"op", "set"}, {"kind", "concept"}, {"by", "agent"}, {"mode", "replace"}, {"fields", QVariantMap{{"brief", "Bright daytime pop"}}}}))) ||
        ok(run(cmd({{"op", "set"}, {"kind", "concept"}, {"by", "agent"}, {"fields", QVariantMap{{"brief", ""}}}}))) ||
        !plan->concept()["fields"].toMap()["brief"].toString().startsWith("Moody"))
        return fail(13, "an agent rewrote or cleared the brief");
    run(cmd({{"op", "lock"}, {"kind", "concept"}, {"field", "brief"}, {"locked", true}}));
    r = run(cmd({{"op", "set"}, {"kind", "concept"}, {"by", "agent"}, {"fields", QVariantMap{{"brief", plan->concept()["fields"].toMap()["brief"].toString() + "\n- more"}}}}));
    if (r["skipped"].toStringList() != QStringList{"brief"}) return fail(13, "an agent added to a locked brief");
    run(cmd({{"op", "lock"}, {"kind", "concept"}, {"field", "brief"}, {"locked", false}}));
    run(cmd({{"op", "set"}, {"kind", "concept"}, {"fields", QVariantMap{{"brief", "The user's own brief"}}}}));
    if (plan->concept()["fields"].toMap()["brief"] != "The user's own brief") return fail(13, "the user could not rewrite their brief");

    // Locking an empty field and unlocking it again leaves no provenance behind.
    run(cmd({{"op", "lock"}, {"kind", "concept"}, {"field", "treatment"}, {"locked", true}}));
    if (!plan->concept()["prov"].toMap()["treatment"].toMap()["locked"].toBool()) return fail(13, "an empty field could not be locked");
    run(cmd({{"op", "lock"}, {"kind", "concept"}, {"field", "treatment"}, {"locked", false}}));
    if (plan->concept()["prov"].toMap().contains("treatment")) return fail(13, "unlocking an empty field left provenance behind");
    run(cmd({{"op", "lock"}, {"kind", "arc"}, {"id", 3}, {"field", "beat"}, {"locked", true}}));
    run(cmd({{"op", "lock"}, {"kind", "arc"}, {"id", 3}, {"field", "beat"}, {"locked", false}}));
    if (!plan->arc()[3].toMap()["prov"].toMap().isEmpty()) return fail(13, "unlocking an empty arc field left provenance behind");
    if (ok(run(cmd({{"op", "set"}, {"kind", "concept"}, {"fields", QVariantMap{{"aspect", "7:3"}}}}))))
        return fail(14, "an unknown aspect ratio was accepted");

    // Cast and locations.
    const int mara = run(cmd({{"op", "add"}, {"kind", "character"}, {"fields", QVariantMap{{"name", "Mara"}, {"role", "singer"}}}}))["id"].toInt();
    const int leo = run(cmd({{"op", "add"}, {"kind", "character"}, {"fields", QVariantMap{{"name", "Leo"}, {"role", "actor"}}}}))["id"].toInt();
    const int coat = run(cmd({{"op", "add"}, {"kind", "look"}, {"character", mara}, {"fields", QVariantMap{{"name", "Red coat"}}}}))["id"].toInt();
    const int booth = run(cmd({{"op", "add"}, {"kind", "location"}, {"fields", QVariantMap{{"name", "Radio booth"}, {"timeOfDay", "night"}}}}))["id"].toInt();
    if (!mara || !leo || !coat || !booth || plan->characters().size() != 2) return fail(15, "cast or location not added: " + plan->message());

    // A batch is all or nothing, and one undo step.
    const int steps = editor.property("canUndo").toBool() ? 1 : 0;
    Q_UNUSED(steps);
    const QString undoBefore = editor.undoText();
    r = run(cmd({{"op", "batch"}, {"label", "Agent: draft locations"}, {"by", "agent"}, {"commands", QVariantList{
        cmd({{"op", "add"}, {"kind", "location"}, {"fields", QVariantMap{{"name", "Rooftop"}}}}),
        cmd({{"op", "set"}, {"kind", "location"}, {"id", 999}, {"fields", QVariantMap{{"name", "Nowhere"}}}})}}}));
    if (ok(r) || plan->locations().size() != 1 || editor.undoText() != undoBefore) return fail(16, "a failed batch left changes behind");
    r = run(cmd({{"op", "batch"}, {"label", "Agent: draft locations"}, {"by", "agent"}, {"commands", QVariantList{
        cmd({{"op", "add"}, {"kind", "location"}, {"fields", QVariantMap{{"name", "Rooftop"}}}}),
        cmd({{"op", "add"}, {"kind", "location"}, {"fields", QVariantMap{{"name", "Parking lot"}}}})}}}));
    if (!ok(r) || plan->locations().size() != 3 || editor.undoText() != "Agent: draft locations") return fail(16, "batch not one step");
    editor.undo();
    if (plan->locations().size() != 1) return fail(16, "undoing the batch did not remove both locations");

    // The arc follows the song's section order.
    const auto sections = plan->sections();
    if (sections.size() < 4) return fail(17, "too few song sections");
    const QString first = sections[0].toMap()["label"].toString(), second = sections[1].toMap()["label"].toString();
    if (ok(run(cmd({{"op", "set"}, {"kind", "arc"}, {"id", 0}, {"section", second}, {"fields", QVariantMap{{"happens", "x"}}}}))))
        return fail(17, "an arc entry was accepted out of section order");
    if (!ok(run(cmd({{"op", "set"}, {"kind", "arc"}, {"id", 0}, {"section", first}, {"fields", QVariantMap{{"happens", "Static, then a voice."}}}}))))
        return fail(17, plan->message());

    // Singers for the first lines, which fall in the first verse.
    if (!ok(run(cmd({{"op", "singers"}, {"rows", QVariantList{0, 1, 2, 3}}, {"singers", QVariantList{mara}}}))))
        return fail(18, plan->message());
    if (plan->singers()[2].toMap()["singers"].toList() != QVariantList{mara}) return fail(18, "singer not stored");

    // ---- B. Shots: placement, overlap, length, split at lines, checks
    int lastRow = 1;
    while (lastRow + 1 < lines.size() && line(lastRow)["end"].toInt() - line(0)["start"].toInt() < 17000) ++lastRow;
    const int s0 = frame(line(0)["start"].toInt()), e3 = frame(line(lastRow)["end"].toInt());
    const QVariantMap full{{"type", "lipsync"}, {"purpose", "Mara finds the frequency"}, {"who", QVariantList{QVariantMap{{"character", mara}, {"look", coat}}}},
                           {"where", booth}, {"framing", "mcu"}, {"move", "push"}, {"route", "h3-lipsync"}};
    const int a = run(cmd({{"op", "shot.add"}, {"track", 1}, {"start", s0}, {"end", e3}, {"fields", full}}))["id"].toInt();
    if (!a) return fail(20, plan->message());
    if (ok(run(cmd({{"op", "shot.add"}, {"track", 1}, {"start", s0 + 5}, {"end", s0 + 30}}))))
        return fail(20, "an overlapping shot was accepted on the same track");
    if ((e3 - s0) / fps <= 15.1) return fail(21, QString("fixture lines span only %1 s").arg((e3 - s0) / fps));
    if (checksWith(plan, "tooLong").size() != 1 || checksWith(plan, "tooLong")[0].toMap()["fix"] != "splitLines")
        return fail(21, "a shot longer than H3's limit was not flagged");
    if (!checksWith(plan, "fields").isEmpty()) return fail(21, "a fully specified shot was reported incomplete");
    if (!ok(run(cmd({{"op", "shot.splitLines"}, {"id", a}})))) return fail(22, plan->message());
    const auto pieces = plan->shots();
    if (pieces.size() < 2) return fail(22, "split produced one shot");
    QSet<int> boundaries;
    for (int i = 1; i < lines.size(); ++i) {
        const int x = line(i - 1)["end"].toInt(), y = line(i)["start"].toInt();
        boundaries.insert(frame(y > x ? (x + y) / 2 : y));
    }
    for (int i = 0; i < pieces.size(); ++i) {
        const auto p = pieces[i].toMap();
        if (p["end"].toInt() - p["start"].toInt() > plan->maxFrames("h3-lipsync")) return fail(22, "a split piece is still too long");
        if (i && (!boundaries.contains(p["start"].toInt()) || p["start"].toInt() != pieces[i - 1].toMap()["end"].toInt()))
            return fail(22, QString("piece %1 does not start between lyric lines").arg(i + 1));
        if (p["fields"].toMap()["purpose"] != full["purpose"]) return fail(22, "a split piece lost its fields");
    }
    if (!checksWith(plan, "tooLong").isEmpty()) return fail(22, "still too long after the split");
    if (checksWith(plan, "gap").isEmpty()) return fail(23, "the uncovered song was not reported as gaps");

    // A lip-sync shot of Leo over Mara's line, and one over the instrumental intro.
    const int s1 = frame(line(1)["start"].toInt()), e1 = frame(line(1)["end"].toInt());
    const int b = run(cmd({{"op", "shot.add"}, {"track", 2}, {"start", s1}, {"end", e1},
                           {"fields", QVariantMap{{"type", "lipsync"}, {"who", QVariantList{QVariantMap{{"character", leo}}}}}}}))["id"].toInt();
    const int c = run(cmd({{"op", "shot.add"}, {"track", 2}, {"start", 0}, {"end", frame(4000)}, {"fields", QVariantMap{{"type", "lipsync"}}}}))["id"].toInt();
    if (!b || !c) return fail(24, plan->message());
    // Line 1 is also under the Mara pieces on V1, so it passes; take V1 out to see the error.
    QVariantList v1;
    for (const auto& p : plan->shots()) if (p.toMap()["track"].toInt() == 1) v1.append(p.toMap()["id"]);
    for (const auto& id : v1) run(cmd({{"op", "shot.retime"}, {"id", id}, {"track", 1}}));  // no-op retimes leave no step
    if (!checksWith(plan, "singer").isEmpty()) return fail(24, "Mara is in frame on V1 for line 1; no singer error expected");
    run(cmd({{"op", "set"}, {"kind", "shot"}, {"id", v1[0]}, {"fields", QVariantMap{{"type", "story"}}}}));
    run(cmd({{"op", "set"}, {"kind", "shot"}, {"id", v1.value(1)}, {"fields", QVariantMap{{"type", "story"}}}}));
    if (checksWith(plan, "singer").isEmpty()) return fail(24, "Leo lip-syncing Mara's line was not reported");
    if (checksWith(plan, "lipsyncSilent").isEmpty()) return fail(25, "lip-sync over the intro was not reported");
    if (checksWith(plan, "fields").isEmpty()) return fail(25, "incomplete shots were not reported");

    // Stale after an upstream change, until marked current.
    if (!checksWith(plan, "stale").isEmpty()) return fail(26, "nothing should be stale yet");
    run(cmd({{"op", "set"}, {"kind", "character"}, {"id", mara}, {"fields", QVariantMap{{"identity", "Late twenties, cropped black hair"}}}}));
    const auto stale = checksWith(plan, "stale");
    if (stale.size() != 1 || stale[0].toMap()["shots"].toList().isEmpty()) return fail(26, "shots of Mara not marked stale");
    if (!ok(run(cmd({{"op", "shot.markCurrent"}, {"ids", stale[0].toMap()["shots"]}}))) || !checksWith(plan, "stale").isEmpty())
        return fail(26, "mark current did not clear the stale shots");
    // Long shot lists are cut to four numbers and "and N more".
    {
        QVariantList many;
        for (int k = 0; k < 7; ++k)
            many.append(cmd({{"op", "shot.add"}, {"track", 2}, {"start", 4000 + k * 40}, {"end", 4040 + k * 40},
                             {"fields", QVariantMap{{"type", "broll"}, {"purpose", "Filler"}}}}));
        run(cmd({{"op", "batch"}, {"label", "Fillers"}, {"commands", many}}));
        run(cmd({{"op", "set"}, {"kind", "concept"}, {"fields", QVariantMap{{"palette", "Cherry and teal"}}}}));
        const auto longStale = checksWith(plan, "stale").value(0).toMap();
        const int count = longStale["shots"].toList().size();
        const QString text = longStale["message"].toString();
        if (count < 7 || !text.contains(QString(" and %1 more").arg(count - 4)) || text.count(QRegularExpression("\\d+")) != 5)
            return fail(26, QString("a long shot list was not shortened (%1 shots): %2").arg(count).arg(text));
        editor.undo(); editor.undo();
        if (!checksWith(plan, "stale").isEmpty()) return fail(26, "undoing the fillers and the concept edit left stale shots");
    }

    // Removing a character leaves a reported dangling reference, and undo brings it back.
    run(cmd({{"op", "remove"}, {"kind", "character"}, {"id", leo}}));
    if (checksWith(plan, "missingRef").isEmpty()) return fail(27, "removed character not reported");
    editor.undo();
    if (!checksWith(plan, "missingRef").isEmpty() || plan->characters().size() != 2) return fail(27, "undo did not restore Leo");

    // ---- B2. Reference pictures you already have
    QTemporaryDir pictures;
    auto picture = [&](const QString& name, QColor color, QSize size) {
        QImage image(size, QImage::Format_RGB32);
        image.fill(color);
        const QString path = pictures.filePath(name);
        return image.save(path) ? path : QString();
    };
    const QString face = picture("mara-face.png", QColor("#c08060"), {400, 500});
    const QString body = picture("mara-body.jpg", QColor("#304060"), {300, 600});
    const QString coatPic = picture("red-coat.png", QColor("#b02020"), {256, 256});
    const QString boothPic = picture("booth.png", QColor("#405030"), {640, 360});
    const QString stray = picture("stray.png", QColor("#777777"), {64, 64});
    {
        QFile text(pictures.filePath("notes.png"));
        text.open(QIODevice::WriteOnly); text.write("not a picture"); text.close();
    }
    // Shots from B show Mara in her red coat at the booth, and Leo; nobody has a picture yet.
    auto needing = [&] {
        QStringList names;
        for (const auto& c : checksWith(plan, "reference")) names << c.toMap()["message"].toString().section(" has no", 0, 0);
        names.sort();
        return names;
    };
    if (needing() != QStringList{"Leo", "Mara", "Radio booth", "The look \u201cRed coat\u201d"})
        return fail(31, "unexpected G3 to-dos: " + needing().join(" | "));
    r = run(cmd({{"op", "ref.add"}, {"owner", "character"}, {"ownerId", mara}, {"path", QUrl::fromLocalFile(face).toString()}}));
    const int faceRef = r["id"].toInt();
    const auto faceRow = plan->referencesFor("character", mara).value(0).toMap();
    if (!ok(r) || faceRow["role"] != "face" || !faceRow["approved"].toBool() || faceRow["width"].toInt() != 400 ||
        !QFileInfo(QDir(QFileInfo(argv[1]).absoluteFilePath()).filePath(faceRow["file"].toString())).isFile() || !faceRow["file"].toString().startsWith("Media/References/"))
        return fail(31, "an imported picture was not copied in, approved and given the face role: " + plan->message());
    if (ok(run(cmd({{"op", "ref.add"}, {"owner", "character"}, {"ownerId", mara}, {"path", face}}))))
        return fail(31, "the same picture was added twice");
    if (ok(run(cmd({{"op", "ref.add"}, {"owner", "character"}, {"ownerId", mara}, {"path", pictures.filePath("notes.png")}}))))
        return fail(31, "a file that is not a picture was accepted");
    const int bodyRef = run(cmd({{"op", "ref.add"}, {"owner", "character"}, {"ownerId", mara}, {"path", body}}))["id"].toInt();
    if (plan->referencesFor("character", mara).value(1).toMap()["role"] != "body") return fail(31, "a second character picture should default to full body");
    run(cmd({{"op", "ref.add"}, {"owner", "look"}, {"ownerId", coat}, {"path", coatPic}}));
    // An agent's picture waits for approval, so the booth still has none approved.
    r = run(cmd({{"op", "ref.add"}, {"owner", "location"}, {"ownerId", booth}, {"path", boothPic}, {"by", "agent"}}));
    const int boothRef = r["id"].toInt();
    if (plan->referencesFor("location", booth).value(0).toMap()["approved"].toBool()) return fail(32, "an agent's picture was approved");
    if (needing() != QStringList{"Leo", "Radio booth"}) return fail(32, "only Leo and the booth should still need a picture: " + needing().join(" | "));
    // The to-do counts the shots instead of listing them, and names what it is about.
    for (const auto& c : checksWith(plan, "reference")) {
        const auto m = c.toMap();
        const auto owner = m["owner"].toMap();
        if (!m["message"].toString().contains("(used in ") || m["message"].toString().contains(", ") ||
            owner["kind"] != (m["message"].toString().startsWith("Leo") ? "character" : "location") ||
            owner["id"].toInt() != (owner["kind"] == "character" ? leo : booth))
            return fail(32, "reference to-do is not short or does not name its owner: " + m["message"].toString());
    }
    run(cmd({{"op", "set"}, {"kind", "reference"}, {"id", boothRef}, {"fields", QVariantMap{{"approved", true}}}}));
    if (needing() != QStringList{"Leo"}) return fail(32, "approving the booth picture did not clear its to-do");
    // A shot uses its look's pictures, then the character's, then the location's.
    const int pieceId = v1.value(1).toInt();
    const auto used = plan->shotReferences(pieceId);
    QStringList usedNames;
    for (const auto& u : used) usedNames << u.toMap()["name"].toString();
    if (usedNames != QStringList{"red-coat.png", "mara-face.png", "mara-body.jpg", "booth.png"}) return fail(33, "shot references in the wrong order: " + usedNames.join(", "));
    // Reorder, remove and undo.
    run(cmd({{"op", "ref.move"}, {"id", bodyRef}, {"position", 0}}));
    if (plan->referencesFor("character", mara).value(0).toMap()["id"].toInt() != bodyRef) return fail(34, "move did not put the body picture first");
    editor.undo();
    if (plan->referencesFor("character", mara).value(0).toMap()["id"].toInt() != faceRef) return fail(34, "undo did not restore the order");
    run(cmd({{"op", "remove"}, {"kind", "character"}, {"id", mara}}));
    if (!plan->references().isEmpty() && plan->referencesFor("look", coat).size()) return fail(35, "removing Mara left her look's pictures");
    editor.undo();
    if (plan->referencesFor("character", mara).size() != 2 || plan->referencesFor("look", coat).size() != 1) return fail(35, "undo did not bring Mara's pictures back");
    // A picture missing from the project folder is an error.
    const int strayRef = run(cmd({{"op", "ref.add"}, {"owner", "location"}, {"ownerId", booth}, {"path", stray}}))["id"].toInt();
    const QString strayFile = QDir(QFileInfo(argv[1]).absoluteFilePath()).filePath(plan->referencesFor("location", booth).value(1).toMap()["file"].toString());
    QFile::rename(strayFile, strayFile + ".moved");
    run(cmd({{"op", "set"}, {"kind", "reference"}, {"id", strayRef}, {"fields", QVariantMap{{"notes", "recheck"}}}}));
    if (checksWith(plan, "referenceMissing").size() != 1) return fail(36, "a missing picture file was not reported");
    QFile::rename(strayFile + ".moved", strayFile);
    run(cmd({{"op", "remove"}, {"kind", "reference"}, {"id", strayRef}}));
    if (!checksWith(plan, "referenceMissing").isEmpty()) return fail(36, "missing-picture error stayed after removing it");

    // ---- C. Everything persists
    const QVariantList savedShots = plan->shots(), savedSingers = plan->singers();
    const QVariantMap savedConcept = plan->concept();
    const auto savedChecks = plan->checks();
    if (!editor.openProject(project)) return fail(30, editor.status());
    if (plan->shots() != savedShots || plan->concept() != savedConcept || plan->singers() != savedSingers || plan->checks() != savedChecks)
        return fail(30, "the reopened plan differs");

    // ---- D. The Plan workspace, with real input
    {
        QVariantList clear;
        for (const auto& v : plan->shots()) clear.append(cmd({{"op", "remove"}, {"kind", "shot"}, {"id", v.toMap()["id"]}}));
        if (!ok(run(cmd({{"op", "batch"}, {"label", "Clear shots"}, {"commands", clear}})))) return fail(39, plan->message());
    }
    qmlRegisterType<Monitor>("Chorus", 1, 0, "VideoMonitor");
    qmlRegisterType<WaveformView>("Chorus", 1, 0, "WaveformView");
    QQmlApplicationEngine engine;
    ProjectLibrary library(&editor);
    engine.rootContext()->setContextProperty("editor", &editor);
    engine.rootContext()->setContextProperty("library", &library);
    engine.load(QUrl("qrc:/src/Main.qml"));
    if (engine.rootObjects().isEmpty()) return fail(40, "QML did not load");
    auto window = qobject_cast<QQuickWindow*>(engine.rootObjects().first());
    window->resize(1280, 820);  // the default window size
    if (!QTest::qWaitForWindowExposed(window)) return fail(40, "window not exposed");
    QTest::qWait(300);
    const QString shots = qEnvironmentVariable("CHORUS_SHOTS");
    auto capture = [&](const QString& name) { if (!shots.isEmpty()) window->grabWindow().save(shots + "/" + name + ".png"); };
    auto center = [&](QQuickItem* item) { return item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint(); };
    // Scrolls with the wheel, as a user would, until the item is inside its scrolling view.
    auto scrollIntoView = [&](QQuickItem* item) {
        QQuickItem* view = item->parentItem();
        while (view && !view->inherits("QQuickFlickable")) view = view->parentItem();
        auto settle = [&] {  // wheel scrolling can coast; wait until the view stops
            // A press on a moving Flickable only stops it, so wait until it is still.
            for (int k = 0; view && k < 100; ++k) {
                const double y = view->property("contentY").toDouble();
                QTest::qWait(40);
                if (view->property("contentY").toDouble() == y && !view->property("moving").toBool() && !view->property("flicking").toBool()) return;
            }
        };
        for (int k = 0; view && k < 60; ++k) {
            settle();
            const QRectF r = item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height()));
            if (r.top() >= 0 && r.bottom() <= view->height()) break;
            const QPoint at = center(view);
            QWheelEvent wheel(at, window->mapToGlobal(at), QPoint(0, r.top() < 0 ? 40 : -40), QPoint(0, r.top() < 0 ? 120 : -120),
                              Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
            QGuiApplication::sendEvent(window, &wheel);
            QTest::qWait(30);
        }
        settle();
        if (view) {
            const QRectF r = item->mapRectToItem(view, QRectF(0, 0, item->width(), item->height()));
            if (r.top() < 0 || r.bottom() > view->height())
                qCritical() << "could not scroll" << item->objectName() << "into view:" << r << "view" << view->height() << view->property("contentY");
        }
    };
    auto click = [&](QQuickItem* item, Qt::KeyboardModifiers mods = {}) {
        scrollIntoView(item);
        QTest::mouseClick(window, Qt::LeftButton, mods, center(item));
        QTest::qWait(120);
    };
    auto pause = [&] { QTest::qWait(QGuiApplication::styleHints()->mouseDoubleClickInterval() + 80); };
    auto typeText = [&](const QString& text) { for (const QChar ch : text) QTest::keyClick(window, ch.toLatin1()); QTest::qWait(50); };
    auto selectAll = [&] { QTest::keyClick(window, Qt::Key_A, Qt::ControlModifier); };
    // A combo is opened with a click and chosen with the keyboard, the way a user can.
    auto choose = [&](QQuickItem* combo, int steps) {
        if (!combo) { qCritical() << "combo not on screen"; return; }
        auto popup = combo->property("popup").value<QObject*>();
        QTest::qWait(150);  // let the layout settle after the previous edit
        click(combo);
        if (!waitFor([&] { return popup->property("opened").toBool(); }, 1500)) {
            const QPoint at = center(combo);
            QQuickItem* hit = window->contentItem()->childAt(at.x(), at.y());
            qCritical() << "popup did not open" << combo->objectName() << "at" << at << "combo" << combo->mapRectToScene(QRectF(0, 0, combo->width(), combo->height())) << "hit" << hit;
        }
        for (int k = 0; k < steps; ++k) QTest::keyClick(window, Qt::Key_Down);
        QTest::keyClick(window, Qt::Key_Return);
        waitFor([&] { return !popup->property("visible").toBool(); }, 1500);
        QTest::qWait(100);
    };
    auto selection = [&] { return window->property("selection").toMap(); };

    auto planTab = find(window, "workspacePlan");
    if (!planTab) return fail(41, "no Plan workspace button");
    click(planTab);
    if (!find(window, "planWorkspace") || find(window, "timeline")) return fail(41, "Plan did not replace the timeline");
    auto logline = find(window, "conceptLogline");
    if (!logline || logline->width() < 200) return fail(42, "logline field missing or collapsed");
    click(logline); selectAll();
    typeText("Two strangers share one radio frequency");
    QTest::keyClick(window, Qt::Key_Tab);
    QTest::qWait(100);
    if (plan->concept()["fields"].toMap()["logline"] != "Two strangers share one radio frequency" ||
        plan->concept()["prov"].toMap()["logline"].toMap()["by"] != "user")
        return fail(42, "typed logline not saved as the user's: " + plan->concept()["fields"].toMap()["logline"].toString());
    auto brief = find(window, "conceptBrief");
    if (!brief) return fail(42, "no Brief on the Concept page");
    click(brief); selectAll();
    typeText("Nocturnal and moody");
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    QTest::qWait(100);
    if (plan->concept()["fields"].toMap()["brief"] != "Nocturnal and moody") return fail(42, "the typed brief was not saved: " + plan->concept()["fields"].toMap()["brief"].toString());
    choose(find(window, "conceptAspect"), 2);
    if (plan->concept()["fields"].toMap()["aspect"] != "9:16") return fail(43, "aspect combo did not write 9:16, got " + plan->concept()["fields"].toMap()["aspect"].toString());
    capture("plan-concept");

    click(find(window, "planPage_cast"));
    const int castBefore = plan->characters().size();
    click(find(window, "addCharacter"));
    if (plan->characters().size() != castBefore + 1) return fail(44, "Add character did not add one");
    auto name = find(window, QString("characterName%1").arg(castBefore));
    if (!name) return fail(44, "new character card has no name field");
    click(name); selectAll(); typeText("Ines"); QTest::keyClick(window, Qt::Key_Return);
    QTest::qWait(100);
    if (plan->characters().last().toMap()["fields"].toMap()["name"] != "Ines") return fail(44, "character name not saved");
    // Drop two picture files from the Finder onto Mara's pictures: one undo step, both copied in.
    {
        auto maraCard = find(window, "characterCard0");
        auto dropArea = maraCard ? findItem(maraCard, "refDrop_character") : nullptr;
        if (!dropArea) return fail(47, "no picture strip on Mara's card");
        scrollIntoView(dropArea);
        const int before = plan->referencesFor("character", mara).size();
        const QString extraA = picture("mara-profile.png", QColor("#a07050"), {300, 400});
        const QString extraB = picture("mara-stage.jpg", QColor("#203050"), {800, 450});
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(extraA), QUrl::fromLocalFile(extraB), QUrl::fromLocalFile(pictures.filePath("notes.txt"))});
        const QPoint at = center(dropArea);
        QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, {});
        QGuiApplication::sendEvent(window, &enter);
        QDragMoveEvent move(at + QPoint(3, 0), Qt::CopyAction, &mime, Qt::LeftButton, {});
        QGuiApplication::sendEvent(window, &move);
        QTest::qWait(50);
        QDropEvent dropped(at + QPoint(3, 0), Qt::CopyAction, &mime, Qt::LeftButton, {});
        QGuiApplication::sendEvent(window, &dropped);
        QTest::qWait(200);
        if (plan->referencesFor("character", mara).size() != before + 2) return fail(47, "dropping two pictures did not add both: " + plan->message());
        if (editor.undoText() != "Add 2 references") return fail(47, "the drop was not one undo step: " + editor.undoText());
        // Right-click the newest picture and turn its approval off from the menu.
        auto thumb = findItem(maraCard, QString("ref_character_%1").arg(before + 1));
        if (!thumb) return fail(48, "no thumbnail for the dropped picture");
        scrollIntoView(thumb);
        QTest::mouseClick(window, Qt::RightButton, {}, center(thumb));
        QTest::qWait(120);
        QQuickItem* approved = nullptr;
        waitFor([&] { for (auto i : thumb->findChildren<QQuickItem*>("refApproved")) if (i->isVisible()) approved = i; return approved != nullptr; }, 1500);
        if (!approved) {
            auto menu = thumb->findChild<QObject*>("refMenu");
            const QPoint c = center(thumb);
            return fail(48, QString("right-clicking a picture did not open its menu (menu %1 opened %2, thumb at %3,%4 size %5x%6, hit %7)")
                .arg(menu != nullptr).arg(menu ? menu->property("opened").toBool() : false).arg(c.x()).arg(c.y()).arg(thumb->width()).arg(thumb->height())
                .arg(window->contentItem()->childAt(c.x(), c.y()) ? window->contentItem()->childAt(c.x(), c.y())->metaObject()->className() : "none"));
        }
        click(approved);
        if (plan->referencesFor("character", mara).value(before + 1).toMap()["approved"].toBool()) return fail(48, "the menu did not unapprove the picture");
        capture("plan-references");
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        if (!plan->referencesFor("character", mara).value(before + 1).toMap()["approved"].toBool()) return fail(48, "⌘Z did not re-approve it");
    }
    capture("plan-cast");

    click(find(window, "planPage_arc"));
    auto arcCard = find(window, "arcCard1");
    auto happens = arcCard ? findItem(arcCard, "arcHappens1") : nullptr;
    if (!happens) return fail(45, "no arc card for the second section");
    click(happens);
    typeText("She hears him for the first time");
    QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
    QTest::qWait(100);
    if (plan->arc()[1].toMap()["fields"].toMap()["happens"] != "She hears him for the first time") return fail(45, "arc entry not saved");
    auto lock = findItem(arcCard, "lock_arc_happens");
    if (!lock) return fail(46, "no lock on the arc field");
    click(lock);
    if (!plan->arc()[1].toMap()["prov"].toMap()["happens"].toMap()["locked"].toBool()) return fail(46, "clicking lock did not lock the field");
    r = run(cmd({{"op", "set"}, {"kind", "arc"}, {"id", 1}, {"by", "agent"}, {"mode", "replace"}, {"fields", QVariantMap{{"happens", "Agent text"}}}}));
    if (r["skipped"].toStringList() != QStringList{"happens"}) return fail(46, "agent wrote a field the user locked in the UI");
    capture("plan-arc");

    // ---- E. Shot cards on the timeline, the inspector and the checks panel
    click(find(window, "workspaceEdit"));
    auto timeline = find(window, "timeline");
    if (!timeline || !waitFor([&] { return editor.peaksReady(); })) return fail(50, "no timeline");
    auto zoomTo = [&](int a, int b) { QMetaObject::invokeMethod(timeline, "zoomTo", Q_ARG(QVariant, a), Q_ARG(QVariant, b)); QTest::qWait(150); };
    auto laneAt = [&](int ms, const char* trackY, const char* trackH) {
        const double x = timeline->property("header").toDouble() + (ms / 1000.0 - timeline->property("viewStart").toDouble()) * timeline->property("pps").toDouble();
        const double y = timeline->property(trackY).toDouble() + timeline->property(trackH).toDouble() / 2;
        return timeline->mapToScene(QPointF(x, y)).toPoint();
    };
    auto drag = [&](QPoint from, QPoint to) {
        QTest::mousePress(window, Qt::LeftButton, {}, from);
        for (int k = 1; k <= 10; ++k) QTest::mouseMove(window, from + (to - from) * k / 10, 10);
        QTest::mouseRelease(window, Qt::LeftButton, {}, to, 10);
        QTest::qWait(150);
    };
    const int row = 10;
    zoomTo(line(row)["start"].toInt() - 3000, line(row + 2)["end"].toInt() + 3000);
    pause();
    QTest::mouseDClick(window, Qt::LeftButton, {}, laneAt((line(row)["start"].toInt() + line(row)["end"].toInt()) / 2, "v1Y", "v1H"));
    QTest::qWait(150);
    if (plan->shots().size() != 1) return fail(51, "double-clicking V1 did not plan a shot");
    auto shot = plan->shots()[0].toMap();
    const int id = shot["id"].toInt();
    if (shot["track"].toInt() != 1 || shot["start"].toInt() != frame(line(row)["start"].toInt()) || shot["end"].toInt() != frame(line(row)["end"].toInt()))
        return fail(51, "the new shot does not cover the lyric line");
    if (selection()["kind"] != "shot" || selection()["id"].toInt() != id) return fail(51, "the new shot is not selected");
    pause();

    choose(find(window, "shotType"), 1);
    if (plan->shot(id)["fields"].toMap()["type"] != "lipsync") return fail(52, "type combo did not set lip-sync");
    choose(find(window, "shotWhoAdd"), 1);
    const auto who = plan->shot(id)["fields"].toMap()["who"].toList();
    if (who.size() != 1 || who[0].toMap()["character"].toInt() != mara) return fail(52, "adding someone in frame did not add Mara: " + QString::fromUtf8(QJsonDocument::fromVariant(plan->shot(id)).toJson(QJsonDocument::Compact))
                       + " combo " + QString::number(find(window, "shotWhoAdd") ? find(window, "shotWhoAdd")->property("count").toInt() : -1));

    if (!find(window, "whoEntry0") || find(window, "whoEntry1")) return fail(52, "the inspector does not list Mara in frame");
    for (const auto& ref : plan->referencesFor("character", mara))
        if (!find(window, QString("shotRef%1").arg(ref.toMap()["id"].toInt()))) return fail(52, "the inspector does not show Mara's pictures for the shot");

    // Trim the end toward the next line's end: it snaps there.
    auto card = find(window, QString("shotCard%1").arg(id));
    if (!card || card->width() < 30) return fail(53, "shot card not on screen");
    const QPoint edge = card->mapToScene(QPointF(card->width() - 2, card->height() / 2)).toPoint();
    const QPoint target = laneAt(line(row + 1)["end"].toInt(), "v1Y", "v1H") + QPoint(4, 0);
    drag(edge, QPoint(target.x(), edge.y()));
    if (plan->shot(id)["end"].toInt() != frame(line(row + 1)["end"].toInt()))
        return fail(53, QString("trimmed end %1, expected the snap to line end %2").arg(plan->shot(id)["end"].toInt()).arg(frame(line(row + 1)["end"].toInt())));
    // Drag the card up onto V2, then undo it.
    card = find(window, QString("shotCard%1").arg(id));
    const QPoint grab = center(card);
    drag(grab, QPoint(grab.x(), laneAt(0, "v2Y", "v2H").y()));
    if (plan->shot(id)["track"].toInt() != 2 || plan->shot(id)["start"].toInt() != frame(line(row)["start"].toInt()))
        return fail(54, "dragging the card up did not move it to V2 at the same time");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    if (plan->shot(id)["track"].toInt() != 1) return fail(54, "⌘Z did not put the shot back on V1");
    capture("edit-shot");

    // Select two lines, give them a singer, and plan a shot over them on V2.
    auto lineClip = [&](int r) { return find(window, QString("lyricClip%1").arg(r)); };
    auto strip = [&](QQuickItem* clip) { return clip->mapToScene(QPointF(clip->width() / 2, 8)).toPoint(); };
    QTest::mouseClick(window, Qt::LeftButton, {}, strip(lineClip(row)));
    pause();
    QTest::mouseClick(window, Qt::LeftButton, Qt::ShiftModifier, strip(lineClip(row + 1)));
    QTest::qWait(150);
    if (selection()["kind"] != "lines") return fail(55, "shift-click did not select two lines");
    auto ines = find(window, "singer_Ines");
    if (!ines) return fail(55, "no singer button for Ines");
    click(ines);
    if (plan->singers()[row].toMap()["singers"].toList() != QVariantList{plan->characters().last().toMap()["id"]} ||
        plan->singers()[row + 1].toMap()["singers"].toList() != plan->singers()[row].toMap()["singers"].toList())
        return fail(55, "singer not set on both lines");
    click(find(window, "addShotV2"));
    if (plan->shots().size() != 2 || selection()["kind"] != "shot") return fail(56, "Add shot on V2 did not plan and select a shot");
    // Ines sings, but the lip-sync shot on V1 shows Mara: the check says so.
    if (checksWith(plan, "singer").isEmpty()) return fail(56, "singer mismatch not reported");

    // A whole section on V2 is too long for H3; the checks panel's fix splits it at lyric lines.
    const auto verse = plan->sections()[1].toMap();
    run(cmd({{"op", "remove"}, {"kind", "shot"}, {"id", selection()["id"]}}));
    zoomTo(verse["start"].toInt() - 2000, verse["end"].toInt() + 2000);
    pause();
    QTest::mouseClick(window, Qt::LeftButton, {}, laneAt((verse["start"].toInt() + verse["end"].toInt()) / 2, "sectionY", "sectionH"));
    QTest::qWait(150);
    if (selection()["kind"] != "region") return fail(57, "clicking the section did not select it");
    click(find(window, "addShotV2"));
    const int longId = selection()["id"].toInt();
    choose(find(window, "shotRoute"), 1);
    if (plan->shot(longId)["fields"].toMap()["route"] != "h3-lipsync") return fail(57, "route combo did not set H3 lip-sync");
    if (checksWith(plan, "tooLong").isEmpty()) return fail(57, QString("a %1 s section shot was not too long").arg((plan->shot(longId)["end"].toInt() - plan->shot(longId)["start"].toInt()) / fps));
    click(find(window, "panelChecks"));
    auto list = find(window, "checksList");
    if (!list) return fail(58, "no checks list");
    QQuickItem* fix = nullptr;
    for (int k = 0; k < 30 && !fix; ++k) {
        fix = findItem(list, "fix_splitLines");
        if (!fix) { QMetaObject::invokeMethod(list, "positionViewAtEnd"); QTest::qWait(50); }
    }
    if (!fix) return fail(58, "no Split at lines fix in the checks panel");
    capture("edit-checks");
    click(fix);
    if (!checksWith(plan, "tooLong").isEmpty() || plan->shots().size() < 3) return fail(58, "the fix did not split the long shot");
    // Clicking a check about a shot selects that shot.
    QMetaObject::invokeMethod(list, "positionViewAtBeginning");
    QTest::qWait(100);
    QQuickItem* fieldsRow = nullptr;
    for (int i = 0; i < 40 && !fieldsRow; ++i) fieldsRow = findItem(list, QString("check_fields_%1").arg(i));
    if (!fieldsRow) return fail(59, "no incomplete-shot check listed");
    const int expectShot = fieldsRow->property("modelData").toMap()["shots"].toList().value(0).toInt();
    QTest::mouseClick(window, Qt::LeftButton, {}, fieldsRow->mapToScene(QPointF(30, fieldsRow->height() / 2)).toPoint());
    QTest::qWait(150);
    if (selection()["kind"] != "shot" || selection()["id"].toInt() != expectShot) return fail(59, "clicking the check did not select its shot");
    if (!find(window, "coverageGap") || !find(window, "coverageCovered")) return fail(60, "coverage is not drawn on the ruler");

    // A reference to-do opens the card where it is fixed: put Leo (no pictures) in a shot, click his to-do.
    {
        const int shotWithMara = plan->shots().value(0).toMap()["id"].toInt();
        run(cmd({{"op", "set"}, {"kind", "shot"}, {"id", shotWithMara},
                 {"fields", QVariantMap{{"who", QVariantList{QVariantMap{{"character", mara}}, QVariantMap{{"character", leo}}}}}}}));
        QTest::qWait(150);
        const auto model = list->property("model").toList();
        int at = -1;
        for (int i = 0; i < model.size(); ++i) {
            const auto m = model[i].toMap();
            if (m["code"] == "reference" && m["owner"].toMap()["id"].toInt() == leo && m["owner"].toMap()["kind"] == "character") at = i;
        }
        if (at < 0) return fail(60, "no reference to-do for Leo in the checks panel");
        QMetaObject::invokeMethod(list, "positionViewAtIndex", Q_ARG(int, at), Q_ARG(int, 1));
        QTest::qWait(100);
        auto row = findItem(list, QString("check_reference_%1").arg(at));
        if (!row) return fail(60, "Leo's to-do is not on screen");
        QTest::mouseClick(window, Qt::LeftButton, {}, row->mapToScene(QPointF(30, row->height() / 2)).toPoint());
        QTest::qWait(300);
        int leoIndex = 0;
        for (int i = 0; i < plan->characters().size(); ++i) if (plan->characters()[i].toMap()["id"].toInt() == leo) leoIndex = i;
        auto workspace = find(window, "planWorkspace");
        auto card = find(window, QString("characterCard%1").arg(leoIndex));
        if (!workspace || workspace->property("page") != "cast" || !card) return fail(61, "clicking the to-do did not open the Cast page");
        QQuickItem* view = card->parentItem();
        while (view && !view->inherits("QQuickFlickable")) view = view->parentItem();
        const QRectF r = card->mapRectToItem(view, QRectF(0, 0, card->width(), qMin(card->height(), view->height())));
        if (!card->property("flashing").toBool() || r.top() < -1 || r.top() > view->height() / 2)
            return fail(61, QString("Leo's card is not highlighted near the top of the view (top %1 of %2)").arg(r.top()).arg(view->height()));
        capture("check-opens-card");
        click(find(window, "workspaceEdit"));
        click(find(window, "panelChecks"));
    }

    // Delete removes the selected shot; ⌘Z brings it back.
    click(find(window, "panelInspector"));
    const int count = plan->shots().size();
    QTest::keyClick(window, Qt::Key_Backspace);
    if (plan->shots().size() != count - 1 || !selection().isEmpty()) return fail(61, "Delete did not remove the selected shot");
    QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
    if (plan->shots().size() != count) return fail(61, "⌘Z did not restore the deleted shot");

    // ---- E2. Links between sections, cast, locations and shots
    {
        // A known set of shots: section 1 holds Mara (in her coat, at the booth), an empty
        // shot and Leo on V2; section 2 holds Mara again. Leo's second shot starts 0.2 s before
        // section 2, so it counts only there.
        const auto s1 = plan->sections()[1].toMap(), s2 = plan->sections()[2].toMap();
        const int a = frame(s1["start"].toInt()), b = frame(s1["end"].toInt()), c = frame(s2["end"].toInt());
        const int mid = (a + b) / 2;
        QVariantList commands;
        for (const auto& v : plan->shots()) commands.append(cmd({{"op", "remove"}, {"kind", "shot"}, {"id", v.toMap()["id"]}}));
        auto shotCmd = [&](const char* ref, int track, int start, int end, const QVariant& who, const QVariant& where) {
            return cmd({{"op", "shot.add"}, {"ref", ref}, {"track", track}, {"start", start}, {"end", end},
                        {"fields", QVariantMap{{"type", "story"}, {"purpose", QString(ref)}, {"who", who}, {"where", where}}}});
        };
        commands << shotCmd("maraBooth", 1, a, mid, QVariantList{QVariantMap{{"character", mara}, {"look", coat}}}, booth)
                 << shotCmd("nobody", 1, mid, b, "none", "none")
                 << shotCmd("leoTop", 2, a + 2, mid, QVariantList{QVariantMap{{"character", leo}}}, "none")
                 << shotCmd("maraLater", 1, b, c, QVariantList{QVariantMap{{"character", mara}}}, booth)
                 << shotCmd("leoLate", 2, b - int(0.2 * fps), std::min(c, b + int(3 * fps)), QVariantList{QVariantMap{{"character", leo}}}, "none");
        r = run(cmd({{"op", "batch"}, {"label", "Links shots"}, {"commands", commands}}));
        if (!ok(r)) return fail(63, "could not plan the links shots: " + plan->message());
        const int maraBooth = r["refs"].toMap()["maraBooth"].toInt();

        auto section = plan->links("section", 1);
        auto names = [](const QVariantList& l) { QStringList n; for (const auto& v : l) n << v.toMap()["name"].toString() + ":" + v.toMap()["shots"].toString(); return n.join(","); };
        if (section["shots"].toList().size() != 3 || names(section["characters"].toList()) != "Mara:1,Leo:1" ||
            section["empty"].toInt() != 1 || names(section["locations"].toList()) != "Radio booth:1" ||
            section["characters"].toList()[0].toMap()["looks"].toStringList() != QStringList{plan->looks()[0].toMap()["fields"].toMap()["name"].toString()})
            return fail(63, "section links: " + QString::fromUtf8(QJsonDocument::fromVariant(section).toJson(QJsonDocument::Compact)));
        auto person = plan->links("character", leo);
        QStringList where;
        for (const auto& v : person["sections"].toList()) where << v.toMap()["ordinal"].toString() + ":" + v.toMap()["shots"].toString();
        if (person["shots"].toList().size() != 2 || where.join(",") != "1:1,2:1" || person["marks"].toList().size() != 2)
            return fail(64, "Leo's links: " + where.join(","));
        auto place = plan->links("location", booth);
        if (names(place["characters"].toList()) != "Mara:2" || place["sections"].toList().size() != 2) return fail(64, "the booth's links");
        const auto rows = plan->overview()["rows"].toList();
        if (rows.size() != plan->characters().size() + plan->locations().size() || rows[0].toMap()["cells"].toList().value(1).toInt() != 1)
            return fail(64, "overview rows");
        if (!plan->links("character", 999).isEmpty() || !plan->links("section", 99).isEmpty()) return fail(64, "links for nothing");

        // With the mouse: click the section's card, follow a chip, a section, go back, and open a shot.
        click(find(window, "workspacePlan"));
        click(find(window, "planPage_arc"));
        auto panel = find(window, "linksPanel");
        auto title = find(window, "linksTitle");
        if (!panel || panel->width() < 300 || !title) return fail(65, "no links panel beside the page at 1280 wide");
        // Typing in the cards earlier picked them; × goes back to who and where.
        if (title->property("text") != plan->sections()[1].toMap()["label"]) return fail(65, "the arc field typed in earlier did not pick its section");
        click(find(window, "linksClose"));
        if (title->property("text") != "Who and where" || !find(window, "overviewRow0")) return fail(65, "× did not go back to who and where");
        // A card is picked by clicking its empty top-right corner, away from fields and buttons.
        auto pick = [&](QQuickItem* card) {
            scrollIntoView(card);
            QTest::mouseClick(window, Qt::LeftButton, {}, card->mapToScene(QPointF(card->width() - 20, 18)).toPoint());
            QTest::qWait(120);
        };
        auto card1 = find(window, "arcCard1");
        pick(card1);
        if (title->property("text") != s1["label"] || !card1->property("picked").toBool()) return fail(65, "clicking the card did not show its links");
        if (!find(window, "laneShot1") || !find(window, "linksShot3")) return fail(65, "the section's shots are not drawn");
        QQuickItem* leoChip = nullptr;
        QStringList chips;
        for (auto item : find(window, "arcLinks1")->childItems())
            if (item->inherits("QQuickAbstractButton")) { chips << item->property("text").toString(); if (item->property("text") == "Leo") leoChip = item; }
        if (!leoChip) return fail(66, "no Leo chip on the section's card: " + chips.join(", "));
        click(leoChip);
        if (title->property("text") != "Leo") return fail(66, "the chip did not open Leo's links");
        auto later = find(window, "linksSection1");
        if (!later) return fail(66, "Leo's second section is not listed");
        click(later);
        if (title->property("text") != s2["label"]) return fail(66, "the section row did not open it");
        click(find(window, "linksBack"));
        if (title->property("text") != "Leo") return fail(67, "Back did not return to Leo");
        QTest::keyClick(window, Qt::Key_BracketLeft, Qt::ControlModifier);
        QTest::qWait(100);
        if (title->property("text") != s1["label"]) return fail(67, "⌘[ did not go back to the section");
        // Typing in another card picks that card.
        auto happens2 = find(window, "arcHappens2");
        click(happens2);
        if (title->property("text") != s2["label"]) return fail(68, "focusing a field did not pick its card");
        QTest::keyClick(window, Qt::Key_Escape);
        // Show card on a character opens the Cast page at that card.
        click(find(window, "planPage_cast"));
        pick(find(window, "characterCard0"));
        if (title->property("text") != "Mara") return fail(68, "clicking Mara's card did not pick her");
        click(find(window, "planPage_arc"));
        click(find(window, "linksShowCard"));
        if (find(window, "planWorkspace")->property("page") != "cast" || !find(window, "characterCard0")->property("flashing").toBool())
            return fail(68, "Show card did not open Mara's card");
        capture("plan-links");
        // A shot opens in Edit, selected, with the playhead on it.
        auto shotRow = find(window, QString("linksShot%1").arg(plan->shot(maraBooth)["number"].toInt()));
        if (!shotRow) return fail(69, "Mara's shot is not listed");
        click(shotRow);
        if (!find(window, "timeline") || selection()["kind"] != "shot" || selection()["id"].toInt() != maraBooth ||
            std::abs(editor.frame() - a) > 1)
            return fail(69, QString("the shot did not open in Edit (frame %1, start %2)").arg(editor.frame()).arg(a));
        // A narrow window: the panel opens over the page from the toggle, and closes again.
        click(find(window, "workspacePlan"));
        window->resize(1000, 820);
        QTest::qWait(200);
        if (find(window, "linksPanel") || find(window, "linksOverlay"))
            return fail(70, QString("the panel stayed docked on a narrow window (window %1, workspace %2, docked %3)").arg(window->width())
                .arg(find(window, "planWorkspace")->width()).arg(find(window, "planWorkspace")->property("docked").toBool()));
        click(find(window, "linksToggle"));
        auto overlay = find(window, "linksOverlay");
        if (!overlay || overlay->width() < 300) return fail(70, "the toggle did not open the panel");
        capture("plan-links-narrow");
        click(findItem(overlay, "linksClose"));
        if (find(window, "linksOverlay")) return fail(70, "the panel did not close");
        window->resize(1280, 820);
        QTest::qWait(200);
        // Removing the character in view falls back to who and where.
        click(find(window, "planPage_cast"));
        pick(find(window, "characterCard1"));
        if (title->property("text") != "Leo") return fail(71, "clicking Leo's card did not pick him");
        run(cmd({{"op", "remove"}, {"kind", "character"}, {"id", leo}}));
        QTest::qWait(100);
        if (title->property("text") != "Who and where") return fail(71, "a removed character stayed in the panel");
        editor.undo();
        click(find(window, "workspaceEdit"));
    }

    // ---- E3. Long prompts: folded on the card, open while editing, and the large editor
    {
        QStringList many;
        for (int i = 0; i < 120; ++i) many << "soft" << "rim" << "light";
        const QString longPrompt = "Studio portrait, " + many.join(" ");
        const int words = longPrompt.split(' ', Qt::SkipEmptyParts).size();
        run(cmd({{"op", "set"}, {"kind", "character"}, {"id", mara}, {"fields", QVariantMap{{"prompt", longPrompt}}}}));
        click(find(window, "workspacePlan"));
        click(find(window, "planPage_cast"));
        auto card = find(window, "characterCard0");
        auto field = card ? findItem(card, "characterPrompt0") : nullptr;
        if (!field) return fail(72, "no picture prompt on Mara's card");
        scrollIntoView(field);
        auto more = findItem(field, "showAll");
        if (!field->property("collapsed").toBool() || !more || field->height() > 110 ||
            more->property("text").toString() != QString("%1 words  ·  Show all").arg(words))
            return fail(72, QString("the long prompt is not folded (height %1, %2)").arg(field->height()).arg(more ? more->property("text").toString() : "no Show all"));
        capture("plan-prompt-folded");
        const double folded = field->height();
        click(more);
        if (field->property("collapsed").toBool() || field->height() < folded + 40) return fail(72, "Show all did not open the prompt");
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(150);
        if (!field->property("collapsed").toBool()) return fail(72, "leaving the field did not fold it again");
        // The large editor, with what the prompt follows.
        click(findItem(card, "expand_character_prompt"));
        auto promptEditor = window->findChild<QObject*>("promptEditor");
        if (!promptEditor || !waitFor([&] { return promptEditor->property("opened").toBool(); }, 1500)) return fail(73, "⤢ Editor did not open the editor");
        auto editorText = find(window, "promptEditorText");
        if (!editorText || editorText->property("text") != longPrompt || find(window, "promptEditorTitle")->property("text") != "Mara")
            return fail(73, "the editor does not show Mara's prompt");
        bool identity = false;
        for (const auto& v : promptEditor->property("context").toList())
            identity |= v.toMap()["label"] == "Identity" && v.toMap()["text"] == plan->item("character", mara)["fields"].toMap()["identity"];
        if (!identity) return fail(73, "the editor does not show Mara's identity beside the prompt");
        capture("plan-prompt-editor");
        selectAll();
        typeText("Close up of a face");
        QTest::keyClick(window, Qt::Key_Return, Qt::ControlModifier);
        QTest::qWait(150);
        if (promptEditor->property("visible").toBool() || plan->item("character", mara)["fields"].toMap()["prompt"] != "Close up of a face")
            return fail(74, "⌘Return did not save the prompt and close");
        QTest::keyClick(window, Qt::Key_Z, Qt::ControlModifier);
        if (plan->item("character", mara)["fields"].toMap()["prompt"] != longPrompt) return fail(74, "⌘Z did not bring the long prompt back");
        click(findItem(card, "expand_character_prompt"));
        waitFor([&] { return promptEditor->property("opened").toBool(); }, 1500);
        typeText(" more");
        QTest::keyClick(window, Qt::Key_Escape);
        QTest::qWait(150);
        if (promptEditor->property("visible").toBool() || plan->item("character", mara)["fields"].toMap()["prompt"] != longPrompt)
            return fail(74, "Esc did not cancel the editor");
        // A look's prompt wraps over several lines now.
        auto lookPrompt = find(window, QString("lookPrompt%1").arg(coat));
        if (!lookPrompt || !lookPrompt->property("multiline").toBool()) return fail(75, "the look's prompt is still one line");
        click(find(window, "workspaceEdit"));
    }

    // ---- F. Reopen: the plan made with the mouse and keyboard persisted.
    const QVariantList uiShots = plan->shots();
    const QVariantMap uiConcept = plan->concept();
    const QVariantList uiArc = plan->arc();
    if (!editor.openProject(project) || plan->shots() != uiShots || plan->concept() != uiConcept || plan->arc() != uiArc)
        return fail(62, "the plan differs after reopening");
    qInfo() << "PASS: commands (provenance, locks, agent modes, batches, undo, arc order, singers, split at lines, checks, stale)"
            << "and real input (Plan pages, typing, combos, locks, double-click a shot, snap trim, drag to V2, undo, singers,"
            << "Add shot, checks panel fix and selection, ruler coverage, Delete), reference pictures (import, roles, approval,"
            << "order, cascade, missing files, G3, drag and drop, menu, shot references), links (section, character and location"
            << "links, overview, card summaries, chips, Back, focus, Show card, open a shot in Edit, narrow window), long prompts"
            << "(folded, Show all, the large editor with context, ⌘Return, ⌘Z, Esc), reopen.";
    return 0;
}
