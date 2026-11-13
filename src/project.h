#pragma once

#include <QJsonObject>
#include <QString>

#include "dataformat.h"
#include "sequencemodel.h"

// Serializes a whole Lucidlight session — mode, both port configurations,
// every sequence, the script, and the display options — into a JSON document
// (.llproj files on disk, session.json for autosave).
class ChannelSettings;
class CommLog;
class CommManager;
class SequenceModel;

class Project {
public:
    static QJsonObject save(CommManager *comm, ChannelSettings *a, ChannelSettings *b,
                            SequenceModel *sequences, const QString &script, CommLog *log,
                            bool runOnOpen = false, const QString &scriptFile = QString());
    // Docklight's "run this script when the project opens" option.
    // Applies a stored state to the live objects. Returns an error string, or
    // an empty string on success.
    static QString load(const QJsonObject &state, CommManager *comm, ChannelSettings *a,
                        ChannelSettings *b, SequenceModel *sequences, QString *script, CommLog *log,
                        bool *runOnOpen = nullptr, QString *scriptFile = nullptr);
    static QString displayFormatName(DataFormat::Format format);
static QJsonObject sequenceToJson(const Sequence &seq);
    static Sequence sequenceFromJson(const QJsonObject &object);
    static DataFormat::Format displayFormatFromName(const QString &name);
};
