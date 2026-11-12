#pragma once

#include <QObject>
#include <QStringList>

// File dialogs through xdg-desktop-portal over D-Bus, so open/save always
// match the desktop (Omacut's pattern). Results arrive on fileOpened /
// fileSaved signals; an empty path means the user cancelled or the portal
// failed.
class PortalPicker : public QObject {
    Q_OBJECT

public:
    explicit PortalPicker(QObject *parent = nullptr);

    Q_INVOKABLE void openFile(const QString &title, const QString &filterName, const QString &filterPattern);
    Q_INVOKABLE void saveFile(const QString &title, const QString &filterName, const QString &filterPattern,
                              const QString &suggestedName);

signals:
    void fileOpened(const QString &path);
    void fileSaved(const QString &path);

private slots:
    void onPortalResponse(uint response, const QVariantMap &results);

private:
    void request(const QString &method, const QString &title, const QString &filterName,
                 const QString &filterPattern, const QString &suggestedName);

    // Request object paths awaiting a response, mapped to save/open.
    QStringList m_openRequests;
    QStringList m_saveRequests;
};
