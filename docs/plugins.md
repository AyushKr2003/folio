# Writing a Folio plugin

A plugin adds something Folio doesn't know about on its own: a section in the
sidebar, a status on one of its rows, entries in the right-click menu, rows
in the details pane, or a view in the main pane. Tailscale, Dropbox and LocalSend come with
Folio as plugins, and they use nothing that isn't described here, so a plugin
you write sits on equal footing with them.

A plugin is a folder holding a `manifest.json` and some commands. Folio never
loads your code: it runs your commands and reads the JSON they print, the way
it runs thumbnailers. Write them in any language. A command that hangs is
killed and one that prints nonsense is ignored; either way your rows and menu
entries just don't show until it works again, and Folio carries on.

Plugins are trusted like Nautilus scripts: they run as you, with your files.
Install only plugins you'd run yourself. A plugin's folder and its
`manifest.json` must belong to you (or to root, for a package's) and be
writable by no one else, not even a group; otherwise Folio skips it. The same
goes for a command of the plugin's own (`./x`, `bin/x`) and the folders it's
in, or Folio won't run it.

## Where plugins live

| Folder | What's there |
| --- | --- |
| `/usr/share/folio/plugins/<id>/` | Plugins installed by packages, including the three that come with Folio. |
| `~/.local/share/folio/plugins/<id>/` (`$XDG_DATA_HOME`) | Your own. One with the same id replaces the packaged one entirely. |

The folder's name is the plugin's id: lowercase letters, digits and dashes,
starting with a letter or digit (`tailscale`, `my-nas`). Folio reads the
plugins when it starts and again on F5. To try plugins from a checkout, set
`FOLIO_PLUGINS_PATH` to a folder of plugin folders; Folio then reads it in
place of `/usr/share/folio/plugins` (the user's folder still wins).

## A complete example

This plugin lists the remote folders bookmarked in GTK's bookmarks (the
`sftp://` and `smb://` lines Folio's Pinned section leaves out) in a sidebar
section called Servers, opens them through GVfs, and adds Share > Upload To
with one entry per server.

```
~/.local/share/folio/plugins/servers/
├── manifest.json
├── servers          (executable)
├── upload           (executable)
├── server.svg
└── i18n/
    └── da.json
```

`manifest.json`:

```json
{
  "folio": 1,
  "requires": ["gio"],
  "poll": { "command": ["./servers"], "every": 60 },
  "section": { "title": "Servers" },
  "actions": [
    { "id": "upload", "label": "Upload To", "menu": "share", "to": "items",
      "command": ["./upload"], "busy": "Uploading to %1…" }
  ]
}
```

`servers`, which Folio runs every minute and whose items fill the section and
the Upload To menu:

```sh
#!/bin/sh
# Lists the remote bookmarks in GTK's bookmarks file, one item per server.
bookmarks="${XDG_CONFIG_HOME:-$HOME/.config}/gtk-3.0/bookmarks"

# A bookmark's label can hold quotes or backslashes, which JSON escapes.
quote() { printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g')"; }

printf '{"items":['
grep -E '^(sftp|smb|davs?|ftp)://' "$bookmarks" 2>/dev/null | {
  separator=
  while read -r url label; do
    # Bookmarks without a label go by their server's name.
    host=${url#*://}
    host=${host%%/*}
    scheme=$(printf '%s' "${url%%://*}" | tr 'a-z' 'A-Z')
    printf '%s{"id":%s,"name":%s,"icon":"server.svg","subtitle":"%s",' \
      "$separator" "$(quote "$url")" "$(quote "${label:-$host}")" "$scheme"
    printf '"details":[["Address",%s]],"open":{"mount":%s}}' "$(quote "$url")" "$(quote "$url")"
    separator=,
  done
}
printf ']}\n'
```

`upload`, which Folio runs with the selected files as its arguments and the
chosen server in `FOLIO_TARGET`:

```sh
#!/bin/sh
# Copies the selected files to the top of the chosen server. Mounting first
# does nothing when it's mounted already; with no terminal to ask in, it only
# works where keys are set up, and gio's error says so when they aren't.
gio mount "$FOLIO_TARGET" </dev/null 2>/dev/null
gio copy -- "$@" "$FOLIO_TARGET" || exit 1
echo '{"notice": "Uploaded."}'
```

`server.svg`, drawn on the same 24-unit grid as Folio's own glyphs (Folio
colors it to match the theme):

```svg
<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 24 24" fill="none" stroke="#000" stroke-width="1.6">
  <path d="M4 4h16v7H4zM4 13h16v7H4zM7 7.5h2M7 16.5h2"/>
</svg>
```

`i18n/da.json`:

```json
{
  "Servers": "Servere",
  "Upload To": "Upload til",
  "Uploading to %1…": "Uploader til %1…",
  "Address": "Adresse",
  "Uploaded.": "Uploadet."
}
```

Try the commands yourself before Folio does; what they print is all Folio
sees:

```sh
cd ~/.local/share/folio/plugins/servers
FOLIO_PLUGIN_DIR=$PWD FOLIO_LANGUAGE=en ./servers | jq .
```

## An example with menu entries and details

This plugin adds Copy Git Link to the right-click menu of files in your
repositories on GitHub, and shows in the details pane the branch a
repository is on and the last commit that changed the file. It has no poll:
what it offers is worked out for the file at hand.

```
~/.local/share/folio/plugins/git/
├── manifest.json
├── link       (executable)
├── details    (executable)
└── i18n/
    └── da.json
```

`manifest.json`:

```json
{
  "folio": 1,
  "requires": ["git", "jq"],
  "actions": [
    { "id": "link", "label": "Copy Git Link", "command": ["./link"],
      "under": ["~/Code/"], "files": true, "single": true }
  ],
  "details": { "command": ["./details"], "under": ["~/Code/"] }
}
```

`details`, which Folio runs with the file or folder the details pane shows:

```sh
#!/bin/sh
# The branch the repository is on and the last commit that changed the file
# or folder; outside a repository, nothing.
quote() { printf '"%s"' "$(printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g')"; }

if [ -d "$1" ]; then cd "$1"; else cd "$(dirname "$1")"; fi || { echo '[]'; exit 0; }
branch=$(git branch --show-current 2>/dev/null) || { echo '[]'; exit 0; }
printf '[["Branch", %s]' "$(quote "$branch")"
last=$(git log -1 --format=%s -- "$1" 2>/dev/null)
[ -n "$last" ] && printf ', ["Last commit", %s]' "$(quote "$last")"
printf ']\n'
```

`link`, which Folio runs with the selected file:

```sh
#!/bin/sh
# Copies a link to the file as it is at the commit checked out.
cd "$(dirname "$1")" || exit 1
# -z gives the name as it is, without Git's quoting of unusual ones.
file=$(git ls-files -z --full-name -- "$(basename "$1")" | tr -d '\0')
[ -n "$file" ] || { echo '{"error": "Not tracked by Git."}'; exit 1; }
repo=$(git remote get-url origin 2>/dev/null | sed -E 's#^git@github\.com:#https://github.com/#; s#\.git$##')
case "$repo" in
  https://github.com/*) ;;
  *) echo '{"error": "Not a GitHub repository."}'; exit 1 ;;
esac
# Each part of the path is percent-encoded, so a name like notes#1.md
# isn't cut short at its # (or ?) by the browser.
jq -cn --arg base "$repo/blob/$(git rev-parse HEAD)/" --arg file "$file" \
  '{copy: ($base + ($file | split("/") | map(@uri) | join("/"))), notice: "Git link copied."}'
```

`i18n/da.json`:

```json
{
  "Copy Git Link": "Kopiér Git-link",
  "Branch": "Gren",
  "Last commit": "Seneste commit",
  "Git link copied.": "Git-link kopieret.",
  "Not tracked by Git.": "Ikke sporet af Git.",
  "Not a GitHub repository.": "Ikke et GitHub-repository."
}
```

## The manifest

Every key but `folio` is optional; a plugin uses only the hooks it needs.
Unknown keys are ignored, so a manifest written for a later Folio still loads.

| Key | Meaning |
| --- | --- |
| `folio` | The version of this interface the plugin is written for: `1`. A plugin asking for another is skipped. |
| `requires` | Programs that must be on `PATH`, checked when plugins are read. Without them the plugin is off: no rows, no entries, no polling. |
| `poll` | `{"command": [...], "every": seconds}`: the command whose output gives the plugin's items, statuses and available actions (see [Polling](#polling)). `every` defaults to 15, and is at least 5 and at most 86400 (a day). |
| `section` | A sidebar section of the plugin's items (see [The sidebar section](#the-sidebar-section)). |
| `view` | A view of all the plugin's items in the main pane (see [The view](#the-view)). |
| `actions` | Entries in the right-click menu, in the menu itself or under Share (see [Actions](#actions)). |
| `details` | Rows in the details pane about a file or folder (see [Details](#details)). |

## Running commands

A command is an array: the program, then its arguments. A program with a `/`
that isn't absolute (`./servers`, `bin/poll`) is in the plugin's folder; a
bare name (`tailscale`) is looked up on `PATH`. Folio never runs commands
through a shell, so file names with spaces or quotes arrive intact as single
arguments; quote `"$@"` in your scripts to keep them that way.

Every command runs in the plugin's folder, with no input, and with:

| Variable | Value |
| --- | --- |
| `FOLIO_PLUGIN_DIR` | The plugin's folder. |
| `FOLIO_LANGUAGE` | The language Folio is showing, as Omarchy's code: `en`, `da`, `pt-BR`. Folio translates the text you print itself (see [Translations](#translations)); this is for anything else that should follow the language. |
| `FOLIO_TARGET`, `FOLIO_TARGET_NAME` | For an action in a submenu, the chosen item's `id` and `name`. |

Each command starts in a process group of its own, and when it finishes or
Folio ends it, the whole group goes, so nothing it started lingers. What it prints on
standard output must be one JSON object (for a details command, an array),
at most 1 MB. What it prints on standard error goes to Folio's own standard
error (the journal), each line marked with the plugin's id and cut short at
1000 bytes; a failing action's last line there is also its error message.

How long a command may take depends on what it's for:

- A **poll** has 10 seconds. One that takes longer is killed and counts as
  failed.
- A **details** command has 2 seconds, since the details pane waits for it.
  One that takes longer is killed and shows nothing.
- An **action** or an item's **run** command is work you asked for
  (sending a big file takes as long as it takes), so it has no time limit.
  While it runs, the status bar shows its `busy` text (for a run command,
  "Opening" and the item's name), and the rest of Folio stays usable, file
  operations included. A second click on the same entry
  waits until the first is done. Closing Folio ends it.
- An action with `"detach": true` is started and forgotten, for programs
  that open a window of their own.

### Text

Wherever a command prints text a person reads (a status, a subtitle, a
notice, an error), it can be a string, or an array of an English template
and the values for its `%1`, `%2`…:

```json
{"error": ["Couldn't send to %1: %2", "studio", "no such peer"]}
```

Folio looks the template up in the plugin's catalog and fills in the values,
which it doesn't translate. That way a shell script never handles languages,
and switching language in Omarchy changes the text at once, without running
anything again.

## Polling

Folio runs the poll command when plugins are read, every `every` seconds
after that, when you press F5, and after one of the plugin's actions
finishes, so its effect shows. One poll runs at a time per plugin: one that's
due while the last is still running is skipped. Folio keeps the latest output
in memory and answers everything from it: drawing the sidebar or opening a
menu never runs a command. Nothing is kept between launches, so nothing stale
shows at start.

A poll that fails (it exits with an error, crashes, times out or prints
something that isn't JSON) counts as an empty answer: the plugin's rows,
statuses and actions go until a poll works again. Folio writes why to
its standard error, once each time a plugin goes from working to failing.

The output has three optional keys:

```json
{
  "items": [
    {
      "id": "studio.example.ts.net",
      "name": "studio",
      "icon": "peer",
      "subtitle": "Linux",
      "details": [["Host", "studio.example.ts.net"]],
      "open": { "mount": "sftp://studio.example.ts.net/", "verified": true }
    }
  ],
  "places": [
    { "path": "~/Dropbox", "icon": "dropbox.svg", "status": "syncing", "tone": "warning" }
  ],
  "actions": ["link"]
}
```

**`items`** are the things the plugin offers, in the order they should be
listed. They fill its section, its view and its submenus.

| Key | Meaning |
| --- | --- |
| `id` | Required, unique and stable across polls: it's what Folio remembers an opened item by, and what `FOLIO_TARGET` carries. |
| `name` | Required. Shown as it is, never translated. |
| `icon` | See [Icons](#icons). Defaults to `file`. |
| `subtitle` | One short line: under the name on a tile, in the size column of the list, and in the details. Translated. |
| `details` | Rows for the details pane, each `[label, value]`; the label is translated, the value isn't. |
| `open` | What opening it does (see [Opening an item](#opening-an-item)). An item without one is only a target in its plugin's submenus. |

**`places`** puts a status, or another icon, on sidebar rows Folio already
has: Home's folders, pinned folders and drives. `path` may start with `~`.
`status` is a word or two, shown quietly at the row's end and translated;
`tone` colors it: `quiet` (the default), `warning` (the theme's yellow) or
`error` (its red). Leave `status` out when there's nothing worth saying; up
to date is the normal state and says nothing. When two plugins mark the same
row, the one whose id sorts first is shown.

**`actions`** names the actions that are available right now, by `id`.
Leave it out and every action is available. A plugin with a poll offers no
actions while its poll is failing.

## The sidebar section

```json
"section": { "title": "Tailnet", "rows": "opened", "limit": 6 }
```

The section comes after Devices, sections ordered by plugin id, and shows
while the plugin has items. Its rows are either:

- `"rows": "all"` (the default): every item, in the plugin's order.
- `"rows": "opened"`: only the items you've opened, most recently opened
  first, at most `limit` of them (6 by default), while they're among the
  plugin's items. Each has a ✕ on hover to take it off the sidebar. This
  needs a `view`, where the rest are found.

Remembering is Folio's job: opening an item from its row or from the view
puts it at the top. Folio keeps the 20 most recent ids per plugin in its
settings, so an item that goes away for a while (a machine asleep) comes back
to the sidebar when it reappears. When the plugin itself goes away
(uninstalled, or its `requires` missing), its section goes with it but the
ids stay, so it's all back if the plugin returns.

With a `view`, the section ends with a row that opens it, showing the view's
`label` and `icon` and, quietly, how many items there are.

A row is lit while you're inside what it opens. A row that opens a local
folder takes dropped files like any other place.

## The view

```json
"view": { "title": "Tailnet", "label": "Machines", "icon": "tailnet",
          "empty": "No machines are online" }
```

The view lists all the plugin's items in the main pane, at the path
`plugin:<id>` (which also works typed into the location bar). `title` names
it in the window's title, the path bar and the search field ("Search
Tailnet") and is required; `label` (the `title` when left out) and `icon`
(`file` when left out) are its row at the end of the section; `empty` is
shown when there are no items. It follows the polls, so items come and go
while you look at it.

Items show in the grid, list and columns layouts with their icon, name and
subtitle, in the plugin's order (the sort menu doesn't apply), and the
details pane shows the item's details. / searches their names. ⏎, a double
click or Open in the right-click menu opens one; there's nothing to copy,
rename, pin or preview in a view, so those are left out.

## Opening an item

An item's `open` is one of:

| `open` | What happens |
| --- | --- |
| `{"path": "/some/folder"}` | Folio goes to that folder, an absolute path or one starting with `~`. |
| `{"mount": "sftp://host/", "verified": true}` | Folio mounts the URL through GVfs (`sftp://`, `smb://`, `davs://`, anything GVfs mounts) and goes to the URL's folder. A URL naming only a server lands in the folder at your home's path on it when there is one (one person's machines tend to share a layout), or its top otherwise. A password request is asked in the window. `verified` says the plugin has already made sure who's on the other end, as Tailscale does, so Folio answers GVfs's host-key questions yes; without it, Folio asks you, as it does for an address typed in the path bar. |
| `{"run": ["./wake", "studio"]}` | Folio runs the command as it runs an action, and acts on its [result](#results). |

## Actions

```json
"actions": [
  { "id": "thumbnail", "label": "Make Thumbnail", "types": ["image/*"],
    "files": true, "command": ["./thumbnail"] },
  { "id": "link", "label": "Dropbox Link", "menu": "share", "command": ["./link"],
    "under": ["~/Dropbox/"], "single": true, "busy": "Getting a Dropbox link for %1…" },
  { "id": "taildrop", "label": "Taildrop", "menu": "share", "to": "items", "files": true,
    "command": ["./send"], "busy": "Sending to %1…" }
]
```

Each action runs its `command` with the selected paths appended as
arguments. With `"to": "items"` the action is a submenu with one entry per
item instead, and the chosen item comes in `FOLIO_TARGET` and
`FOLIO_TARGET_NAME`.

| Key | Meaning |
| --- | --- |
| `id` | Required, unique in the plugin; what the poll's `actions` names. |
| `label` | Required. The entry, or the submenu's title, in Title Case as Folio's own entries are. Translated. |
| `command` | Required. |
| `menu` | `"main"` (the default) for an entry in the menu itself, `"share"` for one under Share. |
| `to` | `"items"` for a submenu of the plugin's items. It only shows while there are items. |
| `under` | Folders the selection must be inside, every item of it. `~` works. |
| `types` | Types the selection must be of, every item of it, as MIME types: `application/pdf`, or `image/*` for every kind of picture. A type takes the types that are kinds of it, so `text/plain` takes source code too, and a folder is `inode/directory`. Folio tells a file's type by its name. |
| `files` | `true` when the entry can't take folders: it doesn't show while a folder is selected. |
| `single` | `true` when the entry takes one item: it doesn't show for several. |
| `busy` | What the status bar says while it runs; `%1` is the chosen item's name in a submenu, or the first file's name. Translated. |
| `detach` | `true` to start it and not wait (see [Running commands](#running-commands)). |

Actions show on files and folders wherever Folio lists real ones, never in a
view or in the trash. Those in the menu itself come together after Compress (or Extract, and
Burn), just before Share; the others are Share's entries. Each lot comes in
plugin id order, plain entries first and submenus after them, and Share
itself is left out when none of its entries applies.

### Results

An action or a `run` command that succeeds may print what Folio should do
next; printing nothing is fine.

| Key | What Folio does |
| --- | --- |
| `notice` | Shows it in the status bar for a moment. Text. |
| `copy` | Puts it on the clipboard. Not translated. |
| `path` | Goes to that folder, an absolute path or one starting with `~`. |
| `error` | Shows it as a failure, even with a zero exit, and does none of the above. Text. |

A command that exits with an error fails: Folio shows its `error` if it
printed one, otherwise the last line of its standard error after the action's
label ("Taildrop: no such peer").

## Details

```json
"details": { "command": ["./filestatus"], "under": ["~/Dropbox/"] }
```

When the details pane shows a file or folder (a real one: not in the trash,
nor a plugin's item) that's inside `under` and of one of the `types`, both
as for [actions](#actions) and both optional, Folio runs `command` with its
path as the last argument. The command prints a JSON array of rows, each
`[label, value]`; the label is translated and the value isn't. Printing `[]`
is fine when there's nothing to say.

```json
[["Dropbox", "up to date"]]
```

The rows show below Folio's own, plugins in id order. Folio keeps them for
the file as it was last modified, so going through a folder and back runs
nothing again. It asks again about a file changed since, about everything
after F5, and about every file once the plugin's poll prints something new,
since a plugin's state can change how a file stands without changing the
file (Dropbox finishing a sync).

The command runs in the background, never holding up the window, and has 2
seconds. One that fails, takes longer or prints something other than a list
of rows shows nothing for the file, and Folio writes why to its standard
error, once each time the plugin goes from working to failing. Moving on to
another item before a command's turn comes means it isn't run at all; an
answer that comes after the selection has moved on is kept, but not shown.

## Icons

An `icon` is either the name of one of Folio's glyphs:

`folder` `documents` `downloads` `pictures` `music` `videos` `recent`
`trash` `drive` `peer` (a computer) `tailnet` (computers joined) `open`
`file`

or the name of an SVG file in the plugin's folder, ending in `.svg`
(`server.svg`, `icons/nas.svg`). Folio draws the SVG's shape in the theme's
colors, whatever colors it was drawn in, so it matches the glyphs around it;
draw it on a 24-unit square with strokes about 1.6 wide, as they are. An
unknown name or a missing file shows `file`.

## Translations

A plugin ships its translations as `i18n/<code>.json`, one per Omarchy
language code (`da.json`, `pt-BR.json`), in the same format as Folio's own:
an object from the English text to its translation. Folio uses the catalog
for the manifest's labels and titles and the text its commands print, as
marked above, falling back from `pt-BR` to `pt` to the English. Names, ids,
values and anything to copy are never looked up.

An `i18n/en.json` listing every English text the plugin shows, each as
itself, gives translators the whole of it to work from; the plugins that
come with Folio have one.

## When something's wrong

Folio says nothing in the window about a plugin that fails; a broken plugin
only goes missing. It writes the reason to its standard error, so run
`folio` from a terminal, or read the journal, to see a manifest it couldn't
read, a missing program from `requires`, a poll or details command that
failed or timed out, or output that wasn't JSON.

## The plugins that come with Folio

They're in the `plugins/` folder of Folio's repository and install to
`/usr/share/folio/plugins/`.

**tailscale** lists the online machines that can hold files, opens them over
SFTP, keeps the ones you've opened in the sidebar, and sends files with
Taildrop.

```json
{
  "folio": 1,
  "requires": ["tailscale", "jq"],
  "poll": { "command": ["./machines"] },
  "section": { "title": "Tailnet", "rows": "opened" },
  "view": { "title": "Tailnet", "label": "Machines", "icon": "tailnet",
            "empty": "No machines are online" },
  "actions": [
    { "id": "taildrop", "label": "Taildrop", "menu": "share", "to": "items", "files": true,
      "command": ["./send"], "busy": "Sending to %1…" }
  ]
}
```

`machines` reads `tailscale status --json` with `jq` and prints each online
Linux, macOS or Windows machine as the item shown under [Polling](#polling);
`send` runs `tailscale file cp "$@" "$FOLIO_TARGET:"`.

**dropbox** marks the Dropbox folder in Home with Dropbox's mark and its sync
status, gets share links, and says in the details pane how a file in it
stands.

```json
{
  "folio": 1,
  "poll": { "command": ["./status"] },
  "actions": [
    { "id": "link", "label": "Dropbox Link", "menu": "share", "command": ["./link"],
      "under": ["~/Dropbox/"], "single": true,
      "busy": "Getting a Dropbox link for %1…" }
  ],
  "details": { "command": ["./filestatus"], "under": ["~/Dropbox/"] }
}
```

It requires nothing, so the folder keeps its mark without `dropbox-cli`.
`status` prints `{}` without a `~/Dropbox` folder; otherwise the mark,
plus, when `dropbox-cli` is there, `syncing` (warning) or `stopped` (error)
from `dropbox-cli status`, and `"actions": ["link"]` while Dropbox runs
(`"actions": []` otherwise, since leaving `actions` out would offer the link). `link`
runs `dropbox-cli sharelink` and prints `{"copy": link, "notice": ["Dropbox
link for %1 copied.", name]}`. `filestatus` prints `[["Dropbox", status]]`
with what `dropbox-cli filestatus` says of the file (`up to date`, `syncing`
or `unsyncable`), and `[]` when Dropbox isn't running or `dropbox-cli` isn't
there.

**localsend** sends to nearby devices.

```json
{
  "folio": 1,
  "requires": ["localsend"],
  "actions": [
    { "id": "send", "label": "LocalSend…", "menu": "share",
      "command": ["localsend", "--headless", "send"], "detach": true }
  ]
}
```
