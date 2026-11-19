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
public:
    explicit AgentBridge(Editor* editor, QObject* parent = nullptr);
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
    Q_INVOKABLE void copy(const QString& text) const;

    // One tool call: {content: [...], isError}.
    QJsonObject call(const QString& tool, const QJsonObject& arguments);
signals:
    void stateChanged();
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
};
