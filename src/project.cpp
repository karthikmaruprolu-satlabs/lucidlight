#include "project.h"

#include <QJsonArray>

#include "channelsettings.h"
#include "commlog.h"
#include "commmanager.h"
#include "sequencemodel.h"

QJsonObject Project::save(CommManager *comm, ChannelSettings *a, ChannelSettings *b,
                          SequenceModel *sequences, const QString &script, CommLog *log) {
    QJsonObject state;
    state["version"] = 1;
    state["mode"] = comm->mode();

    auto channelJson = [](const ChannelSettings *settings) {
        QJsonObject object;
        object["device"] = settings->device();
        object["baud"] = settings->baud();
        object["dataBits"] = settings->dataBits();
        object["parity"] = settings->parity();
        object["stopBits"] = settings->stopBits();
        object["flow"] = settings->flow();
        return object;
    };
    state["channelA"] = channelJson(a);
    state["channelB"] = channelJson(b);

    QJsonArray sequenceArray;
    for (const Sequence &seq : sequences->sequences()) {
        QJsonObject object;
        object["id"] = seq.id;
        object["name"] = seq.name;
        object["isSend"] = seq.isSend;
        object["format"] = Project::displayFormatName(seq.format);
        object["data"] = seq.data;
        object["hotkey"] = seq.hotkey;
        object["repeatIntervalMs"] = seq.repeatIntervalMs;
        object["replyToId"] = seq.replyToId;
        object["enabled"] = seq.enabled;
        sequenceArray.append(object);
    }
    state["sequences"] = sequenceArray;
    state["script"] = script;

    QJsonObject display;
    display["format"] = Project::displayFormatName(log->displayFormat());
    display["timestamps"] = log->showTimestamps();
    display["delta"] = log->showDelta();
    state["display"] = display;
    return state;
}

QString Project::load(const QJsonObject &state, CommManager *comm, ChannelSettings *a,
                      ChannelSettings *b, SequenceModel *sequences, QString *script, CommLog *log) {
    if (state.value("version").toInt() != 1)
        return QObject::tr("Not a Lucidlight project file");

    comm->setMode(state.value("mode").toInt(0));

    auto applyChannel = [](ChannelSettings *settings, const QJsonValue &value) {
        const QJsonObject object = value.toObject();
        settings->setDevice(object.value("device").toString());
        settings->setBaud(object.value("baud").toInt(115200));
        settings->setDataBits(object.value("dataBits").toInt(8));
        settings->setParity(object.value("parity").toInt(0));
        settings->setStopBits(object.value("stopBits").toInt(1));
        settings->setFlow(object.value("flow").toInt(0));
    };
    applyChannel(a, state.value("channelA"));
    applyChannel(b, state.value("channelB"));

    QVector<Sequence> loaded;
    const QJsonArray sequenceArray = state.value("sequences").toArray();
    for (const QJsonValue &value : sequenceArray) {
        const QJsonObject object = value.toObject();
        Sequence seq;
        seq.id = object.value("id").toInt(loaded.size() + 1);
        seq.name = object.value("name").toString();
        seq.isSend = object.value("isSend").toBool(true);
        seq.format = Project::displayFormatFromName(object.value("format").toString());
        seq.data = object.value("data").toString();
        seq.hotkey = object.value("hotkey").toInt(0);
        seq.repeatIntervalMs = object.value("repeatIntervalMs").toInt(0);
        seq.replyToId = object.value("replyToId").toInt(-1);
        seq.enabled = object.value("enabled").toBool(true);
        loaded.append(seq);
    }
    sequences->replaceAll(loaded);

    if (script)
        *script = state.value("script").toString();

    const QJsonObject display = state.value("display").toObject();
    log->setDisplayFormat(Project::displayFormatFromName(display.value("format").toString()));
    log->setShowTimestamps(display.value("timestamps").toBool(true));
    log->setShowDelta(display.value("delta").toBool(false));
    return {};
}

QString Project::displayFormatName(DataFormat::Format format) {
    switch (format) {
    case DataFormat::Format::Hex: return "hex";
    case DataFormat::Format::Dec: return "dec";
    default: return "text";
    }
}

DataFormat::Format Project::displayFormatFromName(const QString &name) {
    if (name == QLatin1String("hex"))
        return DataFormat::Format::Hex;
    if (name == QLatin1String("dec"))
        return DataFormat::Format::Dec;
    return DataFormat::Format::Text;
}
