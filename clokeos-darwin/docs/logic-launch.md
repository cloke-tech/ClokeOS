# Logic Pro on ClokeOS: first launch attempt (arm64)

Date 2026-10-30. Image: `brand.sh` from main with `CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop` (ClokeOS 2.7, all packages of
this week: libcompat, CoreAudio shim, Accelerate, Carbon shim, pkginstall), root file system grown from 3 to 5.1 GB.
Logic's `Contents/MacOS`, `Contents/Frameworks` (16,301 files, 1.5 GB) and `Info.plist` were written into the image with
`debugfs` from the host copy of `/Applications/Logic Pro.app` (the full tree is not needed to launch; the native installer
from `docs/logic-installer.md` already proved the full install). Nothing of Logic is in the repository. VM: arm64, HVF.

## Result

**The launcher does not get past dyld.** It never reaches `NSApplicationMain`.

| Round | Command (in the guest) | First error | Fix | Result |
|---|---|---|---|---|
| 1 | `DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks DYLD_PRINT_LIBRARIES=1 "./Logic Pro"` | `Library not loaded: /System/Library/Frameworks/DeveloperToolsSupport.framework/Versions/A/DeveloperToolsSupport` (needed by `Logic Pro` itself) | empty stub frameworks (see "Round 2") | next round |
| 2 | `DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks "./Logic Pro"` | `Symbol not found: _$s10Foundation12CharacterSetV14urlPathAllowedACvgZ`, referenced from `MAAccessibility.framework` (built for Mac OS X 15.6), expected in our Foundation | none: Swift overlay symbol | stop |

What dyld did load before failing: the launcher, `LogicAppFramework`, `MACore`, `Logic.framework`, our AppKit, Foundation,
CoreFoundation, libc++, libSystem. So `@rpath` resolution, chained fixups and the `DYLD_FRAMEWORK_PATH` redirect to
`/Library/ClokeOS/MacFrameworks` work for Logic's own frameworks. dyld reports one missing library at a time, so the full
picture was computed statically (`macapps/machcheck.py "…/Logic Pro" --arch arm64 --deep` against this image plus
the MacFrameworks of this build): **128 libraries are in the load closure, 28 are missing, and 1,869 symbols
are missing from the libraries that exist** (about 1,070 of them printed in full; 635 of those are Swift-mangled).

### The 28 missing libraries

| Group | Libraries | Cause |
|---|---|---|
| Swift runtime and overlays (12) | `/usr/lib/swift/libswiftCore`, `_Concurrency`, `_StringProcessing`, `_errno`, `libswiftCoreFoundation`, `Darwin`, `Dispatch`, `ObjectiveC`, `Observation`, `os`, `UniformTypeIdentifiers`, `AVFoundation`, `Accelerate` | `libswiftCore`/`_Concurrency` were built for arm64 by the Swift session (see the Swift study) but are not part of the image; the overlays (Dispatch, os, ObjectiveC, CoreFoundation, Darwin, Observation) are not built at all |
| SwiftUI and friends (6) | `SwiftUI`, `Combine`, `Charts`, `CoreTransferable`, `RealityKit`, `RealityFoundation`, `_RealityKit_SwiftUI`, `DeveloperToolsSupport` | closed Apple frameworks, no implementation; the launcher itself links `DeveloperToolsSupport` (an autolink of SwiftUI) |
| Cheap (7) | `AudioToolbox`, `AudioUnit`, `CoreAudioKit`, `ExceptionHandling`, `Quartz`, `ServiceManagement`, `libAppleArchive` | `AudioToolbox` exists in the repo (`macapps/audiotoolbox.sh`) but my `brand.sh` run did not stage it; `AudioUnit`, `CoreAudioKit`, `Quartz`, `ExceptionHandling`, `ServiceManagement` can be stub or umbrella frameworks like the others |

### The missing symbols (inside libraries that do load)

Summed over the listed libraries: Foundation 1,366 (almost all Swift-mangled `_$s10Foundation…`, the Darwin Foundation overlay),
CoreServices 166 (`MDQuery`/`MDItem` Spotlight, `UCKeyTranslate`), CoreGraphics 70 (Swift overlay + `CGEvent*`), AppKit 65,
QuartzCore 36, ApplicationServices 25 (`PMPrint*`), Carbon 18 (`TIS*`, `HIGetMousePosition`), Accelerate 16, CoreML 16,
CoreAudio 14, CoreData 13, Network 12. The C/ObjC part is the cheap part; the Swift part cannot be fixed by stubs.

## Round 2: the cheap fixes

`macapps/stubgen` now also generates empty stubs (install name, version 1000.0.0, no symbols) for libraries that are only
loaded or only used from Swift: `DeveloperToolsSupport`, `ExceptionHandling`, `ServiceManagement`, `Quartz`, `AudioUnit`,
`CoreAudioKit`, `CoreTransferable`, `Charts`, `RealityKit`, `RealityFoundation`, `_RealityKit_SwiftUI`, `Combine`, `SwiftUI`,
`/usr/lib/libAppleArchive.dylib`, and the flat `/usr/lib/swift/libswift{Core,_Concurrency,_StringProcessing,_errno,Dispatch,
ObjectiveC,CoreFoundation,Darwin,Observation,os,UniformTypeIdentifiers,AVFoundation,Accelerate}.dylib`; `macapps/install.sh`
stages them and builds `audiotoolbox.sh`. New image (same recipe, root grown to 5.1 GB, Logic code written with `debugfs`).

Result: **dyld now loads all 128 libraries of the closure** (the 28 missing ones are present as stubs or the real AudioToolbox)
and fails at the first *bind*, the symbol above. dyld stops at the first missing symbol, so the length of the list is the
`machcheck.py` figure: 1,869 symbols, most of them Swift-mangled (Foundation overlay, CoreGraphics overlay, SwiftUI, Combine).

Not a real Swift runtime: the `libswiftCore`/`_Concurrency` in the image are empty stubs. The real ones need
`swift/build.sh` (a partial clone of swiftlang/swift, about 110 MB, and about 15 minutes per architecture); I did not start
that download without asking. Even with it, the Foundation overlay and SwiftUI are still missing.

## Round 3: real Swift runtime, and x86_64

Approved download: swiftlang/swift tag swift-6.4.0-RELEASE (partial clone, 159 MB with the build trees), built with
`swift/build.sh` for arm64 and x86_64 (about 25 minutes in all). `macapps/install.sh` now takes `$WORK/swiftsrc/run-<arch>/libswiftCore.dylib`
and `libswift_Concurrency.dylib` instead of the empty stubs and builds the `/usr/lib/system/libdispatch.dylib` shim
(`swift/dispatch-shim.c`; linked with `ld` and a tbd stand-in because the macOS 27 SDK has no x86_64 slice of the system libraries).
`macapps/logicimage.sh` builds the whole test image (brand.sh, grown root, Logic code), `CLOKEOS_ARCH=x86_64` for x86_64.

| Round | Arch | First error |
|---|---|---|
| 3 | arm64 (HVF) | the same `Symbol not found: _$s10Foundation12CharacterSetV14urlPathAllowedACvgZ` from `MAAccessibility` (the 7.6 MB `libswiftCore` is in the image, loads fine) |
| 3 | x86_64 (TCG, native, no translation) | the same symbol, same message (7.8 MB `libswiftCore`); then `EXC_CORPSE_NOTIFY` for the dying launcher |

So both slices of Logic's launcher load all 128 libraries, the Swift runtime is real, and both stop on the **Foundation Swift
overlay**: the module `Foundation` as seen from Swift (`CharacterSet`, `Notification`, `URLResourceValues`, `Data`, `Date`, `JSONEncoder` … about
1,366 symbols across the 98 binaries). dyld reports one symbol at a time, so which other overlays follow (CoreGraphics 70, SwiftUI,
Combine, simd, os, Dispatch) is known only from the static list above.

## Round 4: Swift overlay and C/ObjC supplements, past dyld (arm64)

New tool `macapps/stubgen/swiftov.py` (called by `macapps/install.sh`): for every library Logic imports from, it compares the imported
symbols with what our build exports. What is missing goes into a supplement dylib `/usr/lib/sov/<n>.dylib`, and the library gets an
`LC_REEXPORT_DYLIB` to it, added by patching the Mach-O header in the space left by the linker (56 to 112 bytes; hence the short
file names). Two-level namespaces keep working: dyld finds the symbol "in" Foundation, AppKit, SwiftUI …

* **Swift symbols** (`_$s…`, 424 in Foundation, 2,102 in SwiftUI, 179 Combine, …): a label with a trap instruction (`brk #1`).
  Loading works; the first real call traps. These are placeholders, not an overlay.
* **C functions, constants, ObjC classes**: the existing stubgen classifier (`emit`) from the SDK headers; instance variable offsets
  (`OBJC_IVAR_$_NSBox._bFlags`, `NSCell._cFlags`, `NSButtonCell._bcFlags`, `NSTableView._tableColumns`): placeholder value 16.
* **IOKit**: PureDarwin's `libIOKitCF.dylib` (IOKit is a symlink to it) gets a plain-assembly supplement (functions return 0). Mixing in
  ObjC/CoreFoundation there killed launchd ("Symbol not found: _OBJC_CLASS_$_NSObject", kernel panic at boot); that is why
  `plain_dylib` has no dependency but libSystem.
* **CoreServices and Carbon**: `macapps/carbon.sh` now also puts the 140 CoreServices/Carbon symbols that Logic imports and nothing provides
  (Spotlight `MDQuery*`, Apple Events, HIToolbox `TIS*`, `LMGetKbdType` …) into `libclokeos_carbon` as stubs returning 0.
* **libswiftCore**: 3 symbols are missing from the 6.4 build (`$ss5SIMD2VMn`, `$ss5SIMD3VMn`, `…_ExpressibleByBuiltinFloatLiteralP…`);
  the real `libswiftCore` is patched the same way. `libswift_Concurrency` lacks nothing.

Rounds (each a full image: `macapps/logicimage.sh`, about 10 minutes):

| Round | First error | Fix |
|---|---|---|
| 4 | `Symbol not found: _NSFontDescriptorSystemDesignRounded` (MACore, expected in AppKit) | C/ObjC supplements |
| 5 | boot panic: launchd, `libIOKitCF` supplement pulls NSObject | plain assembly for IOKit |
| 6 | `_LMGetKbdType` expected in Carbon | CoreServices/Carbon stubs in `libclokeos_carbon` |
| 7 | `_OBJC_IVAR_$_NSBox._bFlags` (MAToolKit, expected in AppKit) | ivar placeholders |
| 8 | `_$ss5SIMD2VMn` expected in libswiftCore | trap labels for the 3 missing symbols |
| 9 | none from dyld: **the process dies with signal 10 (SIGBUS, exit status 138)** | not fixed |

**State at the end of this session: on arm64 Logic's launcher passes all of dyld** (every library loads, every symbol binds, `DYLD_PRINT_INITIALIZERS=1`
prints `calling initializer function … in /usr/lib/libSystem.B.dylib` as its last line) **and then crashes with SIGBUS** while running
initializers. It does not reach `NSApplicationMain`. Cause unknown: no debugger or crash report in the image; candidates are a call into a
placeholder data symbol (a stubbed constant or ivar used as code or as an offset), a trap in a supplement, or an initializer of a C++/Swift
library that depends on real overlay code. Not analysed further.

Next step: get a crash location (an in-image debugger or a `SIGBUS` handler that prints `pc` and the image of `pc` via `dladdr`, loaded with
`DYLD_INSERT_LIBRARIES`), then replace the offending placeholder with real code. x86_64: the same tooling builds the x86_64 supplement, but the
x86_64 slice was not re-run after round 3.

## Round 10: where the SIGBUS is (not found yet)

Findings, all in the arm64 VM with the image of round 9 (plus `macapps/crashpc.c`, a crash reporter loaded with `DYLD_INSERT_LIBRARIES`):

* The crash is **before any `+load` and before `libcrashpc`'s constructor**: with `OBJC_PRINT_CLASS_SETUP=YES` the last line is
  `CLASS: found 7549 classes during launch`, then the duplicate-class warnings for `__NSCFType`, `__NSCFNumber`, `__NSCFBoolean`
  (PureDarwin's `libCoreFoundation` against Darling's CoreFoundation, the same as in every Mac program here), then SIGBUS. So it
  happens inside `libobjc`'s `read_images` (called from libSystem's initializer through `_objc_init`), while classes are read.
  A handler cannot be installed earlier than libSystem's initializer; `crashpc` therefore stays silent.
* Duplicate classes from my own supplements were a real bug and are fixed: `swiftov.py` now follows `LC_REEXPORT_DYLIB` chains when it decides what a
  library lacks (Foundation re-exports CoreFoundation, which has `NSError`, `NSCharacterSet`, …), and the stub root classes get unique names
  (`ClokeStubRoot_<lib>_sov`). The crash did not go away.
* **Not the cause:** the Logic binaries (identical SHA-1 in the image and on the host for Logic, MACore, LogicAppFramework); relative
  method lists (a test program built with `-objc_relative_method_lists` runs on PureDarwin's libobjc exactly like one without: both print `answer=42`).
* **Per-framework `dlopen`** (`dlp`, one process each, `DYLD_FRAMEWORK_PATH` set): these load fine: `ChordsKit`, `EmbeddedRemoteConfiguration`,
  `InternalCollectionsUtilities`, `MAAccessibility`, `MAContentDownloading`, `MAGenerativeInstrumentsCore`, `MALogicLegacySong`, `OMF`
  (so Swift frameworks load at least in part). These **crash silently** (no output, process gone): `MASwiftUtilities`, `MAToolKit`, `MAToolKitHighLevel`,
  `MAGUI`, `MALoopBrowser`, `MASearchKit`, `MAVectorUIKit`, `MAResources*`. The others answer `image not loaded`, which is only the missing `@rpath` of a
  program without `LC_RPATH` (not a Logic problem).
* `MASwiftUtilities` is the smallest crasher (5 pure Swift classes whose superclass is `_OBJC_CLASS_$__TtCs12_SwiftObject`, imports SwiftUI/CoreGraphics/Dispatch/
  Observation). Best next experiment: build a Swift test with `final class X {}` (no `NSObject`) against our `libswiftCore` and `dlopen` it. The earlier Swift tests only
  covered `NSObject` subclasses and the generic `Box<T>` of `hello2.swift`.

### Round 11: pure Swift classes are fine

A test dylib with a `final class`, a `class` with a `final` subclass (all rooted at `_OBJC_CLASS_$__TtCs12_SwiftObject`, like Logic's
`MASwiftUtilities`), built with the host Swift 6.4 for `arm64-apple-macos11.0` **and** `macos15.6` against our `libswiftCore`, loads with `dlopen`
and runs on PureDarwin's `libobjc` (`pure_test=83` = 3*14 + 41, both variants; in a ClokeOS desktop image built with `CLOKE_NO_MACAPPS=1`, driver
`dlopen` + `dlsym`). So neither plain Swift classes nor the 15.6 deployment target are the cause. What `MASwiftUtilities` has beyond that
(sections): `__objc_protolist`/`__objc_protorefs` (ObjC protocol list), `__objc_stubs` (selector stubs), `__swift5_*` metadata for protocols and
associated types, imports of SwiftUI/CoreGraphics/Observation. Next candidates, in order: (1) a Swift `@objc protocol` and a class conforming to it,
(2) `-fobjc-msgsend-selector-stubs`, (3) a build of PureDarwin's libobjc with `_objc_inform` prints around `read_images` (its source is in the
PDSRC tarball; no debugger exists in the image).

### Round 12: the SIGBUS is found and fixed (libobjc and `__DATA_CONST`)

Method: PureDarwin's libobjc (objc4-781, `src/Libraries/objc4` in the PDSRC tarball) was built from source with the host clang
(`libobjc/build.sh`, recipe from `nix/pkgs/apple/libobjc.nix`; one source change for the newer libc++: `std::binary_function` removed) and
`write(2)` trace prints around the stages of `_read_images`. The last line before the crash: `stage done discover protocols`, so the fault was in
the next stage, **fix up @protocol references** (`remapProtocolRef`).

Cause: binaries built with a newer Xcode (Logic Pro: SDK 26) put `__objc_protorefs` (and class refs) in `__DATA_CONST`. dyld makes that segment read-only after
binding; this old libobjc rewrites the references when the same protocol exists in several images (`*protoref = newproto`) — a write to a read-only page, which is
SIGBUS on arm64 Darwin. Fix (`libobjc/dataconst.patch`): `mprotect` the page read-write before writing (`remapProtocolRef`, `remapClassRef`).
`macapps/logicimage.sh` builds the image with `CLOKE_LIBOBJC=1`, which makes `macapps/install.sh` stage the patched `/usr/lib/libobjc.A.dylib`.

Result (arm64): **`_read_images` completes for all 7,549 classes and Logic's launcher runs its own code.** It now ends with an Objective-C exception from our Foundation:

```
libc++abi: terminating due to uncaught exception of type NSException
Logic Pro[…]: Terminating app due to uncaught exception 'NSException', reason: '__NSCFNumber objCType requires a subclass implementation'
```

That is the duplicate `__NSCFNumber` (also `__NSCFType`, `__NSCFBoolean`): PureDarwin's `libCoreFoundation` and Darling's CoreFoundation both define the
class, objc picks one ("One of the two will be used"), and `NSNumber` ends up on the bridged class whose Darling `objCType` is abstract. A Mac program sees
this when it calls `-[NSNumber objCType]` (Logic does so during startup). Next step: make the two CoreFoundations agree on one `__NSCFNumber`
(for example by not loading PureDarwin's `libCoreFoundation` classes in programs that use `DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks`), or
give Darling's `__NSCFNumber` an `objCType`.

### Round 13: Logic's launcher reaches NSApplicationMain (arm64)

The `__NSCFNumber objCType` exception came from running with two CoreFoundations: PureDarwin's `/usr/lib/libCoreFoundation.dylib` and
`libFoundation.dylib` (loaded by system libraries) define the same classes as Darling's frameworks, and objc keeps the first one found by name. Fix for
programs (not system daemons): `DYLD_LIBRARY_PATH=/Library/ClokeOS/MacLibs`, a folder with `libCoreFoundation.dylib` and `libFoundation.dylib` symlinked to Darling's
frameworks (staged by `macapps/install.sh`). dyld then loads only Darling's CoreFoundation and Foundation (verified with `DYLD_PRINT_LIBRARIES`; the
PureDarwin ones are gone from the list) and AppKit's X11 backend (glib, gobject) comes up.

With the crash reporter (`DYLD_INSERT_LIBRARIES=/usr/lib/libcrashpc.dylib`) the next stop is readable:

```
DISPLAY=:0 DYLD_INSERT_LIBRARIES=/usr/lib/libcrashpc.dylib DYLD_LIBRARY_PATH=/Library/ClokeOS/MacLibs DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks "Logic Pro"
[crashpc] Signal 0xb (SIGSEGV), fault address 0x10
  pc  libobjc.A.dylib objc_msgSend +0x14
  lr  AppKit -[NSDocument initWithType:error:] +0x24
  bt  Logic.framework/Logic +0xb47f9c
  bt  AppKit -[NSDocumentController makeUntitledDocumentOfType:error:]
  bt  AppKit -[NSDocumentController openUntitledDocumentAndDisplay:error:]
  bt  AppKit -[NSApplication finishLaunching]
  bt  AppKit -[NSApplication run]
  bt  AppKit NSApplicationMain
  bt  Logic Pro +0x1070                      (the launcher)
```

**This is step 9 of the test ladder (`logic-pro-gap.md` section 6): the launcher reaches `NSApplicationMain`** — Logic's `CLgApplication` is running, AppKit's X11 backend is
initialised on the ClokeOS desktop (the desktop session was logged in, `DISPLAY=:0`), and `finishLaunching` asks the document controller for an untitled
document, which calls Logic's own document subclass and then Cocotron's `-[NSDocument initWithType:error:]`, where a message goes to nil (fault address 0x10).
No window appears yet. Next: find which object in `-[NSDocument initWithType:error:]` is nil (probably a class or property the Cocotron AppKit lacks, for
which Logic's override of `makeUntitledDocumentOfType:` passes through), then continue the same loop (crash location, smallest fix).

### Round 14: the NSDocument bug, NSBundle image methods, Logic's own startup messages

Register dump of the previous crash (`crashpc` now prints x0..x3 and the selector for `objc_msgSend` faults): receiver of `-setFileType:` was a heap object with isa 0, i.e. freed.
Cocotron's `-[NSDocument initWithType:error:]` calls `[self init];` and drops the result; Logic's document class returns another object from `-init`. All five such
places in `AppKit/NSDocument.m` now read `self = [self init];` (`macapps/patches/darling-cocotron/AppKit/NSDocument.m.sed`). Document creation now fails inside Logic itself instead of crashing,
AppKit shows the error through `-[NSResponder presentError:]` → `runModalForWindow:` and Logic's code runs there (dispatch_once, a worker); this is code of the
product, executing.

Next crash: `-[NSBundle URLForImageResource:]: unrecognized selector` (Cocotron has no NSBundle image methods). `macapps/appkit/nsbundle_images.m` (X11 backend,
`URLForImageResource:`, `pathForImageResource:`, `imageForResource:`) adds them. `macapps/logicimage.sh` now also installs `Contents/Resources` (1.7 GB) and grows the
root by 4.2 GB. With that, the messages printed by Logic's own startup in the arm64 VM are:

```
Logic Pro: Unable to load main nib file MainMenu
CALLED: O2ColorSpaceCreateWithName                       (our CoreGraphics/Onyx2D: not implemented)
Logic Pro: convertFont:toHaveTrait: failed, San Francisco 2
Terminating app due to uncaught exception 'NSException', reason: '-[NSURLComponents initWithURL:resolvingAgainstBaseURL:]: unrecognized selector'
```

State at the end: **no window**. The four items are the next wall, each a small-to-medium missing piece:
1. `NSURLComponents` in Darling's Foundation is an empty 5-line file (`src/NSURLComponents.m`); a real one (scheme, host, port, path, query, queryItems/`NSURLQueryItem`, `-URL`, `-string`) is about 200 lines on top of CFURL.
2. `MainMenu.nib` is not found/loaded although `Contents/Resources` is installed: Logic's nib is the compiled Apple format (probably in `Base.lproj`/`en.lproj`); Cocotron's NSNib/NSBundle must be checked for the
   lookup (the `.lproj` search) and for the `keyedobjects` archive version.
3. `O2ColorSpaceCreateWithName` (Onyx2D) must create the named color spaces (sRGB, generic RGB/gray, display P3).
4. Fonts: "San Francisco" does not exist; the system font must map to a font on ClokeOS.

### Round 15: a Logic Pro window on the ClokeOS desktop (arm64)

Added: a real `NSURLComponents` (+ `NSString -stringByAddingPercentEncodingWithAllowedCharacters:`, the six `NSCharacterSet URL…AllowedCharacterSet`
class methods; none of them existed in Darling's Foundation): `macapps/fndurlcomponents.m`, the empty Darling implementation is cut out by
`patches/darling-foundation/src/NSURLComponents.m.sed`. And `macapps/appkit/lenient.m` (X11 backend): with `CLOKE_LENIENT=1` a message nobody implements goes to a
shared sink object that returns nil/0 and logs `[clokeos-lenient] -[Class selector]` once, so Logic keeps running and reports its missing methods instead of aborting
at the first. For the test image `macapps/logicimage.sh` also links `Contents/Resources/MainMenu.nib` to the nib inside `Logic.framework` (Info.plist asks for
`NSMainNibFile = MainMenu` in the main bundle, the file lives in the framework).

Result: **Logic Pro is running as a process on ClokeOS** (`ps`: `./Logic Pro`, 80 % CPU in AppKit's modal loop, up for minutes) and has a **window on the desktop**: its menu bar
shows "Logic Pro / File / Window", the Dock shows the running app, and an alert panel (warning icon, OK button, empty message) titled "Logic Pro" is on screen, presented through
`-[NSResponder presentError:]` because creating the untitled document failed. This is the product's own error path, with its strings missing (the localized nib/strings are not loaded).
A click on OK did not close it in the one attempt (the modal loop was busy). Screenshot: `~/Desktop/ClokeOS-audio-proof/logic-on-clokeos-alert.png` (not in the repository).

Messages of the run: `-[NSHelpManager registerBooksInBundle:]` (the only lenient selector logged), `TIFF unsupported, compression 8` (ImageIO: Deflate-compressed TIFFs),
`-[NSTextView setGrammarCheckingEnabled:] unimplemented`, `-[NSTextView setEnabledTextCheckingTypes:] unimplemented`, `convertFont:toHaveTrait: failed, San Francisco 2`,
`Unable to load main nib file MainMenu` (also with the link; not yet understood: compiled Apple nib format, or the lookup).

Not a rendering of anything, not a project: no `Jump.logicx` was opened, no audio engine was started, SwiftUI parts still run on trap stubs.

### Round 16: Logic runs to `didFinishLaunching` and then idles (no delegate, no window)

Findings (arm64 VM, `CLOKE_TRACE=1`, `CLOKE_LENIENT=1`, `CLOKE_NO_UNTITLED=1`):

* `MainMenu.nib` is Apple's **NIBArchive** format (`NIBArchive` header), not a keyed archive: Cocotron cannot load it, which is the real cause of `Unable to load main nib file MainMenu`.
  `macapps/nibarchive.py` parses it (format read off the file; documented in the script). Content: `NSIBObjectData` with File's Owner = `NSCustomObject` of class `CLgApplication`
  and a template menu (About/Hide/Quit connected to File's Owner). **No delegate** is in it.
* At `NSApplicationWillFinishLaunchingNotification` Logic's `NSApp` (`CLgApplication`) has **no delegate**, and no main menu (`[NSApp mainMenu] == nil`) at `DidFinishLaunching`.
  So Cocotron's `-finishLaunching` falls back to opening an untitled document, Logic's document factory returns nil, and the empty alert (round 15) appears.
  `macapps/patches/darling-cocotron/AppKit/NSApplication.m.sed`: `CLOKE_NO_UNTITLED=1` skips that fallback.
* With it: `willFinishLaunching` and `didFinishLaunching` are both posted, and the process then **sits idle in its run loop** (state S, 0 % CPU, 80 MB), with **no window and no error**.
  Where Logic would normally create its delegate/UI: `Logic.framework` contains `appManager_applicationDidFinishLaunching`, `appManager_applicationWillFinishLaunching`,
  `applicationShouldOpenUntitledFile:` and the Swift classes `LogicAppFramework.AppFactory`, `ProjectFactory`, `CLgDocumentControllerObserver`; how the delegate gets attached
  on macOS (probably through `AppFactory` called from the launcher or from the nib's File's Owner machinery) was not found. Next step: disassemble the launcher's `main`
  and `CLgApplication`'s `+initialize`/`-init` (`otool -tV`, `objdump`), or log `-[NSApplication setDelegate:]` calls with the trace helper.

### Round 17: Logic's startup chain runs, the first Swift overlay trap fires

Found with `macapps/logictrace.c` (interposes `SetSetupAppFactoryCB`/`setupAppFactorySharedInstance` and logs `-[NSApplication setDelegate:]`) and a disassembly of the launcher
(`otool -tV`): the launcher calls `SetSetupAppFactoryCB(cb)` (Logic.framework), then `NSApplicationMain`. Logic attaches its delegate (`CLgAppManager`) and builds its factories in
**`-[CLgApplication awakeFromNib]`**, which only runs after the main nib has loaded. Our AppKit cannot load the NIBArchive nib, so it never ran. `macapps/patches/darling-cocotron/AppKit/NSApplication.m.sed`:
`CLOKE_AWAKE_APP=1` sends `-awakeFromNib` to `NSApp` after the failed load. With it (arm64 VM, `CLOKE_AWAKE_APP=1 CLOKE_NO_UNTITLED=1 CLOKE_LENIENT=1 CLOKE_TRACE=1`):

```
[clogic] -[CLgApplication setDelegate:] CLgAppManager
[clokeos-trace] willFinishLaunching: NSApp=CLgApplication delegate=CLgAppManager shouldOpenUntitled=1
[clokeos-lenient] -[CLgApplication setAppearance:]        (missing in Cocotron; no-op)
[clokeos-lenient] -[NSHelpManager registerBooksInBundle:]
```

and the notification handlers of `NSApplicationWillFinishLaunchingNotification` run Logic's code. Four bugs of mine/ours were found on the way and fixed:
* `NSAllocateCollectable`/`NSReallocateCollectable` (GC-era Foundation, used by `MAGFFoundation`) were stubs returning NULL: `-[CGenGFNodeManager initWithNamespace:]: Memory allocation failed`. Now `calloc`/`realloc` (`macapps/fndurlcomponents.m`).
* Constants such as `kTISPropertyInputSourceID`/`kMD…` in my CoreServices/Carbon and IOKit supplements were emitted as code (`mov x0,#0; ret`), so Logic read instruction bytes as pointers (`CFRelease(0xd65f03c0d2800000)`). Now 8 zero bytes of data (`carbon.sh`, `swiftov.py`).
* `TISCopyCurrentKeyboardInputSource()` returned NULL and Logic calls `CFRelease` on the result without a check (never NULL on macOS): now an empty CFDictionary (`macapps/carbon.c`); `TISGetInputSourceProperty` still returns NULL, which Logic handles.
* `macapps/logicimage.sh` links the nib and installs `Resources` (earlier rounds).

**First Swift overlay trap** (`brk #1`, by design, in `/usr/lib/sov/<n>.dylib`): `$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF` = `String._bridgeToObjectiveC() -> NSString`, called from
`MACore` (+0x17d70) in a nested `dispatch_once`. Logic's Swift code now needs the real Foundation overlay; 424 Foundation symbols are imported by the arm64 slices, `String`↔`NSString`,
`Array`/`Dictionary`↔`NS…`, `URL`, `Data`, `Date`, `CharacterSet`, `Notification`, `JSONEncoder/Decoder`, `NSLocalizedString`. That is package E of `logic-pro-gap.md`: a Swift module named `Foundation`
built with the host `swiftc` against Darling's Foundation (the bridging functions are small; `String._bridgeToObjectiveC` alone is a `NSString(string:)`).

### Round 18: Foundation Swift overlay from swift-foundation (first measurement)

Approved download: `swift-foundation` (tag swift-6.4.0-RELEASE) plus, through SwiftPM, `swift-collections` 1.1.6, `swift-foundation-icu` and `swift-syntax` (release/6.4.x). **The sources and the
build cache take about 490 MB on disk** (git history included), more than the 150 to 250 MB I estimated; `swift/foundation.sh` is the recipe.

With `-Xfrontend -module-abi-name Foundation` (and the package's own `DATA_LEGACY_ABI` for Apple platforms) **`FoundationEssentials` builds in about 4 minutes** and its symbols are named
`$s10Foundation…`: **153 of the 424 Foundation Swift symbols Logic imports are provided** (Data, Date, URL core, UUID, JSONEncoder/Decoder, AttributedString core, `__DataStorage` …).

The other 271 are what Apple's closed ObjC layer of the overlay adds: `IndexSet` (23), `CharacterSet` (18), `URLResourceValues` (10), `Notification` (6), `URLRequest`, `URLError`, `_BridgedNSError`/`CocoaError`
bridging, `LocalizedError`, `NSFastEnumerationIterator`, KVO (`NSKeyValueObservation`, `_KeyValueCodingAndObserving`), the `_bridgeToObjectiveC` family, the `Locale`/`FormatStyle`
symbols from `FoundationInternationalization` (not built yet; needs swift-foundation-icu, whose build was resolved but not run), and `NSLog`/`NSLocalizedString`.

Linking `FoundationEssentials.o` and the collections objects into a dylib against our `libswiftCore` leaves **244 undefined symbols**, all from Swift runtime modules we have not built yet:
`Observation` (`ObservationRegistrar`), `_StringProcessing`/`RegexBuilder` pieces (`Regex…`), `Synchronization` (`Mutex`, `_Cell`). Those come from the same `swift` repository as `libswiftCore`
(`Runtimes/Supplemental/…`, built like round 3 with `swift/build.sh`, plus `-lswiftSynchronization`-style dylibs in `/usr/lib/swift`).

Order of the next steps: (1) build the supplemental runtimes (Synchronization, Observation, _StringProcessing, _RegexParser) into `/usr/lib/swift`; (2) link `FoundationEssentials` into `/usr/lib/sov/37.dylib` and drop
the 153 trap labels; (3) write the ObjC bridging layer for the remaining ~270 symbols against Darling's Foundation (old open `stdlib/public/Darwin/Foundation/*.swift` of the swift 5.x repository is the
natural base, it has `IndexSet`, `CharacterSet`, `Notification`, `URLRequest`, `_BridgedNSError`, KVO; the ABI is the shipping one); (4) `FoundationInternationalization` for Locale/format styles.

### Round 19: the supplemental Swift runtimes build; FoundationEssentials still needs the Darwin and os overlays

`swift/supplemental.sh` builds, for arm64, **Synchronization, Observation, _StringProcessing, _RegexParser and RegexBuilder** (`libswiftSynchronization.dylib`, `libswiftObservation.dylib`, `libswift_StringProcessing.dylib`,
`libswift_RegexParser.dylib`, `libswiftRegexBuilder.dylib`, 0.2 to 1.3 MB each) into `run-arm64` next to `libswiftCore`/`libswift_Concurrency`. Details found: the Supplemental CMake files require the Core build tree's
`cmake/SwiftCore` package and an optional `SwiftOverlay` (not available on Darwin: `find_package(SwiftOverlay REQUIRED)` is sed-ed to optional); Observation needs `b-arm64/include` (`CMakeConfig.h`); the string-processing
sources live in a separate repository, `swift-experimental-string-processing` (tag swift-6.4.0-RELEASE, 17 MB), which `Runtimes/Resync.cmake -DStringProcessing_ROOT_DIR=…` copies into the tree.
The new libraries are **not staged into the image yet**: Observation and the rest import the `Darwin`/`os` overlays, which are still empty trap stubs, and dyld binds eagerly.

Linking `FoundationEssentials.o` (+ collections, `_FoundationCShims`) against the full set leaves **163 undefined symbols**, down from 244. They are now, apart from libSystem/ObjC runtime
names that resolve at load (`___maskrune`, `__NSGetEnviron`, `__os_log_impl`, `___stack_chk_guard`…):
* the **`Darwin` overlay** (`errno`, `open`, `fcntl`, `S_IF*`, `MAP_FAILED`, `POSIXErrorCode` …): the open source of this overlay is `stdlib/public/Platform` in the swift repository (gyb-generated, in the sparse clone);
* the **`os` overlay** (`Logger`, `os_log_type_t` … , 5 symbols);
* **`Combine`** (`TopLevelEncoder`/`TopLevelDecoder` protocol requirements, 6 symbols): closed.

Next steps in order: (1) build the `Darwin` overlay from `stdlib/public/Platform` (module `Darwin`, install `libswiftDarwin.dylib`); (2) a minimal `os` (Logger writing to stderr) and a `Combine` protocol-only stub module;
(3) link `FoundationEssentials` into `sov/37.dylib` and drop the 153 trap labels; (4) stage all of it (`install.sh`), then the ObjC bridging layer and `FoundationInternationalization` as in round 18.

### Round 20: the Darwin overlay (arm64)

`swift/darwin.sh` builds `libswiftDarwin.dylib` (module `Darwin`) from the open `stdlib/public/Platform` sources. Apple's own Darwin overlay is not open source, so: `Platform.swift` without the `semaphore.h`
block, `POSIXErrorCode` generated from the SDK's `<sys/errno.h>` (107 cases with Darwin's own numbers: the open sources only have Linux/BSD/Windows/WASI tables), and `swift/darwin/extras.swift`
(`MAP_FAILED`, `noErr`, `POSIXErrorCode.EWOULDBLOCK`). The tgmath wrappers are left out (they do not compile here). Result: 48 `$s6Darwin…` symbols, **covering both Logic's own imports from `libswiftDarwin`
(2) and everything `FoundationEssentials` needs from `Darwin`** (errno get/set, `open`, `fcntl`, `S_IF*`, `S_ISVTX`, `S_IWUSR`, `MAP_FAILED`, `POSIXErrorCode` with its metadata and raw-value accessors).

Not tested in the VM yet (build and symbol coverage only). Remaining for the overlay chain: a minimal `os` (`Logger`, `os_log_type_t`) and a protocol-only `Combine`, then the link of
`FoundationEssentials` into `sov/37.dylib` and staging (`install.sh`), then the ObjC bridging layer and `FoundationInternationalization`.

## What blocks, in order

1. ~~Cheap fixes~~ done in round 2 (empty stubs, AudioToolbox staged). Left from this step: the real `libswiftCore` and
   `libswift_Concurrency` from `swift/build.sh` (needs the download above).
2. **Swift overlays** (Dispatch, os, ObjectiveC, CoreFoundation, Darwin, Observation, simd, UniformTypeIdentifiers, XPC,
   IOKit, Metal, QuartzCore, CoreMIDI, CoreAudio, Accelerate, Spatial, OSLog) and the **Foundation Swift overlay**
   (1,366 symbols here). Packages D and E of `logic-pro-gap.md`; months.
3. **SwiftUI and Combine.** 2,076 + 179 imported symbols, 14 of Logic's binaries including the main framework. Without a
   SwiftUI the launcher cannot load `MACore`. No known avoidance.

So the honest state: the plain C/C++ part of Logic loads (see `logic-dyld.md`), the launcher's own dependency closure fails
at the first Swift-only framework. Reaching a window needs 2 and 3, which are not started.

## Not done / untested

x86_64 (not run); the full Logic resources (only code and frameworks were installed); no launch with the missing
frameworks stubbed (round 2 would immediately hit the Swift libraries). The 1,869 figure comes from `machcheck.py`
(static, symbol names only), not from a run.

### Round 21: the Foundation overlay is in the image; the trap that remains is the ObjC bridging layer

Built and staged (arm64, VM `CLOKEOS_ARCH=arm64`):
* `swift/foundation-overlay.sh` links `fo.dylib` (10 MB): FoundationEssentials + swift-collections + `_FoundationCShims` + minimal `os` (Logger via `os_log_create`, `OSLogType`) and protocol-only `Combine`
  (`swift/overlay/`) + `compat.m` (typed malloc, fts/removefile/chflags/quotactl stubs, `uuid_parse`, `OS_os_log`). Install name `/usr/lib/sov/fo.dylib`; `libswiftCompatibilitySpan.dylib` is a re-export of `libswiftCore`
  (the 6.4 core already contains `Span`).
* `macapps/stubgen/swiftov.py --overlay Foundation=<fo.dylib>`: symbols that `fo.dylib` provides get no trap label, and the Foundation supplement (`sov/37.dylib`) re-exports it.
  `macapps/install.sh` stages `fo.dylib` and the extra Swift runtimes (Synchronization, Observation, _StringProcessing, _RegexParser, RegexBuilder, Darwin, CompatibilitySpan).

Result in the VM: Logic loads `fo.dylib` and its runtimes (no dyld error any more), reaches `willFinishLaunching` as in round 17, and stops at the **same** trap:
`$sSS10FoundationE19_bridgeToObjectiveCSo8NSStringCyF` (`String._bridgeToObjectiveC() -> NSString`), called from `MACore` +0x17d70. So the 153 symbols from FoundationEssentials do not cover it,
as predicted in round 18: the ObjC layer (`String`/`Array`/`Dictionary` bridging, `IndexSet`, `CharacterSet`, `Notification`, `URLRequest`, `URLError`, KVO, …) still has to be written.
The way I see: a Swift module whose ABI name is `Foundation`, compiled against Darling's Foundation headers so that `NSString` etc. mangle as `So8NSStringC`; not started. Package E of `logic-pro-gap.md`.

Untested: x86_64 since round 3, `Jump.logicx` (nothing opened), audio engine, any rendering on ClokeOS.
Trap when rebuilding the image: `macapps/stubs.sh` must have run once (imports.json) and `CLOKE_EXTRA_STAGE` must hold `libcrashpc`/`libclogictrace` (`macapps/crashpc.sh <dir>`), otherwise Logic fails in dyld.

### Round 22: the ObjC layer of the Foundation overlay (`fo2.dylib`) from Apple's old open overlay

Apple's Foundation overlay (Swift 5.3, `stdlib/public/Darwin/Foundation`) is open source there, ABI-stable on Darwin, and was removed from later trees. `swift/foundation-objc.sh` fetches the 74-file
directory's relevant parts (about 2 MB from raw.githubusercontent.com, tag swift-5.3-RELEASE; Apache-2.0), patches them for Swift 6.4 / SDK 27 and compiles them as module `Foundation`
(`-import-underlying-module`, i.e. against the SDK's Foundation headers; classes bind to Darling's at run time) on top of FoundationEssentials:
NSArray/NSDictionary/NSSet/NSString bridging, IndexSet, CharacterSet, Notification, NSError, NSNumber, KVO, plus the ObjectiveC, CoreGraphics (`CGFloat`), Dispatch and Darwin (`MachErrorCode`)
overlays from the same tag, and `swift/overlay/Bridging.swift` (URL/Data/Date/UUID `_ObjectiveCBridgeable`, which FoundationEssentials does not have). Result `fo2.dylib`, re-exported after `fo.dylib` by the Foundation supplement
(`swiftov.py --overlay` can be repeated). **134 of the 271 missing Foundation symbols** come from it; still traps: `URLRequest`, `URLResourceValues`, `NSStringAPI` (the `String` NSString methods),
`NSCoder`, `FormatStyle`/`AttributedString` markdown, Combine publishers, most `URL` ObjC members.

Things that did not work out of the box (all handled in the script): the compiler crashes (`AArch64ABIInfo::computeInfo`) on ObjC method signatures with `Dictionary<AnyHashable, Any>` unless the
module contains `Foundation.swift` (the `AnyHashable: _ObjectiveCBridgeable` conformance); `NSNotification.userInfo` still crashes it (read through `perform(_:)`); `import CoreGraphics` in the overlay is a
module cycle (now `CoreFoundation`); `Array/Dictionary/Set/String: CVarArg` clash with the SDK interface; `String(_cocoaString:)` is gone (uses `utf8String`).

`swift/deploy-overlay.sh` rebuilds `fo.dylib`/`fo2.dylib`, re-runs `swiftov.py` and writes the supplements straight into an existing image (12 s instead of a 10-minute image build; the VM must be off);
`macapps/logicrun.sh` boots the VM and starts Logic with the trace helpers. Trap: a VM still running on the image keeps stale pages and shows the old behaviour, so `logicrun.sh` stops only the QEMU of that image.

Logic in the arm64 VM now gets through: dyld (all of `fo.dylib`/`fo2.dylib` and the runtimes load), `String`, `Array`, `Dictionary` and `NSURL`→`URL` bridging, then stops (SIGSEGV) in
`___chkstk_darwin` called from `FoundationEssentials`' `_URL.appending(path:directoryHint:encodingSlashes:)` (called through `URL.appending(path:directoryHint:)` from Logic.framework +0x88728):
the generic metadata argument (x3) has no sensible value witness table (size read from `*(x3-8)+0x40` is garbage). Not understood yet; either Logic's caller or a bridged value is wrong.

Untested: x86_64, opening `Jump.logicx`, audio engine, rendering.
