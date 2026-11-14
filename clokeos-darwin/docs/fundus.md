# Fundus 2: a Finder for ClokeOS

Status 2026-11-14: first version, checked in the arm64 VM (see "What works"). Fundus was Thunar (Xfce, GTK) under another name. Fundus 2 is a Mac app
written in Objective-C on ClokeOS's own AppKit (Darling's Cocotron), so it looks and behaves like the Finder, and the same
browser shows up in the Open and Save panels of every Mac app (Logic Pro included).

## Why Objective-C on our AppKit (decision of 2026-11-14)

| Option | For | Against |
|---|---|---|
| **Objective-C + ClokeOS AppKit** (chosen) | the Finder's own world; one browser component for Fundus *and* `NSOpenPanel`/`NSSavePanel` in Mac apps; every fix to AppKit helps Mac apps too; Apple's SDK and compiler | AppKit is still incomplete and slower than GTK; GTK and Windows programs keep their own dialogs for now |
| C + GTK | fastest to write, every GTK widget works today | not the Mac world; Open/Save in Mac apps would need a separate service |
| Rust + GTK | memory safety, speed | new toolchain (not installed), bindings to PureDarwin, longest start |

## Layers (so that parts can move to another language later)

```
Fundus.app (Objective-C)            windows, toolbar, menus, tabs, preferences
FundusKit.framework (Objective-C)   FKBrowserView (sidebar, icon and list view, path bar, status bar),
                                     FKPanel (Open/Save), icons, localization
FundusCore (plain C, fcore.c)       directory listing, kinds, sizes, trash, copy/move/rename, special folders
open (C, Xlib) + CloakServer        "open this path": apps, folders, documents with their default program
```

Rules that keep porting possible: FundusCore has a C API with no Objective-C or Cocoa types (plain structs, UTF-8
paths, error codes), so it can be rewritten in Rust or Swift, or called from GTK and Raven, without touching the UI.
The UI talks to the core only through `fcore.h`. File operations never run inside view code.

## Languages for ClokeOS in the future (recommendation)

* **C** for system services, drivers' user-space parts and anything PureDarwin's libSystem must run without extra runtimes
  (kernel extensions stay C/C++, as XNU requires). Fastest start-up, no runtime.
* **Objective-C** for Mac-app-facing code (AppKit, Foundation, panels, Fundus): it is what AppKit speaks.
* **Swift** once ClokeOS's Swift runtime is complete (it runs Logic's Swift code already, see `docs/swift-runtime.md`):
  natural successor for new Mac-style apps; Objective-C classes can be replaced one by one because Swift and
  Objective-C share the runtime.
* **Rust** for parsers and anything that handles untrusted data (archives, disk images, network, the MIDI and open
  services), once a toolchain for PureDarwin is set up (`x86_64-apple-darwin`/`aarch64-apple-darwin` targets linked against
  PureDarwin's libSystem). Candidates to port first: `dmg.c`, `carreader.c`/`lzfsedec.c`, `clokemidid`, FundusCore.

## Localization

All user-visible text of FundusKit and Fundus is in `Resources/<lang>.lproj/Localizable.strings` (en, de, fr, es, it, the
languages ClokeOS already has). The language comes from `CLOKE_LANG`, then `/etc/clokeos-language`, then English
(Darling's `NSBundle` only follows `-AppleLanguages`, so FundusKit chooses the `.lproj` itself). Adding a language means
adding one `.strings` file.

## Phases

1. **First version** (this change): FundusCore; FundusKit browser (sidebar with Favorites and Locations, icon and list
   view, back/forward, enclosing folder, path bar, status bar, search filter, hidden files, sorting); Fundus.app (new
   window, new folder, rename, move to trash, empty trash, copy/paste, open, open with Terminal, Finder shortcuts);
   Finder-style folder and file icons; `open` command and open service in CloakServer; Open/Save panels of Mac apps
   through FundusKit; English, German, French, Spanish, Italian.
2. Drag and drop (within Fundus, to the Dock and desktop, into Mac apps), column view, Get Info, Quick Look (space),
   tags, tabs, progress window for long copies, undo.
3. Open/Save for GTK programs (portal) and Windows programs (Raven's `GetOpenFileName`) through the same panel;
   desktop icons drawn by Fundus; Spotlight index shared with Fundus search.

## What works (first version, 2026-11-14)

Checked in the arm64 VM (ClokeOS 3.0 image with the new frameworks) and, for the UI logic, natively on the Mac with Apple's
AppKit (Fundus is an ordinary Cocoa app, `DYLD_FRAMEWORK_PATH=<stage>/Library/ClokeOS/MacFrameworks`).

* **Window without a title bar** (user's rule, see the memory note): content up to the top edge, CloakServer draws the
  traffic lights over the sidebar (`FKHideTitlebar`: FullSizeContentView + transparent title bar, published as
  `_CLOKE_TITLEBAR` overlay); the toolbar row holds back/forward, the folder name, icon/list switch, new folder and search.
* **Sidebar**: Favorites (Applications, Desktop, Documents, Downloads, home, Movies, Music, Pictures) and Locations
  (Cloke HD, every volume under `/Volumes`, Windows (C:) = `/NT`, Linux = `/TUX`, Trash).
* **Icons in the Finder style**: folders with an embossed symbol for the standard folders, document icons per kind with a label
  (PDF, TXT, EXE, …), app icons from the bundle (`render.swift --apps` renders every `AppIcon.svg` to `AppIcon.png` at build time).
  Standard folders show translated names (Documents → Dokumente) while renaming still uses the real name.
* **Previews like on the Mac**: PNG/JPEG/GIF/TIFF/BMP show the picture itself (scaled once to 256 px and kept in memory),
  text and source files a small page with their first lines.
* **Quick Look**: space shows the selected item in a window without title bar (picture, text/code in a scroll view, otherwise
  a large icon with kind, size and date); arrow keys move on, space or Esc close it.
* **Functions**: icon and list view (sortable columns Name, Date Modified, Size, Kind), back/forward, enclosing folder, Go menu
  with the Finder shortcuts, Go to Folder, new window, new folder (inline rename), rename (Return or a second click on the name),
  move to trash, empty trash, duplicate, copy/cut/paste, open, open in Terminal, show hidden files, search filter, type-ahead,
  path bar (clickable), status "n items, x available", context menus. Localized in en, de, fr, es, it.
* **open service**: `/usr/bin/open [-a app | -b bundle-id] [-R] path…` (`cloakserver/open.c`) appends the request to the root
  window property `_CLOKE_OPEN`; CloakServer starts apps, opens folders in Fundus, documents with `gio open` (mimeapps.list),
  `-R` reveals in Fundus. Mac apps get documents as `-NSOpen <path>`. CloakServer's old Thunar calls (Trash, "Show in Fundus",
  Spotlight files, open Applications) use it.
* **Open and Save panels of Mac apps**: `macapps/appkit/fundusbridge.m` (in the X11 backend of AppKit) answers Cocotron's
  `implementsCustomPanelForClass:` with yes and runs `+[FKPanel fk_runPanelWithOptions:]`; Darling's panel object keeps the
  settings and gets the result. Also added: `-[NSSavePanel beginWithCompletionHandler:]`, `beginSheetModalForWindow:
  completionHandler:` and `-[NSDocumentController beginOpenPanelWithCompletionHandler:]`, `beginOpenPanel:forTypes:
  completionHandler:` (Darling had none of them; Logic's "Open…" calls the document controller one). Logic Pro 12.3: File >
  Open… shows the Fundus panel (files Logic cannot open are dimmed, `Jump.logicx` can be chosen and opened); File > Save As…
  shows it with Logic's own accessory view (project organisation and copy options) below the browser. `CLOKE_FUNDUS_PANEL=0`
  goes back to Darling's panel.

Found on the way (Darling, arm64): `-[NSFileManager fileExistsAtPath:isDirectory:]` (via `fileSystemRepresentation`) and
looking up an `NSNumber` key in a dictionary literal hung Fundus at start; FundusKit therefore checks files through
FundusCore (`fc_exists`) and uses `UTF8String` paths. Loading a PDF with `NSImage` kills the process with SIGTRAP in Onyx2D's PDF
reader, so PDFs keep their document icon (no thumbnail, no Quick Look picture). Logic tests ran with the logic session's image,
whose AppKit is newer than main's; there the bridge was injected as a separate library built from the same source.

## Look and drag and drop (2026-11-14, second round)

* **Looks like the Finder of macOS 27** (compared side by side on the Mac, light and dark): toolbar with capsules (back/forward
  with a divider, icon/list switch with the selected view in a darker capsule, new folder, a round search button that opens a
  search field in a capsule), no line under the toolbar, sidebar in its own slightly darker colour without a border (rows 28 pt,
  accent-coloured symbols, rounded selection), icon view with 112 pt cells, 64 pt icons and pictures without a white frame, the
  name in an accent capsule when selected, list view with 20 pt rows, rounded zebra stripes that run to the bottom of the
  window, column separators and a sort chevron, size right-aligned, dates as "Today at 16:31" / "Heute, 16:31". Path bar and
  status bar are off by default like in the Finder (View > Show Path Bar ⌥⌘P, Show Status Bar ⌘/). Colours follow light/dark
  mode of the program (`CLOKEOS_DARK`/`AppleInterfaceStyle`).
* **Drag and drop** inside Fundus: selected items onto a folder, onto an app (opens them with it), into another Fundus window,
  or onto a sidebar entry (Trash = move to Trash). Same volume moves, another volume copies, ⌥ always copies (green plus on the
  drag image, red counter for several items). The target folder or window is highlighted; without a target the drag image slides
  back. Dropped items are selected in the target window.
* **Window focus like macOS**: a click into a background window does not bring it forward; it comes forward on mouse release
  only if nothing was dragged; the target window of a drop is not activated. CloakServer already raised windows only on release
  (`pending_focus`); now Fundus calls `-[NSApplication preventWindowOrdering]` when a drag starts, which our AppKit
  (`macapps/appkit/ordering.m`) turns into the property `_CLOKE_PREVENT_ORDERING` on the window, and CloakServer then skips the
  raise. `ordering.m` also implements `+[NSWindow windowNumberAtPoint:belowWindowWithWindowNumber:]` from the X server's
  stacking order (Darling had neither).
* **Drag image without black box**: Darling's X11 windows have no alpha channel, so FundusKit draws the drag image into a bitmap
  and cuts the window to its opaque pixels with XShape (`-[NSWindow clk_setShapeRects:]` in `ordering.m`); CloakServer's
  compositor now clips every window to its XShape bounding shape (`comp.c`), which also fixes undefined pixels outside a shape.
* Tested in the arm64 VM: background-window drag into the front window (file moved, source window stayed behind, front window
  unchanged), ⌥-drag onto a folder in the other window (copied). `clokeos-xtest drag x1 y1 x2 y2 [alt]` drives such tests.

## Scroll bars like macOS (all Mac apps)

`macapps/appkit/overlayscrollers.m` gives Darling's AppKit overlay scroll bars ("Show scroll bars: when scrolling"): content runs
under the scroll bar, which is invisible until the view scrolls, then a thin rounded knob without track that fades out after
about a second (wider with a light track while the knob is dragged); an invisible scroll bar does not take clicks.
`AppleShowScrollBars = Always` or `CLOKE_SCROLLBARS=always` keeps the classic ones. On the way: Darling decided a scroller's
direction once in `-initWithFrame:`; NSScrollView creates them 15 × 0 points large, so vertical scroll bars thought they were
horizontal (no knob, the wheel over them did nothing); and the line scroll was 1 point (3 points per wheel step) instead of
Apple's 10. Checked in Fundus (VM) and in Logic Pro 12.3 (no permanent scroll bars any more, scrolling works; Logic's track
area is wider than the 1280-pixel VM screen, so its knob could not be seen there).

## Rubber band, spring-loaded folders, autoscroll (2026-11-14, third round)

* **Rubber-band selection**: dragging on empty space draws a selection rectangle (light accent fill, accent border) and selects
  every item it touches; with ⌘ or ⇧ it toggles against the previous selection, as in the Finder.
* **Autoscroll**: near or past the top or bottom edge the view scrolls, faster the further out the pointer is, also while the mouse
  stands still, both for the rubber band and while dragging items.
* **Spring-loaded folders**: holding dragged items over a folder (or a sidebar entry) for 0.9 s makes it blink twice and opens it
  in the same window; the drag goes on there.
* Found on the way and fixed for every Mac app:
  * `NSMutableIndexSet` (Darling Foundation) inserted new ranges before instead of after their predecessor (`DL_INSERT`), so the
    range list became unsorted: after adding 4, 5, 10, 11 `containsIndex:10` was false although `count` was 4. Patch
    `macapps/patches/darling-foundation/src/NSIndexSet.m.sed` (also the split in `removeIndexesInRange:`).
  * `-[NSWindow nextEventMatchingMask:untilDate:inMode:dequeue:]` (Darling AppKit) never returned when `untilDate` passed: the
    `nil` from NSApp counted as "not matching" and the loop asked again. Tracking loops that work while the mouse stands still
    (autoscroll, spring loading) stopped. Patch in `macapps/patches/darling-cocotron/AppKit/NSWindow.m.sed`.
  * After scrolling inside a tracking loop Darling passed a shifted dirty rectangle; FundusKit draws the whole visible area.
  * The drag image's shape is now built from the icon's pixels plus rectangles for the name capsule and badges (text drawn into
    the mirrored bitmap landed one line off in Darling and showed as black letters).
* Tested in the arm64 VM: rubber band with autoscroll over 60 files, spring-loading a folder and dropping into it.

Not done yet: dragging to other programs, the Dock and the desktop (XDND), column view, Get Info, tabs, undo, copy progress, PDF/video thumbnails, sheets attached to the
parent window (panels are modal windows), x86_64 in the VM, the GTK and Windows file dialogs (phase 3).
