#pragma once
#include <QJsonObject>
#include <QMap>
#include <QObject>
#include <QVariantList>

class Editor;

// Conversations between the user and their agent about the plan, kept in the project (table
// "requests"), outside the undo stack. A thread is about something (a field of a character,
// look, location, arc entry, shot or the concept, a whole item, or nothing in particular) and
// is started by the user ("Ask the agent…") or by the agent (ask_user). Its turn is "agent"
// (waiting for the agent), "user" (waiting for the user) or "done".
//
// Every message the user sends is also announced (signal posted), so the agent bridge can push
// it into a running Claude Code session as a channel event. Pushes may be dropped silently when
// the session was not started with channels, so the list here is what counts: an agent without
// channels reads it with get_requests.
class Requests : public QObject {
    Q_OBJECT
    // Threads, open ones first, newest first: [{id, about: {kind, id, field}, label, origin,
    // turn, seen, sent, messages: [{from, text, options}], updated}]
    Q_PROPERTY(QVariantList list READ list NOTIFY changed)
    Q_PROPERTY(int waitingOnUser READ waitingOnUser NOTIFY changed)
    Q_PROPERTY(int waitingOnAgent READ waitingOnAgent NOTIFY changed)
public:
    explicit Requests(Editor* editor);
    QVariantList list() const;
    int waitingOnUser() const;
    int waitingOnAgent() const;

    // The user's side. kind "" asks about nothing in particular; field "" about the whole item.
    // label describes it ("Cherry · Picture prompt"). Returns the new thread's id, or 0.
    Q_INVOKABLE int ask(const QString& kind, int id, const QString& field, const QString& label, const QString& text);
    Q_INVOKABLE bool reply(int thread, const QString& text);
    Q_INVOKABLE bool close(int thread);
    Q_INVOKABLE bool reopen(int thread);
    Q_INVOKABLE bool remove(int thread);
    // Open threads about one item (field "" counts every field of it).
    Q_INVOKABLE QVariantList about(const QString& kind, int id, const QString& field = {}) const;

    // The agent's side, for the MCP tools. forAgent marks what it returns as seen.
    QJsonArray forAgent(bool includeDone);
    QJsonObject viewContext(int view) const;
    QJsonObject takeContext(int shot) const;
    bool agentReply(int thread, const QString& text, bool done, QString& error);
    int agentAsk(const QString& question, const QJsonObject& about, const QStringList& options, QString& error);
    // Whether the last push reached a client that can take channel events.
    void markSent(int thread, bool sent);

    void load();
    void unload();
signals:
    void changed();
    // A user's message for the agent: {thread, text, about, label, kind ("request" or "answer")}.
    void posted(const QJsonObject& event);
private:
    Editor* editor;
    QMap<int, QJsonObject> threads;
    bool save(int id);
    bool ensureTable();
    QString currentLabel(const QJsonObject& thread) const;
    QJsonObject eventFor(int id, const QString& text, const QString& kind) const;
};
