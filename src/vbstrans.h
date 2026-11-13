// VBScript → JavaScript transpiler for Docklight Scripting compatibility.
//
// Docklight scripts are VBScript. Lucidlight runs JavaScript (QJSEngine), so
// sample scripts from the Docklight manual (Dim, If...Then, For...Next,
// While...Wend, Do...Loop, Select Case, Sub/Function, MsgBox, call-without-
// parentheses and "&" concatenation) must be translated before execution.
// This module converts the common VBScript subset used in real Docklight
// scripts; constructs it cannot translate are reported as an error comment
// in the emitted JavaScript so failures are visible instead of silent.
#ifndef VBSTRANS_H
#define VBSTRANS_H

#include <QString>
#include <QStringList>

namespace VbsTrans {

// True when the source looks like VBScript rather than JavaScript.
bool looksLikeVbScript(const QString &source);

// Translate VBScript source to JavaScript. Returns the JS code; a leading
// "/*vbs*/" marker records that the translation happened.
QString toJs(const QString &source, QStringList *warnings = nullptr);

}  // namespace VbsTrans

#endif  // VBSTRANS_H
