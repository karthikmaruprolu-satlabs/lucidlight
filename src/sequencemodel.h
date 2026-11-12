#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantMap>
#include <QVector>

#include "dataformat.h"

// One entry of the project's sequence list: a named chunk of data to send, or
// a pattern to watch for on the incoming stream. Matches Docklight's
// send/receive sequences, including hotkeys, repeat, and receive triggers
// that fire a send sequence when their pattern is recognized.
struct Sequence {
    int id = 0;
    QString name;
    bool isSend = true;
    DataFormat::Format format = DataFormat::Format::Text;
    QString data;                 // as typed, in the sequence's notation
    int hotkey = 0;               // 1..12 for F1..F12, 0 = none
    int repeatIntervalMs = 0;     // > 0 keeps re-sending while active
    int replyToId = -1;           // receive sequences: send this id when matched
    bool enabled = true;
};

class SequenceModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)

public:
    enum Roles {
        IdRole = Qt::UserRole + 1,
        NameRole,
        IsSendRole,
        FormatRole,
        DataRole,
        PreviewRole,      // rendered bytes, for the list
        HotkeyRole,
        RepeatRole,
        ReplyToRole,
        EnabledRole,
    };

    explicit SequenceModel(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QHash<int, QByteArray> roleNames() const override;
    QVariant data(const QModelIndex &index, int role) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role) override;

    int count() const { return m_sequences.size(); }
    const QVector<Sequence> &sequences() const { return m_sequences; }

    Q_INVOKABLE QVariantMap get(int row) const;
    Q_INVOKABLE int add(const QVariantMap &fields);
    Q_INVOKABLE void update(int row, const QVariantMap &fields);
    Q_INVOKABLE void remove(int row);
    Q_INVOKABLE void move(int from, int to);
    Q_INVOKABLE void send(int row);   // routed to the backend by a connection

    // Lookups used by the matcher and the scripting DL object.
    int findSendByHotkey(int hotkey) const;
    int findByName(const QString &name) const;
    const Sequence *sequenceAt(int row) const { return row >= 0 && row < m_sequences.size() ? &m_sequences.at(row) : nullptr; }

    void replaceAll(const QVector<Sequence> &sequences);

signals:
    void countChanged();
    void sendRequested(int row);
    void sequenceChanged();

private:
    static void applyFields(Sequence &seq, const QVariantMap &fields);

    QVector<Sequence> m_sequences;
    int m_nextId = 1;
};
