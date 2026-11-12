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
    // Docklight contents filter: 0 all, 1 channel 1, 2 channel 2, 3 comments only.
    Q_PROPERTY(int channelFilter READ channelFilter WRITE setChannelFilter NOTIFY filterChanged)
    // Docklight's "control characters" view option for the ASCII view.
    Q_PROPERTY(bool showControlNames READ showControlNames WRITE setShowControlNames NOTIFY displayChanged)
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
    bool showControlNames() const { return m_showControlNames; }
    void setShowControlNames(bool on);
    int channelFilter() const { return m_channelFilter; }
    void setChannelFilter(int filter);
    void setDisplayFormat(DataFormat::Format format);
    Q_INVOKABLE void setSearchText(const QString &text);

    void startClock();
    void append(Direction direction, int channel, const QByteArray &data, bool highlighted = false);
    void appendComment(const QString &text);
    void appendEvent(const QString &text);
    void clear();

    // Convenience for QML context menus.
    Q_INVOKABLE QString textAt(int row) const {
        const int physical = physicalRow(row);
        return physical >= 0 ? m_rows.at(physical).text : QString();
    }
    Q_INVOKABLE QString hexAt(int row) const {
        const int physical = physicalRow(row);
        return physical >= 0 ? m_rows.at(physical).hex : QString();
    }

    // Everything shown, for saving the window to a log file.
    Q_INVOKABLE QString toText() const;
    // Physical row index behind a filtered view row (-1 when filtered out).
    Q_INVOKABLE int physicalRow(int viewRow) const;

    // Read access for the script bridge (GetCommWindowData) and the file
    // logger.
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
    const QVector<Row> &rows() const { return m_rows; }
    bool filterAccepts(const Row &row) const;
    int filteredCount() const;

signals:
    void countChanged();
    void frozenChanged();
    void displayChanged();
    void filterChanged();

private:
    void flushPending();
    void trimIfNeeded();

    QVector<Row> m_rows;
    qint64 m_startMs = 0;
    bool m_frozen = false;
    int m_channelFilter = 0;
    bool m_showControlNames = false;
    // Recomputes every row's text from the raw data (after a view switch).
    void rerender();
    bool m_showTimestamps = true;
    bool m_showDelta = false;
    DataFormat::Format m_format = DataFormat::Format::Text;
    QString m_search;
    QVector<Row> m_pending;
    int m_trimmed = 0;
    int m_nextIndex = 1;
};
