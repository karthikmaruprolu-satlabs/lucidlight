#include "commlog.h"

#include <QDateTime>

namespace {
QString clockString(qint64 ms) {
    const QTime time = QTime::fromMSecsSinceStartOfDay(int(ms % (24 * 3600 * 1000)));
    return time.toString("HH:mm:ss.zzz");
}
QString deltaString(qint64 deltaMs) {
    return QString::number(deltaMs / 1000.0, 'f', 3) + "s";
}
}  // namespace

CommLog::CommLog(QObject *parent) : QAbstractListModel(parent) {
    m_startMs = QDateTime::currentMSecsSinceEpoch();
}

int CommLog::rowCount(const QModelIndex &parent) const {
    if (parent.isValid())
        return 0;
    return m_channelFilter == 0 ? m_rows.size() : filteredCount();
}

int CommLog::filteredCount() const {
    int n = 0;
    for (const Row &row : m_rows)
        if (filterAccepts(row))
            ++n;
    return n;
}

bool CommLog::filterAccepts(const Row &row) const {
    switch (m_channelFilter) {
    case 1:
        return row.channel == 0;
    case 2:
        return row.channel == 1;
    case 3:
        return row.direction == Comment || row.direction == Event;
    default:
        return true;
    }
}

void CommLog::setChannelFilter(int filter) {
    const int clamped = qBound(0, filter, 3);
    if (clamped == m_channelFilter)
        return;
    m_channelFilter = clamped;
    beginResetModel();
    endResetModel();
    emit filterChanged();
}

int CommLog::findNext(const QString &text, int fromRow) const {
    if (text.isEmpty() || m_rows.isEmpty())
        return -1;
    const QString needle = text.trimmed();
    const int viewCount = filteredCount();
    if (viewCount == 0)
        return -1;
    auto contains = [&](int viewRow) {
        const int physical = physicalRow(viewRow);
        if (physical < 0)
            return false;
        const Row &row = m_rows.at(physical);
        return row.text.contains(needle, Qt::CaseInsensitive)
               || row.hex.contains(needle, Qt::CaseInsensitive);
    };
    const int start = fromRow + 1;
    for (int step = 0; step < viewCount; ++step) {
        const int viewRow = (start + step) % viewCount;
        if (contains(viewRow))
            return viewRow;
    }
    return -1;
}

int CommLog::physicalRow(int viewRow) const {
    if (viewRow < 0 || viewRow >= rowCount({}))
        return -1;
    if (m_channelFilter == 0)
        return viewRow;
    int n = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (filterAccepts(m_rows.at(i)) && ++n == viewRow)
            return i;
    }
    return -1;
}

QHash<int, QByteArray> CommLog::roleNames() const {
    return {
        {IndexRole, "entryIndex"},
        {ClockRole, "clock"},
        {DeltaRole, "delta"},
        {DirectionRole, "direction"},
        {ChannelRole, "channel"},
        {TextRole, "text"},
        {RawHexRole, "rawHex"},
        {HighlightRole, "highlight"},
        {CommentRole, "comment"},
        {SearchHitRole, "searchHit"},
    };
}

QVariant CommLog::data(const QModelIndex &index, int role) const {
    if (!index.isValid() || index.row() >= rowCount({}))
        return {};
    const int physical = physicalRow(index.row());
    if (physical < 0)
        return {};
    const Row &row = m_rows.at(physical);
    switch (role) {
    case IndexRole: return physical + m_trimmed + 1;
    case ClockRole: return clockString(row.stampMs);
    case DeltaRole: return deltaString(row.deltaMs);
    case DirectionRole: return row.direction;
    case ChannelRole: return row.channel;
    case TextRole: return row.text;
    case RawHexRole: return row.hex;
    case HighlightRole: return row.highlighted;
    case CommentRole: return row.direction == Comment || row.direction == Event;
    case SearchHitRole: return !m_search.isEmpty() && row.text.contains(m_search, Qt::CaseInsensitive);
    default: return {};
    }
}

void CommLog::setFrozen(bool frozen) {
    if (frozen == m_frozen)
        return;
    m_frozen = frozen;
    emit frozenChanged();
    // Unfreezing paints everything that arrived while the view was paused.
    if (!frozen)
        flushPending();
}

void CommLog::setShowTimestamps(bool on) {
    if (on == m_showTimestamps)
        return;
    m_showTimestamps = on;
    emit displayChanged();
}

void CommLog::setShowDelta(bool on) {
    if (on == m_showDelta)
        return;
    m_showDelta = on;
    emit displayChanged();
}

void CommLog::setDisplayFormat(DataFormat::Format format) {
    if (format == m_format)
        return;
    m_format = format;
    rerender();
}

void CommLog::setShowControlNames(bool on) {
    if (on == m_showControlNames)
        return;
    m_showControlNames = on;
    rerender();
}

void CommLog::setPauseStampMs(int ms) {
    if (ms == m_pauseStampMs)
        return;
    m_pauseStampMs = ms;
    emit displayChanged();
}

// Docklight's pause stamp: after a configurable silent gap the clock column
// restarts instead of counting on. Implemented by resetting the delta
// baseline, which also re-dates the next rows relative to the gap.
void CommLog::rerender() {
    if (m_rows.isEmpty())
        return;
    beginResetModel();
    for (Row &row : m_rows) {
        if (row.direction == Comment || row.direction == Event)
            continue;
        if (m_format == DataFormat::Format::Text && m_showControlNames)
            row.text = DataFormat::displayWithControlNames(row.data);
        else
            row.text = DataFormat::display(row.data, m_format);
    }
    endResetModel();
    emit displayChanged();
}

void CommLog::setSearchText(const QString &text) {
    if (text == m_search)
        return;
    m_search = text;
    if (!m_rows.isEmpty())
        emit dataChanged(index(0), index(m_rows.size() - 1), {SearchHitRole});
}

void CommLog::startClock() {
    m_startMs = QDateTime::currentMSecsSinceEpoch();
}

void CommLog::flushPending() {
    if (m_pending.isEmpty())
        return;
    const int first = m_rows.size();
    beginInsertRows({}, first, first + m_pending.size() - 1);
    for (Row &row : m_pending)
        m_rows.append(std::move(row));
    m_pending.clear();
    endInsertRows();
    emit countChanged();
    trimIfNeeded();
}

void CommLog::append(Direction direction, int channel, const QByteArray &data, bool highlighted) {
    Row row;
    row.direction = direction;
    row.channel = channel;
    row.data = data;
    row.stampMs = QDateTime::currentMSecsSinceEpoch();
    // Pause detection: after a silent gap longer than the threshold, restart
    // the relative clock so the break is visible in the delta column.
    if (m_pauseStampMs > 0 && !m_rows.isEmpty()) {
        const qint64 gap = row.stampMs - m_rows.last().stampMs;
        if (gap >= m_pauseStampMs) {
            row.deltaMs = gap;
            m_startMs = row.stampMs;
        } else {
            row.deltaMs = row.stampMs - m_startMs;
        }
    } else {
        row.deltaMs = row.stampMs - m_startMs;
    }
    if (direction == Comment || direction == Event)
        row.text = QString::fromUtf8(data);
    else if (m_format == DataFormat::Format::Text && m_showControlNames)
        row.text = DataFormat::displayWithControlNames(data);
    else
        row.text = DataFormat::display(data, m_format);
    row.hex = DataFormat::toHex(data);
    row.highlighted = highlighted;
    if (m_frozen) {
        // Keep the view stable while frozen; buffered arrivals repaint at once
        // when the view is released again.
        m_pending.append(std::move(row));
        if (m_pending.size() > 20000)
            m_pending.remove(0, 10000);
        return;
    }
    const int at = m_rows.size();
    beginInsertRows({}, at, at);
    m_rows.append(std::move(row));
    endInsertRows();
    emit countChanged();
    trimIfNeeded();
}

void CommLog::appendComment(const QString &text) {
    append(Comment, -1, text.toUtf8());
}

void CommLog::appendEvent(const QString &text) {
    append(Event, -1, text.toUtf8());
}

void CommLog::clear() {
    beginResetModel();
    m_rows.clear();
    m_pending.clear();
    m_trimmed = 0;
    m_nextIndex = 1;
    endResetModel();
    emit countChanged();
}

void CommLog::trimIfNeeded() {
    // Keep the memory bounded; a long capture would otherwise grow forever.
    static constexpr int limit = 5000;
    static constexpr int drop = 1000;
    if (m_rows.size() <= limit)
        return;
    beginRemoveRows({}, 0, drop - 1);
    m_rows.remove(0, drop);
    endRemoveRows();
    m_trimmed += drop;
    appendEvent(QObject::tr("--- %1 older entries trimmed ---").arg(drop));
}

QString CommLog::toText() const {
    QString text;
    for (const Row &row : m_rows) {
        text.append(clockString(row.stampMs));
        text.append(QLatin1Char('\t'));
        switch (row.direction) {
        case Rx: text.append(row.channel >= 0 ? QString("RX%1").arg(row.channel + 1) : "RX"); break;
        case Tx: text.append(row.channel >= 0 ? QString("TX%1").arg(row.channel + 1) : "TX"); break;
        case Comment: text.append("NOTE"); break;
        case Event: text.append("INFO"); break;
        }
        text.append(QLatin1Char('\t'));
        text.append(row.direction == Comment || row.direction == Event ? row.text : row.hex);
        text.append(QLatin1Char('\n'));
    }
    return text;
}
