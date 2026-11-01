// What chost lets an AI assistant do: the tools of chost's MCP server (chost-mcp), with their arguments
// as JSON Schema. chost-mcp lists them to the assistant without chost running; chost itself carries
// them out (src/gui/Control.cpp), so both build this file.
//
// chost-mcp talks to the running chost over a local socket, one JSON object per line:
//   -> {"id": 1, "tool": "set_track", "args": {"track": 0, "volume_db": -6}}
//   <- {"id": 1, "ok": true, "result": {...}}      or      {"id": 1, "ok": false, "error": "..."}

#pragma once

#include <QJsonArray>
#include <QString>

namespace control {

QJsonArray tools();          // [{name, description, inputSchema}, ...]
QString serverName();        // the local socket chost listens on, one per user
QString instructions();      // what the assistant is told about chost when it connects
int protocolVersion();       // of the socket protocol, for "hello"

} // namespace control
