#pragma once

#include <QColor>
#include <QFileSystemWatcher>
#include <QHash>
#include <QObject>
#include <QProcess>
#include <QTimer>

// Follows the Omarchy theme live: every color in
// ~/.local/state/omarchy/current/theme/colors.toml becomes a QML property
// (theme.bg, theme.accent, theme.red, ...), so the app always sits inside the
// desktop. Falls back to a Catppuccin-like dark palette when Omarchy isn't
// there. Also tracks Hyprland's window rounding as `radius`.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString accent READ accent NOTIFY changed)
    Q_PROPERTY(QString accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    Q_PROPERTY(QString bg READ bg NOTIFY changed)
    Q_PROPERTY(QString surface READ surface NOTIFY changed)
    Q_PROPERTY(QString surfaceAlt READ surfaceAlt NOTIFY changed)
    Q_PROPERTY(QString deep READ deep NOTIFY changed)
    Q_PROPERTY(QString fg READ fg NOTIFY changed)
    Q_PROPERTY(QString bright READ bright NOTIFY changed)
    Q_PROPERTY(QString muted READ muted NOTIFY changed)
    Q_PROPERTY(QString selection READ selection NOTIFY changed)
    Q_PROPERTY(QString red READ red NOTIFY changed)
    Q_PROPERTY(QString green READ green NOTIFY changed)
    Q_PROPERTY(QString yellow READ yellow NOTIFY changed)
    Q_PROPERTY(QString orange READ orange NOTIFY changed)
    Q_PROPERTY(QString blue READ blue NOTIFY changed)
    Q_PROPERTY(QString cyan READ cyan NOTIFY changed)
    Q_PROPERTY(QString magenta READ magenta NOTIFY changed)

public:
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {});
    ~Theme() override;

    QString accent() const { return m_accent; }
    QString accentForeground() const;
    int radius() const { return m_radius; }

    QString bg() const { return m_colors.value("background", "#1e1e2e"); }
    QString surface() const { return m_colors.value("lighter_background", "#313244"); }
    QString surfaceAlt() const { return m_colors.value("dark_background", "#161622"); }
    QString deep() const { return m_colors.value("darker_background", "#101019"); }
    QString fg() const { return m_colors.value("foreground", "#cdd6f4"); }
    QString bright() const { return m_colors.value("bright_foreground", "#cdd6f4"); }
    QString muted() const { return m_colors.value("dark_foreground", "#6c7086"); }
    QString selection() const { return m_colors.value("selection", "#45475a"); }
    QString red() const { return m_colors.value("red", "#f38ba8"); }
    QString green() const { return m_colors.value("green", "#a6e3a1"); }
    QString yellow() const { return m_colors.value("yellow", "#f9e2af"); }
    QString orange() const { return m_colors.value("orange", "#f6b6ab"); }
    QString blue() const { return m_colors.value("blue", "#89b4fa"); }
    QString cyan() const { return m_colors.value("cyan", "#94e2d5"); }
    QString magenta() const { return m_colors.value("magenta", "#f5c2e7"); }

    Q_INVOKABLE QColor color(const QString &name) const { return QColor(m_colors.value(name, "#cdd6f4")); }

    static QString readAccent(const QString &path);

signals:
    void changed();

private:
    void reload();
    void refreshRounding();

    QString m_directory;
    QString m_accent = "#FFD60A";
    QHash<QString, QString> m_colors;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingPoll, m_roundingTimeout;
};
