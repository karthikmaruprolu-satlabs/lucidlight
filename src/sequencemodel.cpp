#include "sequencemodel.h"

SequenceModel::SequenceModel(QObject *parent) : QAbstractListModel(parent) {}

int SequenceModel::rowCount(const QModelIndex &parent) const {
    return parent.isValid() ? 0 : m_sequences.size();
}

QHash<int, QByteArray> SequenceModel::roleNames() const {
    return {
        {IdRole, "seqId"},
        {NameRole, "name"},
        {IsSendRole, "isSend"},
        {FormatRole, "format"},
        {DataRole, "data"},
        {PreviewRole, "preview"},
        {HotkeyRole, "hotkey"},
        {RepeatRole, "repeatIntervalMs"},
        {ReplyToRole, "replyToId"},
        {EnabledRole, "seqEnabled"},
    };
}

QVariant SequenceModel::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= m_sequences.size())
        return {};
    const Sequence &seq = m_sequences.at(index.row());
    switch (role) {
    case IdRole: return seq.id;
    case NameRole: return seq.name;
    case IsSendRole: return seq.isSend;
    case FormatRole: return int(seq.format);
    case DataRole: return seq.data;
    case PreviewRole: {
        // A short rendered sample so the list itself says what goes out.
        const auto parsed = DataFormat::parse(seq.data, seq.format);
        const QString shown = parsed.ok() ? DataFormat::display(parsed.bytes, DataFormat::Format::Text) : seq.data;
        return shown.length() > 48 ? shown.left(48) + "…" : shown;
    }
    case HotkeyRole: return seq.hotkey;
    case RepeatRole: return seq.repeatIntervalMs;
    case ReplyToRole: return seq.replyToId;
    case EnabledRole: return seq.enabled;
    default: return {};
    }
}

bool SequenceModel::setData(const QModelIndex &index, const QVariant &value, int role) {
    if (!index.isValid() || index.row() >= m_sequences.size())
        return false;
    Sequence &seq = m_sequences[index.row()];
    switch (role) {
    case EnabledRole:
        seq.enabled = value.toBool();
        break;
    default:
        return false;
    }
    emit dataChanged(index, index, {role});
    emit sequenceChanged();
    return true;
}

QVariantMap SequenceModel::get(int row) const {
    QVariantMap map;
    if (row < 0 || row >= m_sequences.size())
        return map;
    const Sequence &seq = m_sequences.at(row);
    map["row"] = row;
    map["seqId"] = seq.id;
    map["name"] = seq.name;
    map["isSend"] = seq.isSend;
    map["format"] = int(seq.format);
    map["data"] = seq.data;
    map["hotkey"] = seq.hotkey;
    map["repeatIntervalMs"] = seq.repeatIntervalMs;
    map["replyToId"] = seq.replyToId;
    map["enabled"] = seq.enabled;
    return map;
}

int SequenceModel::add(const QVariantMap &fields) {
    Sequence seq;
    seq.id = m_nextId++;
    applyFields(seq, fields);
    const int row = m_sequences.size();
    beginInsertRows({}, row, row);
    m_sequences.append(seq);
    endInsertRows();
    emit countChanged();
    emit sequenceChanged();
    return row;
}

void SequenceModel::applyFields(Sequence &seq, const QVariantMap &fields) {
    if (fields.contains("name")) seq.name = fields.value("name").toString();
    if (fields.contains("isSend")) seq.isSend = fields.value("isSend").toBool();
    if (fields.contains("format")) seq.format = DataFormat::Format(fields.value("format").toInt());
    if (fields.contains("data")) seq.data = fields.value("data").toString();
    if (fields.contains("hotkey")) seq.hotkey = fields.value("hotkey").toInt();
    if (fields.contains("repeatIntervalMs")) seq.repeatIntervalMs = fields.value("repeatIntervalMs").toInt();
    if (fields.contains("replyToId")) seq.replyToId = fields.value("replyToId").toInt();
    if (fields.contains("enabled")) seq.enabled = fields.value("enabled").toBool();
}

void SequenceModel::update(int row, const QVariantMap &fields) {
    if (row < 0 || row >= m_sequences.size())
        return;
    Sequence &seq = m_sequences[row];
    applyFields(seq, fields);
    emit dataChanged(index(row), index(row));
    emit sequenceChanged();
}

void SequenceModel::remove(int row) {
    if (row < 0 || row >= m_sequences.size())
        return;
    beginRemoveRows({}, row, row);
    m_sequences.remove(row);
    endRemoveRows();
    emit countChanged();
    emit sequenceChanged();
}

void SequenceModel::move(int from, int to) {
    if (from == to || from < 0 || from >= m_sequences.size())
        return;
    const int destination = qBound(0, to, m_sequences.size() - 1);
    if (!beginMoveRows({}, from, from, {}, destination > from ? destination + 1 : destination))
        return;
    m_sequences.move(from, destination);
    endMoveRows();
    emit sequenceChanged();
}

void SequenceModel::send(int row) {
    emit sendRequested(row);
}

int SequenceModel::findSendByHotkey(int hotkey) const {
    for (int i = 0; i < m_sequences.size(); ++i)
        if (m_sequences.at(i).isSend && m_sequences.at(i).hotkey == hotkey)
            return i;
    return -1;
}

int SequenceModel::findByName(const QString &name) const {
    for (int i = 0; i < m_sequences.size(); ++i)
        if (m_sequences.at(i).name.compare(name, Qt::CaseInsensitive) == 0)
            return i;
    return -1;
}

void SequenceModel::replaceAll(const QVector<Sequence> &sequences) {
    beginResetModel();
    m_sequences = sequences;
    for (const Sequence &seq : m_sequences)
        m_nextId = qMax(m_nextId, seq.id + 1);
    endResetModel();
    emit countChanged();
    emit sequenceChanged();
}
