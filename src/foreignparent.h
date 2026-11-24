#pragma once

#include <QString>
#include <qwindowdefs.h>

class QWindow;

// Attaches a portal FileChooser window as transient-for its caller window,
// using xdg-foreign-unstable-v2 on Wayland ("wayland:<handle>") or
// QWindow::fromWinId on X11 ("x11:<hex>"), so compositors stack the dialog
// above its parent (including pinned floating parents on Hyprland).
namespace ForeignParent {

struct ParsedHandle {
    enum class Kind { None, Wayland, X11 };
    Kind kind = Kind::None;
    QString wayland;
    WId x11 = 0;
};

ParsedHandle parse(const QString &parentWindow);
void attach(QWindow *window, const QString &parentWindow);

} // namespace ForeignParent
