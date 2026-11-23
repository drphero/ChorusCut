#pragma once
#include <QJsonObject>
#include <QLocalServer>
#include <QObject>
#include <QPointer>
#include <QStringList>

class Editor;
class QLocalSocket;

// The app's side of agent control. choruscut-mcp (the stdio MCP server an agent launches)
// connects to this local socket and passes on tool calls; they run here, in the app, through
// the same plan commands the UI uses, so the user sees each change live and can undo each pass.
// Only the user's account can reach the socket.
class AgentBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool listening READ listening NOTIFY stateChanged)
    Q_PROPERTY(QStringList clients READ clients NOTIFY stateChanged)
    Q_PROPERTY(QString note READ note NOTIFY stateChanged)
    Q_PROPERTY(QString shimPath READ shimPath CONSTANT)
    Q_PROPERTY(QString claudeCommand READ claudeCommand CONSTANT)
    Q_PROPERTY(QString codexCommand READ codexCommand CONSTANT)
    // Antigravity has no add command: its MCP servers live in a JSON file this app merges into.
    Q_PROPERTY(QString antigravityConfig READ antigravityConfig CONSTANT)
    // "missing", "added" (this build's server), "other" (another build's), or "unreadable".
    Q_PROPERTY(QString antigravityState READ antigravityState NOTIFY antigravityChanged)
    // Starts Claude Code with ChorusCut as a channel, so requests from the app reach it live.
    Q_PROPERTY(QString channelCommand READ channelCommand CONSTANT)
    // Whether an agent that can take channel events (Claude Code) is connected now.
    Q_PROPERTY(bool liveClient READ liveClient NOTIFY stateChanged)
public:
    explicit AgentBridge(Editor* editor, QObject* parent = nullptr);
    ~AgentBridge() override;
    // Starts serving agents. Only the interactive app (and the MCP check) calls this.
    bool start();
    // Stops serving and drops every agent connection, as quitting the app does.
    void stop();
    bool listening() const { return server.isListening(); }
    QStringList clients() const;
    QString note() const { return notice; }
    QString shimPath() const;
    QString claudeCommand() const;
    QString codexCommand() const;
    // ~/.gemini/config/mcp_config.json, or CHORUS_ANTIGRAVITY_CONFIG (tests).
    QString antigravityConfig() const;
    QString antigravityState() const;
    // Sets mcpServers.ChorusCut to this build's choruscut-mcp, keeping every other entry. A file
    // that is not a JSON object is left alone. True when the file now has the entry.
    Q_INVOKABLE bool addToAntigravity();
    // Rereads the file, for when the sheet opens after someone edited it.
    Q_INVOKABLE void checkAntigravity() { emit antigravityChanged(); }
    QString channelCommand() const { return "claude --dangerously-load-development-channels server:ChorusCut"; }
    bool liveClient() const;
    // Pushes a user's message to every connected Claude Code; true when one was there to take it.
    bool push(const QJsonObject& event);
    Q_INVOKABLE void copy(const QString& text) const;

    // One tool call: {content: [...], isError}.
    QJsonObject call(const QString& tool, const QJsonObject& arguments);
signals:
    void stateChanged();
    void antigravityChanged();
    // An agent's pass changed the plan; label is its undo text.
    void passApplied(const QString& label, int changes, const QString& client);
private:
    Editor* editor;
    QLocalServer server;
    QMap<QLocalSocket*, QString> peers;
    QString notice;
    QString currentClient;
    void accept();
    void read(QLocalSocket* socket);
    QJsonObject project() const;
    QJsonObject song(bool words) const;
    QJsonObject planView() const;
    QJsonObject checks() const;
    QJsonObject schema() const;
    QJsonObject apply(const QJsonObject& arguments);
    QJsonObject reference(int id) const;
    QJsonObject requestsTool(const QJsonObject& arguments);
    QJsonObject replyTool(const QJsonObject& arguments);
    QJsonObject askTool(const QJsonObject& arguments);
};
