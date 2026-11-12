.pragma library
.import QtQml as QtQml

// Sizes, dates, counts and thumbnail URLs as the views show them, in one
// copy that every tile and row shares. A library can't see context
// properties, so a binding that shows a date reads translator.language
// itself to follow a language switch; whatever goes through qsTr here is
// refreshed with the rest of the strings.

// Each unit is whole with its number, so a language can place and space it
// its own way ("1,5 Mo").
function size(bytes) {
    const units = [qsTr("%1 B"), qsTr("%1 KB"), qsTr("%1 MB"), qsTr("%1 GB"), qsTr("%1 TB")]
    let i = 0
    let value = bytes
    while (value >= 1000 && i < units.length - 1) { value /= 1000; ++i }
    return units[i].arg(value.toLocaleString(Qt.locale(), "f", i === 0 ? 0 : 1))
}

// How much room is left on a drive, as its row in the sidebar says it.
function free(bytes) {
    return qsTr("%1 free").arg(size(bytes))
}

// When it changed: the time for today, the date before that. A plugin's
// items have no date.
function when(date) {
    if (!date || isNaN(date.getTime()))
        return ""
    const locale = Qt.locale()
    const now = new Date()
    const today = date.getFullYear() === now.getFullYear() && date.getMonth() === now.getMonth() && date.getDate() === now.getDate()
    return today ? date.toLocaleTimeString(locale, QtQml.Locale.ShortFormat) : date.toLocaleDateString(locale, QtQml.Locale.ShortFormat)
}

// A folder's contents; nothing until they've been counted.
function count(n) {
    return n === 0 ? qsTr("No items") : n > 0 ? qsTr("%Ln items", "", n) : ""
}

// What an item is and how big, under its name in the details and the
// preview; for a plugin's item, its subtitle.
function summary(item, type) {
    if (!item || !item.path)
        return ""
    if (item.kind === "item")
        return item.extension
    if (item.isDir)
        return item.itemCount === 0 ? qsTr("Folder · No items") : item.itemCount > 0 ? qsTr("Folder · %Ln items", "", item.itemCount) : qsTr("Folder")
    return qsTr("%1 · %2").arg(type || item.mime).arg(size(item.size))
}

// The file's modified time is part of the URL: Qt keeps the images it has
// shown by their URL, so a picture saved over would otherwise come back
// with its old pixels.
function thumb(path, mime, px, modified) {
    const changed = modified ? modified.getTime() : 0
    return "image://thumb/" + px + "/" + changed + "/" + encodeURIComponent(mime) + "/" + encodeURIComponent(path)
}

// A text file's first lines on a page in the theme's colors.
function page(path, px, modified, paper, ink) {
    const changed = modified ? modified.getTime() : 0
    const hex = (color) => String(color).slice(1)
    return "image://thumb/page/" + px + "/" + changed + "/" + hex(paper) + "/" + hex(ink) + "/" + encodeURIComponent(path)
}

// A key combination as people read it: "Ctrl + C", not "Ctrl+C".
function keys(text) {
    return text.replace(/(\S)\+(?=\S)/g, "$1 + ")
}
