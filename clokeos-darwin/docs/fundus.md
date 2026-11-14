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

Not done yet: drag and drop, column view, Get Info, tabs, undo, copy progress, PDF/video thumbnails, sheets attached to the
parent window (panels are modal windows), x86_64 in the VM, the GTK and Windows file dialogs (phase 3).
