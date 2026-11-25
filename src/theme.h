#pragma once

#include <QColor>
#include <QDBusVariant>
#include <QFileSystemWatcher>
#include <QHash>
#include <QLocalSocket>
#include <QObject>
#include <QProcess>
#include <QTimer>

// The Omarchy palette, followed live. A file manager draws far more surface
// than a one-tool app, so it reads the whole colors.toml rather than only the
// accent, and the Material style flips with the theme's light or dark mode.
class Theme : public QObject {
    Q_OBJECT
    Q_PROPERTY(QColor accent READ accent NOTIFY changed)
    Q_PROPERTY(QColor accentForeground READ accentForeground NOTIFY changed)
    Q_PROPERTY(QColor background READ background NOTIFY changed)
    Q_PROPERTY(QColor darkBackground READ darkBackground NOTIFY changed)
    Q_PROPERTY(QColor lighterBackground READ lighterBackground NOTIFY changed)
    Q_PROPERTY(QColor foreground READ foreground NOTIFY changed)
    Q_PROPERTY(QColor brightForeground READ brightForeground NOTIFY changed)
    Q_PROPERTY(QColor secondaryForeground READ secondaryForeground NOTIFY changed)
    Q_PROPERTY(QColor red READ red NOTIFY changed)
    Q_PROPERTY(QColor yellow READ yellow NOTIFY changed)
    Q_PROPERTY(QColor blue READ blue NOTIFY changed)
    Q_PROPERTY(QColor magenta READ magenta NOTIFY changed)
    Q_PROPERTY(QColor cyan READ cyan NOTIFY changed)
    Q_PROPERTY(QColor orange READ orange NOTIFY changed)
    Q_PROPERTY(bool light READ light NOTIFY changed)
    Q_PROPERTY(int radius READ radius NOTIFY changed)
    // A tooltip's surface and its words, derived from the theme: a step away
    // from the pane and text that reads on it, in light and dark themes alike.
    Q_PROPERTY(QColor tooltipBackground READ tooltipBackground NOTIFY changed)
    Q_PROPERTY(QColor tooltipForeground READ tooltipForeground NOTIFY changed)
    // A folder card's fill: the lighter background, lifted off the pane.
    Q_PROPERTY(QColor folderFill READ folderFill NOTIFY changed)
    // The system's fixed-width font (Omarchy's pick of font), by its real
    // family, and how much bigger or smaller than Omarchy's default size the
    // system's text is: Folio's sizes are drawn for that default, and scale
    // with it. Followed live once followSystemFont() is called; until then,
    // the application's font at its own size.
    Q_PROPERTY(QString fontFamily READ fontFamily NOTIFY fontChanged)
    Q_PROPERTY(qreal textScale READ textScale NOTIFY fontChanged)

public:
    // `events` is Hyprland's event socket; with neither it nor `hyprctl`
    // given, the running Hyprland's.
    explicit Theme(const QString &currentDirectory = {}, QObject *parent = nullptr, const QString &hyprctl = {},
                   const QString &events = {});
    ~Theme() override;

    QColor accent() const { return color("accent"); }
    QColor accentForeground() const { return m_accentForeground; }
    QColor background() const { return color("background"); }
    QColor darkBackground() const { return color("dark_background"); }
    QColor lighterBackground() const { return m_lighterBackground; }
    QColor foreground() const { return color("foreground"); }
    QColor brightForeground() const { return color("bright_foreground"); }
    QColor secondaryForeground() const { return m_secondaryForeground; }
    QColor red() const { return color("red"); }
    QColor yellow() const { return color("yellow"); }
    QColor blue() const { return color("blue"); }
    QColor magenta() const { return color("magenta"); }
    QColor cyan() const { return color("cyan"); }
    QColor orange() const { return color("orange"); }
    bool light() const { return m_light; }
    int radius() const { return m_radius; }
    QColor tooltipBackground() const { return m_tooltipBackground; }
    QColor tooltipForeground() const { return m_tooltipForeground; }
    QColor folderFill() const { return m_folderFill; }
    QString fontFamily() const { return m_fontFamily; }
    qreal textScale() const { return m_textScale; }

    // Follows the system's fixed-width font from now on, as the application's
    // font and through fontFamily and textScale: when the platform theme says
    // it changed (qtengine's font or size), when fontconfig's own choice of it
    // does (omarchy-font-set rewrites ~/.config/fontconfig/fonts.conf), when
    // Omarchy's display text size changes (~/.config/omarchy/shell.toml), and
    // when the desktop portal's text-scaling-factor changes.
    void followSystemFont();
    // How much to scale text drawn for Omarchy's default size (11pt) to show
    // it at `points`, kept to what Folio's layout holds.
    static qreal textScaleFor(qreal points);
    // Text scale from an Omarchy shell.toml (`[font] base-size`, anchored at
    // 12px = 1.0), clamped to Folio's layout range, or 0 when unset.
    static qreal shellTextScale(const QString &shellConfig);
    // The family fontconfig gives for `family` (an alias like monospace) as
    // its configuration says now, or `family` when it gives none.
    static QString resolveFamily(const QString &family);

public slots:
    // Receives org.freedesktop.portal.Settings.SettingChanged for live
    // text-scaling-factor updates when shell.toml does not pin base-size.
    void portalSettingChanged(const QString &ns, const QString &key, const QDBusVariant &value);

signals:
    void changed();
    void fontChanged();

private:
    QColor color(const QString &key) const;
    void recomputeColors();
    void reload();
    void refreshRounding();
    void watchShellConfig();
    void readPortalTextScale();
    bool applyPortalTextScale(const QDBusVariant &value);
    qreal effectiveTextScale(qreal fixedPoints) const;
    void readSystemFont(bool reloadConfig = true);
    bool eventFilter(QObject *watched, QEvent *event) override;

    QString m_directory;
    bool m_liveDesktop = false;
    QHash<QString, QColor> m_colors;
    bool m_light = false;
    QColor m_lighterBackground;
    QColor m_accentForeground;
    QColor m_secondaryForeground;
    QColor m_tooltipBackground;
    QColor m_tooltipForeground;
    QColor m_folderFill;
    QFileSystemWatcher m_watcher;
    QTimer m_debounce;
    QString m_hyprctl;
    int m_radius = 0;
    QProcess m_roundingProcess;
    QTimer m_roundingTimeout;
    bool m_roundingAgain = false;
    QLocalSocket m_events;
    QString m_fontFamily;
    // The fixed font's family as last asked for, and what it resolved to.
    QString m_requestedFamily, m_resolvedFamily;
    qreal m_textScale = 1;
    qreal m_portalTextScale = 0;
    bool m_followingFont = false;
    bool m_reloadFontconfig = false;
    QFileSystemWatcher m_fontWatcher;
    QFileSystemWatcher m_shellWatcher;
    QTimer m_fontDebounce;
};
