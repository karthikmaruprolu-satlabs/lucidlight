#pragma once

#include <QAbstractListModel>
#include <QByteArray>
#include <QElapsedTimer>
#include <QString>
#include <QVector>

#include "dataformat.h"

// The communication window, one row per data burst or comment, rendered by a
// QML ListView. Rows are immutable once appended, so trimming and clearing
// are the only structural changes.
class CommLog : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(bool frozen READ frozen WRITE setFrozen NOTIFY frozenChanged)
    Q_PROPERTY(bool showTimestamps READ showTimestamps WRITE setShowTimestamps NOTIFY displayChanged)
    Q_PROPERTY(bool showDelta READ showDelta WRITE setShowDelta NOTIFY displayChanged)
    Q_PROPERTY(DataFormat::Format displayFormat READ displayFormat WRITE setDisplayFormat NOTIFY displayChanged)

public:
    enum Direction { Rx, Tx, Comment, Event };
    Q_ENUM(Direction)

    enum Roles {
        IndexRole = Qt::UserRole + 1,
        ClockRole,      // absolute HH:MM:SS.mmm
        DeltaRole,      // seconds since the previous row
        DirectionRole,
        ChannelRole,    // 0 = port A, 1 = port B, -1 = n/a
        TextRole,       // rendered in the current display format
        RawHexRole,     // always-available hex for the context menu / search
        HighlightRole,  // a receive sequence matched inside this row
        CommentRole,
        SearchHitRole,
    };

    explicit CommLog(QObject *parent = nullptr);

    int rowCount(const QModelIndex &parent = {}) const override;
    QHash<int, QByteArray> roleNames() const override;
    QVariant data(const QModelIndex &index, int role) const override;

    int count() const { return m_rows.size(); }
    bool frozen() const { return m_frozen; }
    bool showTimestamps() const { return m_showTimestamps; }
    bool showDelta() const { return m_showDelta; }
    DataFormat::Format displayFormat() const { return m_format; }

    void setFrozen(bool frozen);
    void setShowTimestamps(bool on);
    void setShowDelta(bool on);
    void setDisplayFormat(DataFormat::Format format);
    Q_INVOKABLE void setSearchText(const QString &text);

    void startClock();
    void append(Direction direction, int channel, const QByteArray &data, bool highlighted = false);
    void appendComment(const QString &text);
    void appendEvent(const QString &text);
    void clear();

    // Convenience for QML context menus.
    Q_INVOKABLE QString textAt(int row) const { return row >= 0 && row < m_rows.size() ? m_rows.at(row).text : QString(); }
    Q_INVOKABLE QString hexAt(int row) const { return row >= 0 && row < m_rows.size() ? m_rows.at(row).hex : QString(); }

    // Everything shown, for saving the window to a log file.
    Q_INVOKABLE QString toText() const;

signals:
    void countChanged();
    void frozenChanged();
    void displayChanged();

private:
    void flushPending();
    void trimIfNeeded();

    struct Row {
        Direction direction = Rx;
        int channel = -1;
        qint64 stampMs = 0;
        qint64 deltaMs = 0;
        QByteArray data;
        QString text;
        QString hex;
        bool highlighted = false;
    };

    QVector<Row> m_rows;
    qint64 m_startMs = 0;
    bool m_frozen = false;
    bool m_showTimestamps = true;
    bool m_showDelta = false;
    DataFormat::Format m_format = DataFormat::Format::Text;
    QString m_search;
    QVector<Row> m_pending;
    int m_trimmed = 0;
    int m_nextIndex = 1;
};
