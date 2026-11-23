import QtQuick
import QtQuick.Controls

// A folder's items one per row, as the list and each of the columns show
// them: clipped to its edges, scrolled by the wheel but not dragged, with
// Folio's scroll bar.
ListView {
    clip: true
    acceptedButtons: Qt.NoButton
    boundsBehavior: Flickable.StopAtBounds
    // No cache buffer, here or in the grid: Qt's 16 ms pause before filling
    // one can leave an idle window drawing every refresh.
    cacheBuffer: 0
    // Rows scrolled out are given to the files scrolled in rather than made
    // anew, in both the list and the columns.
    reuseItems: true
    ScrollBar.vertical: FolioScrollBar { }
}
