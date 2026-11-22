// Theme watching follows Omacut (MIT, David Heinemeier Hansson).
#include "theme.h"

#include <QDir>
#include <QEvent>
#include <QFontDatabase>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <algorithm>
#include <cmath>
#include <utility>
#include <fontconfig/fontconfig.h>

namespace {

// Used when Omarchy isn't installed: a neutral dark surface with Omarchy's
// fallback yellow accent, so the app still reads well on its own.
const QHash<QString, QColor> &fallbackColors() {
    static const QHash<QString, QColor> colors{
        {"accent", QColor("#FFD60A")},
        {"background", QColor("#16161a")},
        {"dark_background", QColor("#111114")},
        {"lighter_background", QColor("#232329")},
        {"foreground", QColor("#c8c8d0")},
        {"bright_foreground", QColor("#ececf1")},
        {"red", QColor("#f7768e")},
        {"yellow", QColor("#e0af68")},
        {"blue", QColor("#7aa2f7")},
        {"magenta", QColor("#bb9af7")},
        {"cyan", QColor("#7dcfff")},
        {"orange", QColor("#ff9e64")},
    };
    return colors;
}

double luminance(const QColor &c) {
    auto linear = [](double v) { return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); };
    return .2126 * linear(c.redF()) + .7152 * linear(c.greenF()) + .0722 * linear(c.blueF());
}

QColor mix(const QColor &a, const QColor &b, double t) {
    return QColor::fromRgbF(a.redF() * (1 - t) + b.redF() * t, a.greenF() * (1 - t) + b.greenF() * t,
                            a.blueF() * (1 - t) + b.blueF() * t);
}

double ratio(const QColor &a, const QColor &b) {
    const double x = luminance(a), y = luminance(b);
    return (std::max(x, y) + .05) / (std::min(x, y) + .05);
}

// WCAG's minimum contrast for text. A surface needs much less.
constexpr double textFloor = 4.5;
constexpr double surfaceFloor = 1.4;
// The ground luminance where black and white read equally.
constexpr double midLuminance = .179;
// Bisection steps in ensureRatio, finer than one eight-bit level.
constexpr int bisectionSteps = 18;
// One eight-bit channel's range, so the rounding repair can never step further.
constexpr int channelRange = 255;

// The ink when it already reads at need, else the least mix toward black or
// white that still does once rounded to eight-bit channels.
QColor ensureRatio(const QColor &ink, const QColor &ground, double need) {
    if (ratio(ink, ground) >= need)
        return ink;
    const QColor toward = luminance(ground) > midLuminance ? QColor(Qt::black) : QColor(Qt::white);
    double low = 0, high = 1;
    QColor best = toward;
    for (int i = 0; i < bisectionSteps; ++i) {
        const double middle = (low + high) / 2;
        const QColor candidate = mix(ink, toward, middle);
        if (ratio(candidate, ground) >= need) {
            best = candidate;
            high = middle;
        } else {
            low = middle;
        }
    }
    best = QColor(best.red(), best.green(), best.blue());
    const int step = toward == QColor(Qt::white) ? 1 : -1;
    for (int i = 0; i < channelRange && ratio(best, ground) < need; ++i) {
        const QColor next(qBound(0, best.red() + step, channelRange), qBound(0, best.green() + step, channelRange),
                          qBound(0, best.blue() + step, channelRange));
        if (next == best)
            break;
        best = next;
    }
    return best;
}

struct Palette {
    QHash<QString, QColor> colors;
    bool light = false;
};

Palette readPalette(const QString &path) {
    Palette palette;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return palette;
    const QString text = QString::fromUtf8(file.readAll());
    static const QRegularExpression colorLine(R"re(^\s*([a-z_]+)\s*=\s*["'](#[0-9a-fA-F]{6})["'])re",
                                              QRegularExpression::MultilineOption);
    for (auto it = colorLine.globalMatch(text); it.hasNext();) {
        const auto match = it.next();
        palette.colors.insert(match.captured(1), QColor(match.captured(2)));
    }
    static const QRegularExpression modeLine(R"re(^\s*mode\s*=\s*["']light["'])re",
                                             QRegularExpression::MultilineOption);
    palette.light = modeLine.match(text).hasMatch();
    return palette;
}

} // namespace

Theme::Theme(const QString &directory, QObject *parent, const QString &hyprctl, const QString &events)
    : QObject(parent),
      m_directory(directory.isEmpty() ? QDir::homePath() + "/.local/state/omarchy/current" : directory) {
    m_fontFamily = QGuiApplication::font().family();
    m_debounce.setSingleShot(true);
    m_debounce.setInterval(80);
    connect(&m_watcher, &QFileSystemWatcher::directoryChanged, this, [this] { m_debounce.start(); });
    connect(&m_watcher, &QFileSystemWatcher::fileChanged, this, [this] { m_debounce.start(); });
    connect(&m_debounce, &QTimer::timeout, this, &Theme::reload);

    // Corners follow Hyprland's effective rounding, which themes and personal
    // overrides can both set, so ask hyprctl rather than reading its config.
    // Without Hyprland the corners stay sharp, like Omarchy's default.
    m_hyprctl = !hyprctl.isEmpty() ? hyprctl
              : directory.isEmpty() && !qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")
                  ? QStandardPaths::findExecutable("hyprctl") : QString();
    m_roundingTimeout.setSingleShot(true);
    m_roundingTimeout.setInterval(500);
    connect(&m_roundingTimeout, &QTimer::timeout, &m_roundingProcess, &QProcess::kill);
    connect(&m_roundingProcess, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        m_roundingTimeout.stop();
        // No finished() follows a start that failed, and another try would
        // fail the same way.
        if (error == QProcess::FailedToStart)
            m_roundingAgain = false;
    });
    connect(&m_roundingProcess, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        m_roundingTimeout.stop();
        // Asked again meanwhile: what this read says may already be old.
        if (std::exchange(m_roundingAgain, false)) {
            refreshRounding();
            return;
        }
        const auto value = QJsonDocument::fromJson(m_roundingProcess.readAllStandardOutput()).object().value("int");
        if (code != 0 || status != QProcess::NormalExit || !value.isDouble() || value.toInt(-1) < 0)
            return;
        if (value.toInt() != m_radius) {
            m_radius = value.toInt();
            emit changed();
        }
    });
    if (!m_hyprctl.isEmpty()) {
        // Read again when Hyprland has read its config again, which is how a
        // theme switch or an edit reaches the rounding, rather than asking
        // every second. A change made live (hyprctl eval) says nothing, so
        // it's read again when Folio is back too.
        const QString socket = !events.isEmpty() || !hyprctl.isEmpty()
                                   ? events
                                   : qEnvironmentVariable("XDG_RUNTIME_DIR") + "/hypr/"
                                         + qEnvironmentVariable("HYPRLAND_INSTANCE_SIGNATURE") + "/.socket2.sock";
        connect(&m_events, &QLocalSocket::readyRead, this, [this] {
            bool reloaded = false;
            while (m_events.canReadLine())
                reloaded |= m_events.readLine().startsWith("configreloaded>>");
            if (reloaded)
                refreshRounding();
        });
        m_events.connectToServer(socket, QIODevice::ReadOnly);
        connect(qGuiApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
            if (state == Qt::ApplicationActive)
                refreshRounding();
        });
    }
    reload();
}

Theme::~Theme() {
    // Ending a read here must not start another, nor reach members
    // already gone.
    disconnect(&m_roundingProcess, nullptr, this, nullptr);
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingProcess.kill();
        m_roundingProcess.waitForFinished(1000);
    }
}

void Theme::refreshRounding() {
    if (m_hyprctl.isEmpty())
        return;
    if (m_roundingProcess.state() != QProcess::NotRunning) {
        m_roundingAgain = true;
        return;
    }
    m_roundingProcess.start(m_hyprctl, {"-j", "getoption", "decoration:rounding"});
    m_roundingTimeout.start();
}

QColor Theme::color(const QString &key) const {
    if (const auto it = m_colors.constFind(key); it != m_colors.constEnd())
        return *it;
    // Older themes may lack the derived backgrounds; fall back to the base.
    if (key.endsWith("background") && m_colors.contains("background"))
        return m_colors.value("background");
    if (key.endsWith("foreground") && m_colors.contains("foreground"))
        return m_colors.value("foreground");
    return fallbackColors().value(key, QColor("#888888"));
}

// Folder cards, tiles and panels are drawn in the lighter background. Some
// themes set it to the background itself (or leave it out), which would make
// them invisible, so then it's the background nudged toward the foreground.
QColor Theme::lighterBackground() const {
    const QColor lighter = color("lighter_background");
    const QColor bg = background();
    const double gap = qAbs(luminance(lighter) - luminance(bg));
    if (gap > 0.004)
        return lighter;
    return mix(bg, foreground(), 0.08);
}

// Black or white, whichever reads on the accent (WCAG relative luminance).
QColor Theme::accentForeground() const {
    return luminance(accent()) > .179 ? QColor("black") : QColor("white");
}

// Themes' own dim foregrounds are often too faint for small text, so derive a
// secondary color by mixing the foreground toward the background, keeping
// roughly a 4.5:1 contrast for metadata and labels.
QColor Theme::secondaryForeground() const {
    return mix(background(), foreground(), 0.68);
}

// A tooltip is a small surface over the pane. Take the theme's own step away
// from it - darker in light themes, lighter in dark ones - and lift it just
// enough to read as a card. Material's ToolTip forces its own dark gray, which
// leaves a light theme's text unreadable on it.
QColor Theme::tooltipBackground() const {
    const QColor base = light() ? darkBackground() : lighterBackground();
    return ensureRatio(base, background(), surfaceFloor);
}

// Tooltip words: the theme's foreground, raised to textFloor on that surface.
QColor Theme::tooltipForeground() const {
    return ensureRatio(foreground(), tooltipBackground(), textFloor);
}

// A folder card's fill: the lighter background, raised to surfaceFloor so the
// card stands off the pane on themes where the two nearly match (#24).
QColor Theme::folderFill() const {
    return ensureRatio(lighterBackground(), background(), surfaceFloor);
}

void Theme::reload() {
    refreshRounding();
    const auto paths = m_watcher.files() + m_watcher.directories();
    if (!paths.isEmpty())
        m_watcher.removePaths(paths);

    // A theme switch replaces symlinks and files, so watch the parents too and
    // re-arm after every change. Watching the nearest existing ancestor means
    // installing Omarchy later works without restarting the app.
    QString ancestor = m_directory;
    while (!QFileInfo::exists(ancestor) && ancestor != "/")
        ancestor = QFileInfo(ancestor).absolutePath();
    QStringList candidates{ancestor, QFileInfo(ancestor).absolutePath(), m_directory,
                           m_directory + "/theme", m_directory + "/theme/colors.toml"};
    candidates.removeDuplicates();
    for (const auto &path : candidates)
        if (QFileInfo::exists(path))
            m_watcher.addPath(path);

    const Palette palette = readPalette(m_directory + "/theme/colors.toml");
    if (palette.colors != m_colors || palette.light != m_light) {
        m_colors = palette.colors;
        m_light = palette.light;
        emit changed();
    }
}

// Folio's text sizes are drawn for Omarchy's default text size, 11pt, at
// which its base text is 13px; the system's size scales them, up to half
// again or down by a quarter, past which the rows and columns no longer
// hold their text.
static constexpr qreal designPoints = 11;
static constexpr int designPixels = 13;

qreal Theme::textScaleFor(qreal points) {
    return points > 0 ? std::clamp(points / designPoints, 0.75, 1.5) : 1.0;
}

QString Theme::resolveFamily(const QString &family) {
    // Asked of a configuration loaded afresh, rather than of the one Qt read
    // when Folio started, so a choice made since (omarchy-font-set) counts,
    // and Qt's own is left alone.
    FcConfig *config = FcInitLoadConfigAndFonts();
    if (!config)
        return family;
    QString resolved = family;
    if (FcPattern *pattern = FcNameParse(reinterpret_cast<const FcChar8 *>(family.toUtf8().constData()))) {
        FcConfigSubstitute(config, pattern, FcMatchPattern);
        FcDefaultSubstitute(pattern);
        FcResult result;
        if (FcPattern *match = FcFontMatch(config, pattern, &result)) {
            FcChar8 *name = nullptr;
            if (FcPatternGetString(match, FC_FAMILY, 0, &name) == FcResultMatch && name)
                resolved = QString::fromUtf8(reinterpret_cast<const char *>(name));
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pattern);
    }
    FcConfigDestroy(config);
    return resolved;
}

void Theme::followSystemFont() {
    m_fontDebounce.setSingleShot(true);
    m_fontDebounce.setInterval(200);
    connect(&m_fontDebounce, &QTimer::timeout, this, &Theme::readSystemFont);
    // omarchy-font-set writes the user's fontconfig file; its folder is
    // watched too, for when it's made.
    const QString fontconfig = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) + "/fontconfig";
    QDir().mkpath(fontconfig);
    m_fontWatcher.addPath(fontconfig);
    if (QFileInfo::exists(fontconfig + "/fonts.conf"))
        m_fontWatcher.addPath(fontconfig + "/fonts.conf");
    connect(&m_fontWatcher, &QFileSystemWatcher::directoryChanged, this, [this, fontconfig] {
        // A file written anew stops being watched; it's watched again.
        if (QFileInfo::exists(fontconfig + "/fonts.conf") && !m_fontWatcher.files().contains(fontconfig + "/fonts.conf"))
            m_fontWatcher.addPath(fontconfig + "/fonts.conf");
        m_fontDebounce.start();
    });
    connect(&m_fontWatcher, &QFileSystemWatcher::fileChanged, this, [this] { m_fontDebounce.start(); });
    // The platform theme (qtengine) tells each window when its fonts change.
    qApp->installEventFilter(this);
    readSystemFont();
}

bool Theme::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::ThemeChange)
        m_fontDebounce.start();
    return QObject::eventFilter(watched, event);
}

void Theme::readSystemFont() {
    const QFont fixed = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    const QString family = resolveFamily(fixed.family());
    const qreal scale = textScaleFor(fixed.pointSizeF());
    QFont font(family);
    font.setStyleHint(QFont::Monospace);
    font.setPixelSize(qRound(designPixels * scale));
    // New controls and dialogs take the application's font; what's on screen
    // follows fontFamily and textScale.
    if (font != QGuiApplication::font())
        QGuiApplication::setFont(font);
    if (family != m_fontFamily || !qFuzzyCompare(scale, m_textScale)) {
        m_fontFamily = family;
        m_textScale = scale;
        emit fontChanged();
    }
}
