# Window title bars (CloakServer, all app types)

Goal: title bars that look and behave like macOS. Modern Mac windows have no separate title bar: the content (often a toolbar)
runs to the top edge and the traffic lights sit over it. Classic windows keep a slim bar.

## Reference: macOS 27 on the host Mac (measured, `NSWindow` + `cacheDisplayInRect:`)

| Window | Title bar | Traffic lights (centres from top-left) |
|---|---|---|
| Titled | 32 pt, separator line below, title 13 pt bold, **left-aligned** at x = 85 | x 16 / 39 / 62, y 16, diameter 14 |
| FullSizeContentView + transparent + title hidden | none drawn, content under it | same, y 16 |
| Toolbar unified / unifiedCompact | 66 / 40 pt (empty toolbar) | (26, 26) / (19, 20) |
| Utility panel | 24 pt | 10 pt lights, x 13 / 30 / 47 |

NSAlert windows have no title bar and no lights. Buttons are grey (disabled) when the style mask lacks Closable,
Miniaturizable or Resizable.

## Protocol: `_CLOKE_TITLEBAR` (CARDINAL[6] on the client window)

| Index | Meaning |
|---|---|
| 0 | style: 0 = CloakServer draws a bar, 1 = overlay (app draws the top area, CloakServer draws only edge, corners, shadow and the lights over it), 2 = own (app draws everything, CloakServer adds no lights) |
| 1 | height of the area the app draws at the top (lights centred vertically); 0 = 32 |
| 2 | info flags: 1 FullSizeContentView, 2 transparent, 4 title hidden (no title in a style-0 bar), 8 toolbar visible, 16 unified title and toolbar (style mask 0x1000) |
| 3 | x of the first light's centre; 0 = 16 |
| 4 | bits 0–2 hidden lights (close, miniaturize, zoom); bits 3–5 the same lights disabled (grey, no action) |
| 5 | y of the lights' centre from the top; 0 = middle of the height |

Any program can set it; CloakServer re-reads it on `PropertyNotify` and keeps the frame's top edge in place.
The NT look ignores it (always a bar).

## Who sets what

* **Mac apps (Cocotron AppKit, X11 backend)**: `macapps/appkit/titlebar.m` adds `titlebarAppearsTransparent`, `titleVisibility`,
  `toolbarStyle` to NSWindow (Cocotron had none), implements `standardWindowButton:` (placeholders with macOS 27 frames; `setHidden:`
  and moves go to the property) and republishes on `setStyleMask:`, `setToolbar:`, `NSToolbar setVisible:`. Overlay when
  FullSizeContentView is set, or a visible toolbar with hidden title / unified style (height = toolbar height, lights from x = 26,
  Cocotron's toolbar items start at x = 86 with `patches/…/NSToolbarView.m.sed`, optional at run time); NSAlert = own.
  `X11Window initWithDelegate:` publishes before the window is mapped (`patches/…/X11Window.m.sed`). Without a CloakServer bar the
  app moves its window itself: a left click in the top area on a view whose `mouseDownCanMoveWindow` is YES (Apple's default:
  not opaque; NSControl, NSText, scroll and table views NO) sends `_NET_WM_MOVERESIZE`; a double click zooms.
  `CLOKE_TRACE=1` logs every publish as `[clokeos-titlebar]`.
* **Linux / GTK with header bars (CSD)**: XSETTINGS `Gtk/DecorationLayout = close,minimize,maximize:` puts the buttons on the left;
  the ClokeOS GTK theme draws them as 14 px traffic lights 23 px apart and removes GTK's shadow margin. Every normal window that asks
  for no decorations via `_MOTIF_WM_HINTS` (and is not fullscreen, and has no non-zero `_GTK_FRAME_EXTENTS`) gets edge, rounded corners
  and shadow from CloakServer (style 2).
* **Linux / X11 without CSD** (xterm, xfce4-terminal, …): classic CloakServer bar as measured above.
* **Windows apps (Raven)**: Raven draws its own Windows frame (`native/desktop/frame.cpp`) and sets `_MOTIF_WM_HINTS` = no
  decorations, so it now gets style-2 edge/corners/shadow, but still a Windows caption with buttons on the right. Making it
  macOS-like needs Raven to draw a Mac-style caption (no icon, no right-hand buttons) and set `_CLOKE_TITLEBAR` = 1 with its caption
  height — not done here (the Raven port is changed by another session).

* **Transparent / unified title bars without full-size content** (flags 2 or 16; Logic Pro's project window: mask 0x110f, transparent):
  CloakServer keeps its title row but paints it in the colour of the app's top pixel row, without separator, title centred; with a
  toolbar below the title is centred too. This is how Logic Pro looks on macOS 27 (compared with Logic on the Mac: a title row with
  lights and centred title, the control bar below, one grey surface).
* **Apps with their own top area** like the Claude desktop app (content to the top edge, lights vertically centred in a taller header,
  inset) are style 1 with `[1]` header height and `[3]`/`[5]` light position, like Electron's `trafficLightPosition`.

CloakServer's own apps with toolbar in the title bar (Fundus, Search, Calendar, Notes, Activity Monitor: `uni`) keep their 38 px area.

## Drawing the lights over an app

With the compositor, `comp.c` calls `cs_window_overlay()` after each frame; CloakServer paints the anti-aliased lights over the app
there. Input goes to a small InputOnly child of the frame over the lights. Without compositor that child is an InputOutput window shaped
to the three circles. CloakServer now also handles `_NET_WM_MOVERESIZE` (move 8, size 4, cancel 11) and announces it in `_NET_SUPPORTED`.

## Global menu bar

CloakServer adds its own "Window" menu only when the app's menus (`_CLOKE_MENU`, GTK, Raven) have no top-level "Window"/"Fenster"
(mnemonic `_`/`&` ignored). Logic Pro showed "… Mix View Window Window" before; now one "Window" (its own).

## Tested (arm64 VM, 2026-11-07)

* Logic Pro project window (`LOGIC_ARGS="-NSOpen /var/root/Jump.logicx"`, two Continue clicks): before `screenshots/titlebar-logic-before-blend.png`
  (white bar with line, also shows the single Window menu), after `screenshots/titlebar-logic-tracks.png` (title row in the control bar's grey).

* Logic Pro 12.3: alert "Error initializing CoreMIDI" without title bar; splash (mask 0x8001, title hidden, transparent) with grey lights
  over its content. Before/after: `screenshots/titlebar-before-logic.png`, `screenshots/titlebar-after-logic.png`.
* xfce4-terminal: 32 px bar, lights, left-aligned bold title. GTK header bar (`gtk-builder-tool preview`): traffic lights from the theme,
  rounded corners and shadow. `screenshots/titlebar-after-linux.png`.

Not tested: toolbars
(Cocotron NSToolbar items are not inset for the lights), window moving by dragging the top area of a Mac app, hover/click on overlay
lights, without compositor, x86_64, Raven windows, the NT look. A GTK header-bar window still shows a thin inner border from GTK.
Hand-starting GTK apps in the VM needs SIGSYS ignored (`trap '' SYS`) and `DYLD_INSERT_LIBRARIES=/lib/libXcursor.1.dylib`, as the session does.
