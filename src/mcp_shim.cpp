// choruscut-mcp: the MCP server an agent (Claude Code, Codex or any MCP client) launches over
// stdio. It answers the protocol itself and passes each tool call to the running ChorusCut over
// a per-user local socket, so every change goes through the app's own commands and undo.
// Nothing but protocol messages goes to stdout.
//
// It is also a Claude Code channel: when the user asks the agent something in the app, the app
// sends it here and it goes to the agent as a notifications/claude/channel event, for sessions
// started with --dangerously-load-development-channels server:ChorusCut. Other sessions (and
// other clients) drop or never get these events; the request waits in the app for get_requests.
#include "agent_tools.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QTimer>
#include <iostream>
#include <string>
#include <thread>

namespace {
const QStringList kVersions = {"2025-06-18", "2025-03-26", "2024-11-05"};

void send(const QJsonObject& message) {
    std::cout << QJsonDocument(message).toJson(QJsonDocument::Compact).toStdString() << '\n' << std::flush;
}
void reply(const QJsonValue& id, const QJsonObject& result) { send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}); }
void fail(const QJsonValue& id, int code, const QString& text) {
    send({{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", text}}}});
}
QJsonObject toolError(const QString& text) {
    return {{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", text}}}}, {"isError", true}};
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QLocalSocket socket;
    QString client = "MCP client";
    qint64 serial = 0;
    bool initialized = false, inCall = false;
    // Channel events reach Claude Code only; other clients are not sent them.
    auto channel = [&](const QJsonObject& event) {
        if (!client.contains("claude", Qt::CaseInsensitive)) return;
        send({{"jsonrpc", "2.0"}, {"method", "notifications/claude/channel"}, {"params", event}});
    };
    // Lines the app sends without being asked: {"event": {content, meta}}.
    auto drain = [&] {
        while (socket.canReadLine()) {
            const auto message = QJsonDocument::fromJson(socket.readLine()).object();
            if (message.contains("event")) channel(message.value("event").toObject());
        }
    };
    auto connected = [&] {
        // Notice an app that quit or restarted since the last call (its close is still queued),
        // and connect afresh, so the first call after a restart works. A call that was already
        // sent is never sent again, so a pass cannot be applied twice.
        if (socket.state() == QLocalSocket::ConnectedState) socket.waitForDisconnected(0);
        if (socket.state() == QLocalSocket::ConnectedState) return true;
        socket.abort();
        socket.connectToServer(AgentTools::socketName());
        if (!socket.waitForConnected(1500)) return false;
        socket.waitForBytesWritten(1000);  // the hello, written on connecting
        return true;
    };
    // Every connection starts by saying who the agent is, so the app can show it.
    QObject::connect(&socket, &QLocalSocket::connected, &app, [&] {
        socket.write(QJsonDocument(QJsonObject{{"hello", client}}).toJson(QJsonDocument::Compact) + '\n');
        socket.flush();
    });
    // Sends one tool call to the app and waits for its answer.
    auto call = [&](const QString& name, const QJsonObject& arguments) -> QJsonObject {
        if (!connected())
            return toolError("ChorusCut is not running. Ask the user to open ChorusCut and their project, then try again.");
        const qint64 id = ++serial;
        socket.write(QJsonDocument(QJsonObject{{"id", id}, {"tool", name}, {"arguments", arguments}}).toJson(QJsonDocument::Compact) + '\n');
        socket.waitForBytesWritten(5000);
        while (true) {
            while (!socket.canReadLine()) {
                if (socket.state() != QLocalSocket::ConnectedState || !socket.waitForReadyRead(180000)) {
                    socket.abort();
                    return toolError("ChorusCut closed or stopped answering. Ask the user to check the app, then try again.");
                }
            }
            const auto answer = QJsonDocument::fromJson(socket.readLine()).object();
            if (answer.contains("event")) { channel(answer.value("event").toObject()); continue; }
            if (answer.value("id").toInteger() == id) return answer.value("result").toObject();
        }
    };
    auto callTool = [&](const QString& name, const QJsonObject& arguments) {
        inCall = true;
        const auto result = call(name, arguments);
        inCall = false;
        drain();  // events that arrived during the call
        return result;
    };
    QObject::connect(&socket, &QLocalSocket::readyRead, &app, [&] { if (!inCall) drain(); });
    // Stay connected, so the app can reach the agent between calls, and come back after the
    // app restarts.
    QTimer keep;
    keep.setInterval(2000);
    QObject::connect(&keep, &QTimer::timeout, &app, [&] {
        if (initialized && !inCall && socket.state() == QLocalSocket::UnconnectedState) socket.connectToServer(AgentTools::socketName());
    });
    keep.start();

    auto handle = [&](const std::string& line) {
        if (line.empty()) return;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
        if (!doc.isObject()) { fail(QJsonValue::Null, -32700, "Parse error"); return; }
        const QJsonObject message = doc.object();
        if (!message.contains("id") || !message.contains("method")) return;  // notifications and stray responses
        const QJsonValue id = message.value("id");
        const QString method = message.value("method").toString();
        const QJsonObject params = message.value("params").toObject();
        if (method == "initialize") {
            client = params.value("clientInfo").toObject().value("name").toString(client);
            const QString asked = params.value("protocolVersion").toString();
            initialized = true;
            connected();  // so the app can show that an agent is attached
            reply(id, {{"protocolVersion", kVersions.contains(asked) ? asked : kVersions.first()},
                       {"capabilities", QJsonObject{{"tools", QJsonObject{}}, {"prompts", QJsonObject{}},
                                                    {"experimental", QJsonObject{{"claude/channel", QJsonObject{}}}}}},
                       {"serverInfo", QJsonObject{{"name", "chorus-cut"}, {"title", "ChorusCut"}, {"version", "0.1"}}},
                       {"instructions", AgentTools::instructions()}});
        } else if (method == "ping") {
            reply(id, {});
        } else if (method == "tools/list") {
            reply(id, {{"tools", AgentTools::tools()}});
        } else if (method == "tools/call") {
            reply(id, callTool(params.value("name").toString(), params.value("arguments").toObject()));
        } else if (method == "prompts/list") {
            reply(id, {{"prompts", AgentTools::prompts()}});
        } else if (method == "prompts/get") {
            const auto messages = AgentTools::promptMessages(params.value("name").toString(), params.value("arguments").toObject());
            if (messages.isEmpty()) fail(id, -32602, "Unknown prompt");
            else reply(id, {{"description", "Plan the music video"}, {"messages", messages}});
        } else {
            fail(id, -32601, "Method not found: " + method);
        }
    };
    // stdin is read on its own thread, so the app's events get through while the agent is idle.
    std::thread([&app, handle] {
        std::string line;
        while (std::getline(std::cin, line))
            QMetaObject::invokeMethod(&app, [handle, line] { handle(line); }, Qt::QueuedConnection);
        QMetaObject::invokeMethod(&app, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
    }).detach();
    return app.exec();
}
