#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QVariantList>
#include <QVariantMap>
#include <functional>

struct Project;
class Lyrics;
class QUndoStack;

// Pre-production plan: concept, cast, locations, the arc for each song section, who sings each
// line, and shots on the picture tracks. Everything is written through run(), one command layer
// shared by the UI, the planned MCP server and any future in-app model runner. Each run() is one
// undo step on the editor's stack, and every field records who wrote it (user or agent) and
// whether it is locked against agent passes. See docs/PLANNING.md.
//
// Stored in project.sqlite as plan_items(kind, id, data JSON). Shot times are project frames,
// start inclusive and end exclusive.
class Plan : public QObject {
    Q_OBJECT
    Q_PROPERTY(QVariantMap concept READ concept NOTIFY changed)
    Q_PROPERTY(QVariantList characters READ characters NOTIFY changed)
    Q_PROPERTY(QVariantList looks READ looks NOTIFY changed)
    Q_PROPERTY(QVariantList locations READ locations NOTIFY changed)
    Q_PROPERTY(QVariantList arc READ arc NOTIFY changed)
    Q_PROPERTY(QVariantList sections READ sections NOTIFY changed)
    Q_PROPERTY(QVariantList singers READ singers NOTIFY changed)
    Q_PROPERTY(QVariantList shots READ shots NOTIFY changed)
    Q_PROPERTY(QVariantList references READ references NOTIFY changed)
    Q_PROPERTY(QVariantList checks READ checks NOTIFY changed)
    Q_PROPERTY(QVariantList gates READ gates NOTIFY changed)
    Q_PROPERTY(QVariantList coverage READ coverage NOTIFY changed)
    Q_PROPERTY(int errorCount READ errorCount NOTIFY changed)
    Q_PROPERTY(int warningCount READ warningCount NOTIFY changed)
    Q_PROPERTY(QVariantMap options READ options CONSTANT)
    Q_PROPERTY(QString message READ message NOTIFY messageChanged)
public:
    explicit Plan(Lyrics* lyrics, QObject* parent = nullptr);
    void setHistory(QUndoStack* stack) { history = stack; }
    bool load(Project& next, QString& error);
    void unload();

    // The command entry point. A command is {op, ...}; the result is {ok, error, id, skipped}.
    //   set        {kind, id, fields}          concept (id 1), character, look, location, arc (id = section ordinal), shot
    //   add        {kind, fields, character}   character, look (needs character), location
    //   remove     {kind, id}                  character (and its looks), look, location, shot
    //   lock       {kind, id, field, locked}
    //   singers    {rows: [...], singers: [character ids]}
    //   shot.add   {track, start, end, fields}
    //   shot.retime {id, track, start, end}
    //   shot.split {id, at}                    at a frame inside the shot
    //   shot.splitLines {id}                   into pieces the route can render, cut between lyric lines
    //   shot.markCurrent {ids}                 clears the stale mark after upstream changes
    //   ref.add    {owner, ownerId, path, role} copies a prepared image into Media/References and
    //                                          lists it on a character, look or location (approved
    //                                          when the user adds it, awaiting approval from an agent)
    //   ref.move   {id, position}              reorders a reference among its owner's
    //   set/remove {kind: "reference", ...}    role, approved and notes; or take it off the list
    //   batch      {label, commands: [...]}    all or nothing, one undo step. A command may carry
    //                                          "ref": "name"; later commands write "$name" for the
    //                                          id it created. The result lists each command's id.
    // Common: by = "user" (default) or "agent". Agent writes take mode = "agent" (default: fill
    // empty or agent-owned fields), "fill" (empty only) or "replace" (also user-owned); locked
    // fields are never written by an agent, and skipped fields are listed in the result. Inside an
// agent's batch every command is the agent's, whatever it says.
    Q_INVOKABLE QVariantMap run(const QVariantMap& command);

    QVariantMap concept() const;
    QVariantList characters() const { return listOf("character"); }
    QVariantList looks() const { return listOf("look"); }
    QVariantList locations() const { return listOf("location"); }
    QVariantList arc() const;
    QVariantList sections() const;
    QVariantList singers() const;
    QVariantList shots() const;
    QVariantList references() const;
    // A character's, look's or location's references, in order.
    Q_INVOKABLE QVariantList referencesFor(const QString& owner, int id) const;
    // What a shot will be generated from: approved references of its looks, characters and location.
    Q_INVOKABLE QVariantList shotReferences(int shot) const;
    QVariantList checks() const;
    QVariantList gates() const;
    QVariantList coverage() const;
    int errorCount() const;
    int warningCount() const;
    QVariantMap options() const;
    QString message() const { return notice; }
    Q_INVOKABLE QVariantMap shot(int id) const;
    // Every kind's fields with their types and allowed values, for agents.
    QVariantMap schema() const;
    // Checks that concern one shot, for the inspector.
    Q_INVOKABLE QVariantList checksFor(int shot) const;
    // Lyric rows overlapping a frame span, with their text and singers.
    Q_INVOKABLE QVariantList linesIn(int start, int end) const;
    // The longest shot a route can render, in frames; 0 when unlimited.
    Q_INVOKABLE int maxFrames(const QString& route) const;

    // Used by the undo command.
    struct Change { QString kind; int id; QJsonObject before, after; };
    void apply(const QVector<Change>& changes, bool forward, const QString& done);
signals:
    void changed();
    void messageChanged();
private:
    Project* project = nullptr;
    Lyrics* lyrics = nullptr;
    QUndoStack* history = nullptr;
    QMap<QString, QMap<int, QJsonObject>> items;
    int clock = 0;
    QString notice;
    // Open transaction: rows touched by the command being run.
    QVector<Change> pending;
    QString pendingLabel;
    mutable bool checksValid = false;
    mutable QVariantList cachedChecks;

    struct Result { bool ok = true; QString error; int id = 0; QStringList skipped; QVariantList ids; QVariantMap refs; };
    Result dispatch(const QVariantMap& command, const QString& by, const QString& mode);
    Result setFields(const QString& kind, int id, const QVariantMap& fields, const QString& by, const QString& mode);
    Result shotAdd(const QVariantMap& c, const QString& by);
    Result shotRetime(const QVariantMap& c);
    Result shotSplit(int id, const QVector<int>& cuts);
    Result refAdd(const QVariantMap& c, const QString& by);
    QList<int> refIds(const QString& owner, int id) const;
    void dropReferences(const QString& owner, int id);
    QJsonObject row(const QString& kind, int id) const { return items.value(kind).value(id); }
    bool has(const QString& kind, int id) const { return items.value(kind).contains(id); }
    void write(const QString& kind, int id, const QJsonObject& after);
    int nextId(const QString& kind) const;
    bool persist(const QVector<Change>& changes, QString& error);
    void invalidate();
    void say(const QString& text);
    QVariantList listOf(const QString& kind) const;
    QVariantMap exported(const QString& kind, int id) const;
    QString checkField(const QString& kind, const QString& field, const QJsonValue& value) const;
    QString overlap(int track, int start, int end, int except) const;
    int songFrames() const;
    double fps() const;
    int msToFrame(int ms) const;
    int frameToMs(int frame) const;
    QList<int> shotOrder() const;
    int shotNumber(int id) const;
    QVariantList computeChecks() const;
    QString clockText(int frame) const;
    QString nameOf(const QString& kind, int id) const;
};
