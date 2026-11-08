# Mac-Programme auf ClokeOS

Der Weg, damit Mac-Programme auf ClokeOS laufen: Darlings Mac-Frameworks werden für ClokeOS gebaut,
auf PureDarwins echtem objc4, dyld und libSystem. Hintergrund, Optionen und Messungen stehen in
`~/ClokeOS-work/upstream/appkit-machbarkeit.md`.

Stand 20.10.2026:
- **Foundation-Probe.** CoreFoundation und Foundation aus Darling, ein Prüfprogramm aus Apples SDK
  besteht in der VM 24 von 25 Prüfungen.
- **AppKit-Probe.** AppKit, Cocoa, CoreGraphics, Onyx2D, CoreText, QuartzCore, CoreImage, CoreData
  und ImageIO aus darling-cocotron. Eine x86_64-Cocoa-App aus Apples SDK (`hallo.m`) zeigt ihr
  Fenster in CloakServer, reagiert auf Maus und Tastatur und beendet sich sauber.
- **arm64 (23.10.2026).** Alles davon auch für arm64: `cfarm64.S` und `cfarm64.m` bringen Darlings
  NSInvocation und Nachrichtenweiterleitung Apples arm64-Aufrufkonvention bei, `patches/arm64/`
  korrigiert, was auf arm64 nicht übersetzt oder hängt. In der arm64-VM besteht `invprobe.m` 15 von 15
  Prüfungen (wie mit Apples Foundation), die Foundation-Probe 24 von 25, und `Hallo.app` (jetzt
  universal) läuft aus Launchpad. Einzelheiten im Haupt-README, Abschnitt „Mac-Programme mit Fenstern
  auf arm64“.
- **Im Desktop-Image.** `brand.sh` legt mit `install.sh` die Frameworks nach
  `/Library/ClokeOS/MacFrameworks` und `Hallo.app` nach `/Applications`, sobald die Darling-Quellen
  geladen sind. CloakServer erkennt Mac-Programme, deren Frameworks dort liegen, und startet sie aus
  Launchpad und Dock mit `DYLD_FRAMEWORK_PATH`.

- **CoreAudio-Shim (29.10.2026).** `coreaudio.sh` baut `CoreAudio.framework` (eigener Code, `coreaudio/CoreAudio.c`): eine
  kleine öffentliche HAL über `/dev/dsp0`, dazu `satest` (2 s Sinus). Einzelheiten in `docs/audio-stack.md`, Abschnitt 4.

```bash
macapps/machcheck.py /pfad/zu/Programm.app
```
zeigt, welche Bibliotheken und Symbole einem Mac-Programm auf ClokeOS fehlen (ohne VM). Mit
`--frameworks ~/ClokeOS-work/out/macapps/root/Library/ClokeOS/MacFrameworks` prüft es wie mit
`DYLD_FRAMEWORK_PATH` im Gast gegen die hier gebauten Frameworks.

```bash
macapps/foundation.sh
macapps/appkit.sh
```
bauen die Frameworks nach `~/ClokeOS-work/out/macapps/root/Library/ClokeOS/MacFrameworks`, erst
Foundation, dann AppKit. Mit `CLOKEOS_ARCH=arm64` davor bauen beide für arm64 nach
`~/ClokeOS-work/out/macapps-arm64/` (gegen PureDarwins arm64-Desktop-Image, dessen Sysroot
`cloakserver/build.sh` anlegt). Die Quellen braucht es vorher unter `~/ClokeOS-work/download/darling/`
(`git clone --depth 1` von darlinghq/darling-corefoundation, darlinghq/darling-foundation und
darlinghq/darling-cocotron).

```bash
clang -target x86_64-apple-macos11.0 -fobjc-arc -framework Foundation -o ~/ClokeOS-work/out/macapps/root/Library/ClokeOS/Probe/fprobe macapps/fprobe.m
```
```bash
clang -target x86_64-apple-macos11.0 -fobjc-arc -framework Cocoa -o ~/ClokeOS-work/out/macapps/root/Library/ClokeOS/Probe/hallo macapps/hallo.m
```
```bash
clang -arch x86_64 -arch arm64 -mmacosx-version-min=11.0 -fno-stack-check -fobjc-arc -framework Foundation -o invprobe macapps/invprobe.m
```
bauen das Prüfprogramm und die Test-App. `macapps/probe-image.sh` legt damit eine Testkopie des
Desktop-Images an. Danach startet `./run.sh --headless --image ~/ClokeOS-work/tmp/fprobe-desktop.img`
die VM, `./vmctl.py send 'zsh /Library/ClokeOS/Probe/probe.sh' 300` führt die Foundation-Probe aus.
Für die Test-App nach dem Start mit `./vmctl.py key ret` anmelden, dann
`./vmctl.py send 'zsh /Library/ClokeOS/Probe/hallo.sh --klick 20 > /tmp/hallo.log 2>&1 &'` und
`./vmctl.py shot hallo.png`.

Die Frameworks ersetzen nichts im System. Programme benutzen sie über
`DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks`, weil launchd, configd und Security weiter
PureDarwins CoreFoundation laden und zwei CFs in einem Prozess nicht gehen. PureDarwins eigenes
Onyx2D und CoreGraphics bleiben ebenfalls.

Dateien:
- `include/`: Ersatz für private Apple-Header.
- `syscompat.c`: Ersatz für Funktionen, die PureDarwins libSystem fehlen.
- `cfcompat.m`, `fndcompat.m`: Literal-Klassen, die Apples clang voraussetzt, und kleine Lücken in
  Darling.
- `appkit/x11present.m`: AppKit-Fenster ohne OpenGL auf den Bildschirm (XShmPutImage, sonst
  XPutImage). `CLOKE_X11_DEBUG=1` zeigt Weg und Dauer, `CLOKE_NO_XSHM=1` erzwingt XPutImage.
- `appkit/clokemenu.m`: Hauptmenü in CloakServers globaler Menüleiste statt im Fenster (Eigenschaft
  `_CLOKE_MENU`, Rückmeldung `_CLOKE_MENU_ACTIVATE`), ohne Eingriff in Darlings Quellen.
  `CLOKE_MENU_IM_FENSTER=1` zeigt das Menü wieder im Fenster.
- `appkit/imageio.m`: ImageIO (CGImageSource, CGImageDestination) über Onyx2D.
- `appkit/xkbrules.c`: die eine Funktion aus libxkbfile, die PureDarwin fehlt.
- `appkit/sysstubs.c`: Platzhalter für OpenGL, IOKit und LaunchServices, nicht exportiert.
- `appkit/include/`: Ersatz für Darlings private Header und schlankere Apple-Header.
- `patches/`: Korrekturen an Darling-Fehlern und Anpassungen an ClokeOS (sed).
- `fndlayout.m`, `appkit/autolayout.m`, `appkit/alsolver.c`, `appkit/gridview.m`: Auto Layout (constraints, anchors, visual format
  language, solver, intrinsic sizes, `fittingSize`, windows growing to their content, `NSGridView`); see docs/logic-launch.md round 61.
- `layoutprobe.m`: Auto Layout probe, same output on the Mac and in ClokeOS (built-in cases, or a nib: `layoutprobe x.nib`).
- `fprobe.m`, `hallo.m`: Prüfprogramm (Foundation) und Test-App (Cocoa), `hallo-icon.svg` ihr Icon.
- `install.sh`: Frameworks und `Hallo.app` in die Stage des Desktop-Images (von `brand.sh`), für die
  Architektur des Images; `Hallo.app` ist universal.
- `cfarm64.S`, `cfarm64.m`: arm64-Teil von NSMethodSignature, NSInvocation und der Weiterleitung
  (statt Darlings `CFForwardingPrep.S` und `NSInvoke-x86.S`), eingehängt über
  `patches/arm64/darling-corefoundation/`.
- `arm64abi.py`: prüft `cfarm64.*` auf dem Mac gegen clang (`python3 macapps/arm64abi.py [anzahl] [seed]`;
  zufällige Signaturen, Weiterleitung und Aufruf, Ausnahmen). Der Mac ist selbst arm64, clang setzt
  dort Apples Konvention um.
- `invprobe.m`: NSInvocation-Probe (Weiterleitung, NSProxy, NSUndoManager, KVO, KVC mit Strukturen,
  CFEqual, NSPredicate, NSTimer). Auf arm64 wie Apple; auf x86_64 scheitert sie an Darlings eigener
  x86-Fassung, die float/double-Rückgaben an der falschen Stelle ablegt und Strukturen aus
  float/double wie Ganzzahlen behandelt.
- Programme für arm64 mit `-fno-stack-check` bauen, wenn sie Funktionen mit mehr als 4 KiB Stack
  haben: PureDarwins libSystem fehlt `___chkstk_darwin`.
