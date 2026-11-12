#include "portalpicker.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDateTime>
#include <QUrl>>

namespace {
QVariant filtersVariant(const QString &filterName, const QString &filterPattern) {
    // The portal API wants filters as (label, glob-list) pairs.
    QVariantList filters;
    filters.append(QVariantList{filterName, QStringList{filterPattern}});
    return filters;
}
}  // namespace

PortalPicker::PortalPicker(QObject *parent) : QObject(parent) {}

void PortalPicker::openFile(const QString &title, const QString &filterName, const QString &filterPattern) {
    request(QStringLiteral("OpenFile"), title, filterName, filterPattern, QString());
}

void PortalPicker::saveFile(const QString &title, const QString &filterName, const QString &filterPattern,
                            const QString &suggestedName) {
    request(QStringLiteral("SaveFile"), title, filterName, filterPattern, suggestedName);
}

void PortalPicker::request(const QString &method, const QString &title, const QString &filterName,
                           const QString &filterPattern, const QString &suggestedName) {
    QVariantMap options;
    options.insert(QStringLiteral("handle_token"),
                   QString::number(quintptr(this)) + QStringLiteral("-")
                       + QString::number(QDateTime::currentMSecsSinceEpoch()));
    options.insert(QStringLiteral("filters"), filtersVariant(filterName, filterPattern));
    options.insert(QStringLiteral("modal"), true);
    if (!suggestedName.isEmpty())
        options.insert(QStringLiteral("current_name"), suggestedName);

    QDBusMessage message = QDBusMessage::createMethodCall(
        QStringLiteral("org.freedesktop.portal.Desktop"), QStringLiteral("/org/freedesktop/portal/desktop"),
        QStringLiteral("org.freedesktop.portal.FileChooser"), method);
    message << QStringLiteral("");  // parent window: the portal tracks focus itself
    message << title;
    message << options;

    QDBusPendingCall call = QDBusConnection::sessionBus().asyncCall(message);
    auto *watcher = new QDBusPendingCallWatcher(call, this);
    const bool forSaving = method == QLatin1String("SaveFile");
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, forSaving](QDBusPendingCallWatcher *self) {
                QDBusPendingReply<QDBusObjectPath> reply = *self;
                self->deleteLater();
                if (reply.isError()) {
                    // No portal (headless session): cancel the dialog politely.
                    if (forSaving)
                        emit fileSaved(QString());
                    else
                        emit fileOpened(QString());
                    return;
                }
                const QString requestPath = reply.value().path();
                (forSaving ? m_saveRequests : m_openRequests).append(requestPath);
                QDBusConnection::sessionBus().connect(
                    QStringLiteral("org.freedesktop.portal.Desktop"), requestPath,
                    QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), this,
                    SLOT(onPortalResponse(uint, QVariantMap)));
            });
}

void PortalPicker::onPortalResponse(uint response, const QVariantMap &results) {
    QDBusConnection::sessionBus().disconnect(
        QStringLiteral("org.freedesktop.portal.Desktop"), QString(),
        QStringLiteral("org.freedesktop.portal.Request"), QStringLiteral("Response"), this,
        SLOT(onPortalResponse(uint, QVariantMap)));

    // Which kind of dialog answered? The sender path identifies it, but the
    // signal carries no path, so check the pending lists by membership.
    // Both lists are almost always empty outside an active dialog.
    const bool saving = !m_saveRequests.isEmpty();
    m_saveRequests.clear();
    m_openRequests.clear();

    QString path;
    if (response == 0) {
        const QStringList uris = results.value(QStringLiteral("uris")).toStringList();
        if (!uris.isEmpty())
            path = QUrl(uris.first()).toLocalFile();
    }
    if (saving)
        emit fileSaved(path);
    else
        emit fileOpened(path);
}
