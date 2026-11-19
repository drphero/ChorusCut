// choruscut-mcp: the MCP server an agent (Claude Code, Codex or any MCP client) launches over
// stdio. It answers the protocol itself and passes each tool call to the running Chorus Cut over
// a per-user local socket, so every change goes through the app's own commands and undo.
// Nothing but protocol messages goes to stdout.
#include "agent_tools.h"
#include <QCoreApplication>
#include <QJsonDocument>
#include <QLocalSocket>
#include <iostream>
#include <string>

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
    auto connected = [&] {
        // Notice an app that quit or restarted since the last call (its close is still queued),
        // and connect afresh, so the first call after a restart works. A call that was already
        // sent is never sent again, so a pass cannot be applied twice.
        if (socket.state() == QLocalSocket::ConnectedState) socket.waitForDisconnected(0);
        if (socket.state() == QLocalSocket::ConnectedState) return true;
        socket.abort();
        socket.connectToServer(AgentTools::socketName());
        if (!socket.waitForConnected(1500)) return false;
        socket.write(QJsonDocument(QJsonObject{{"hello", client}}).toJson(QJsonDocument::Compact) + '\n');
        socket.waitForBytesWritten(1000);
        return true;
    };
    // Sends one tool call to the app and waits for its answer.
    auto call = [&](const QString& name, const QJsonObject& arguments) -> QJsonObject {
        if (!connected())
            return toolError("Chorus Cut is not running. Ask the user to open Chorus Cut and their project, then try again.");
        const qint64 id = ++serial;
        socket.write(QJsonDocument(QJsonObject{{"id", id}, {"tool", name}, {"arguments", arguments}}).toJson(QJsonDocument::Compact) + '\n');
        socket.waitForBytesWritten(5000);
        while (true) {
            while (!socket.canReadLine()) {
                if (socket.state() != QLocalSocket::ConnectedState || !socket.waitForReadyRead(180000)) {
                    socket.abort();
                    return toolError("Chorus Cut closed or stopped answering. Ask the user to check the app, then try again.");
                }
            }
            const auto answer = QJsonDocument::fromJson(socket.readLine()).object();
            if (answer.value("id").toInteger() == id) return answer.value("result").toObject();
        }
    };

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(QByteArray::fromStdString(line), &error);
        if (!doc.isObject()) { fail(QJsonValue::Null, -32700, "Parse error"); continue; }
        const QJsonObject message = doc.object();
        if (!message.contains("id") || !message.contains("method")) continue;  // notifications and stray responses
        const QJsonValue id = message.value("id");
        const QString method = message.value("method").toString();
        const QJsonObject params = message.value("params").toObject();
        if (method == "initialize") {
            client = params.value("clientInfo").toObject().value("name").toString(client);
            const QString asked = params.value("protocolVersion").toString();
            connected();  // so the app can show that an agent is attached
            reply(id, {{"protocolVersion", kVersions.contains(asked) ? asked : kVersions.first()},
                       {"capabilities", QJsonObject{{"tools", QJsonObject{}}, {"prompts", QJsonObject{}}}},
                       {"serverInfo", QJsonObject{{"name", "chorus-cut"}, {"title", "Chorus Cut"}, {"version", "0.1"}}},
                       {"instructions", AgentTools::instructions()}});
        } else if (method == "ping") {
            reply(id, {});
        } else if (method == "tools/list") {
            reply(id, {{"tools", AgentTools::tools()}});
        } else if (method == "tools/call") {
            reply(id, call(params.value("name").toString(), params.value("arguments").toObject()));
        } else if (method == "prompts/list") {
            reply(id, {{"prompts", AgentTools::prompts()}});
        } else if (method == "prompts/get") {
            const auto messages = AgentTools::promptMessages(params.value("name").toString(), params.value("arguments").toObject());
            if (messages.isEmpty()) fail(id, -32602, "Unknown prompt");
            else reply(id, {{"description", "Plan the music video"}, {"messages", messages}});
        } else {
            fail(id, -32601, "Method not found: " + method);
        }
    }
    return 0;
}
