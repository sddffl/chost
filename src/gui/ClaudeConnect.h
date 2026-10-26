// AI > Connect Claude: puts chost-mcp into the settings of Claude Desktop (and shows the line for Claude
// Code), so that Claude can work in chost from a chat.

#pragma once

#include <QString>
#include <QStringList>

class QWidget;

namespace claude {

QString mcpProgram();             // chost-mcp next to chost, empty when it is missing
QStringList desktopConfigFiles(); // where Claude Desktop keeps its settings on this computer
// chost-mcp as the MCP server "chost" in that settings file, keeping everything else in it
bool addToConfig(const QString& file, const QString& program, QString& error);
bool isInConfig(const QString& file);
void showDialog(QWidget* parent);

} // namespace claude
