#pragma once
#include <QJsonArray>
#include <QJsonObject>
#include <QString>

// What an agent sees of Chorus Cut over MCP: the tool list, the server instructions and the
// planning prompt. Shared by the stdio program agents launch (choruscut-mcp) and the app,
// which carries out the tool calls on its own command layer.
namespace AgentTools {
// The local socket between choruscut-mcp and the running app. Per user; tests override it
// with CHORUS_AGENT_SOCKET so they never meet a real app.
QString socketName();
QJsonArray tools();
QString instructions();
QJsonArray prompts();
// The planning workflow, served by the get_guide tool and the "plan" prompt.
QString guide();
// Messages for prompts/get, or an empty array for an unknown prompt.
QJsonArray promptMessages(const QString& name, const QJsonObject& arguments);
}
