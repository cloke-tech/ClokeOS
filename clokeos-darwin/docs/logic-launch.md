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

Detail on the stop in round 22 (`macapps/crashpc.c` now prints x0..x28): the exported `URL.appending(component:directoryHint:)` (a merged-function wrapper in `fo.dylib`) hands the generic
metadata of `S` (x3 = 0x…30c0 in the image) and its witness table (x4, a *heap* address, so a dynamically instantiated conformance) on to `_URL.appending(…)`, which reads the value-witness size
from `*(metadata-8)+0x40` for its `alloca` and gets garbage. So `S` is a type whose metadata is not properly initialised in this process (candidates: a Swift generic from Logic that depends on an
unresolved overlay symbol, or metadata from the bridged `URL`). Next step: find out which type `S` is (print `swift_getTypeName` on x3 from the crash handler).

### Round 23: ABI fixed (library evolution), Logic runs through `setupAppFactorySharedInstance`

* **Cause of the `URL.appending` crash (round 22):** Apple's Foundation is built with `-enable-library-evolution`; FoundationEssentials from the 6.4 tag via SwiftPM is not. Resilient structs are passed
  indirectly and `self` goes in x20, so Logic's calls did not match the non-resilient entry points (metadata/witness table arrived in the wrong registers). `swift/foundation.sh` must build with
  `swift build … -Xswiftc -enable-library-evolution` (done by hand this round: 284 s); `fo.dylib` and `fo2.dylib` have to be relinked against it (`swift/deploy-overlay.sh` does both).
* **Own members of Apple's types:** Logic imports `URL._bridgeToObjectiveC` etc. under the names of the type's own module; mine are extensions in a second module and mangle as `…10FoundationE…`.
  `swift/foundation-objc.sh` generates `ld -alias` pairs from the import list (only for symbols `fo.dylib` does not already export: aliasing e.g. `URL`'s type descriptor created a second `URL`
  and made the runtime lose the `_ObjectiveCBridgeable` conformance). `NSURLResourceKey` is treated like `NSString` (same representation) when matching. New in `swift/overlay/Bridging.swift`:
  `URL` (`checkResourceIsReachable`, security-scoped access, `desktopDirectory`, `resourceValues`/`setResourceValues`, `bookmarkData`, resolving bookmarks), `URLResourceValues`, `Data` read/write,
  `Locale` bridging, the `StringProtocol` NSString API subset (`trimmingCharacters`, `replacingOccurrences`, `range(of:)`, `compare`, `contains`, `padding`, `components`, percent encoding),
  `String(format:)`. The Dispatch, ObjectiveC, Darwin and CoreGraphics Swift stubs now also re-export `fo2.dylib` (`swiftov.py --overlay` for each).
* **Availability:** PureDarwin answers `_availability_version_check` as macOS 11, so `#available(…6.3)` in `_Concurrency` failed (`ExecutorImpl.swift:90: this should never happen`).
  `macapps/availfix.c` is an interposer (`/usr/lib/libclokeos-availfix.dylib`, built by `macapps/crashpc.sh`) that lets every check pass; it belongs in the environment of every Mac program (cloakserver/apps.c
  should set `DYLD_INSERT_LIBRARIES` for it; not done). `macapps/crashpc.c` got: all registers, Swift metadata/witness-table inspection for x2/x3 (safe reads via a pipe), a mirror in `/tmp/crashpc.log`,
  interposers for `sigaction`/`signal` (crash signals stay ours) and `exit`/`_exit`/`abort` with a backtrace (needs `-not_for_dyld_shared_cache`).

Now (arm64 VM, `macapps/logicrun.sh`): `willFinishLaunching` → `didFinishLaunching` → `setupAppFactorySharedInstance() begin/end` (Logic's factories are built, Swift async tasks run,
MAUniversalContent's FileSystemEvent sources are created), then `-[NSAppleEventManager suspendCurrentAppleEvent]` (lenient no-op) and the process **dies silently**: shell status 132 (SIGILL), no `[crashpc]`
report (the handler does not run, `exit`/`abort` are not called), no console message. The faulting thread is probably not the main thread (dispatch worker); next step: install a `sigaltstack` on
every thread (interpose `pthread_create` and use a Mach exception port for the rest) or log `ILL` addresses from the kernel.

Still untested: x86_64, a window or menu of Logic, opening `Jump.logicx`, audio, rendering.

### Round 24: the silent death was a stack overflow; Logic now gets into its async startup tasks

* **Silent SIGILL kill** (round 23): the kernel could not deliver `SIGSEGV` on the overflowed thread stack and killed the process with SIGILL, so no signal handler ever ran. `macapps/crashpc.c` now also sets a
  **Mach exception port** for the task and serves it from its own thread (registers, selector and receiver class, a 400-frame walk with safe reads), and keeps its signal handlers against Logic replacing them.
* **Cause:** my placeholder classes `OS_os_log` (`swift/overlay/compat.m`, what `os_log_create` returns) and `OS_dispatch_*` (`dstub.m`) were root classes without any method; the first message to such an object
  (`methodSignatureForSelector:`) ran the forwarding path `__CFArm64Forwarding` ↔ `_CF_forwarding_prep_0` forever. They are `NSObject` subclasses now.
* `swift/overlay/os.swift`: `OSLogType.info/debug/default`, `OSSignpostType`, `OSSignpostID`, `OSSignposter`, `OSSignpostIntervalState` (a class), `OSSignpostError`, `OSLog.signpostsEnabled/disabled`, no-op `os_log(_:dso:log:_:_:)`.
  Still trap: `OSLog(subsystem:category:)` (init is unavailable in Swift, needs a different route) and `OSAllocatedUnfairLock`.

Current stop (arm64 VM, MAUniversalContent's async task): `EXC_BAD_ACCESS` in `objc_release`, called from `MAUniversalContentAppIntegration` +0x136e4 with a stack
`object_dispose → objc_destructInstance → object_cxxDestructFromClass` coming from `Logic.framework` +0xa75ac. The object passed to release has `isa == 0` (freed or zeroed memory: an over-release, a double
`object_dispose`, or a Swift object that was never registered with the ObjC runtime). Next step: find which object (print the argument of `object_dispose` through an interposer, plus `swift_getObjCClassFromMetadata`
for the metadata involved) and whether it is one of my overlay classes (`OSSignpostIntervalState`).

### Round 25: `os_log_create` objects, next stop: Combine

* The `objc_release` crash of round 24 was `-[MAUCContentPackageImporter .cxx_destruct]` releasing its `_logger` ivar (`NSObject<OS_os_log>`): Logic's ObjC code calls libSystem's `os_log_create`, and PureDarwin's returns a
  plain struct whose first word (the `isa`) is 0, so ARC's `objc_release` dereferenced a null class. `macapps/availfix.c` now also interposes `os_log_create` and gives the object the class `OS_os_log` (an NSObject subclass from `fo.dylib`).
  `macapps/crashpc.c` prints the instance variables of an object passed through `x19` (name, type, value) in the exception report, which is how this was found.
* New stop (arm64 VM): `EXC_BAD_INSTRUCTION` in `/usr/lib/sov/17.dylib` `$s7Combine9PublishedVMa` = `Combine.Published` metadata accessor, i.e. the Combine stub (trap labels). Logic's SwiftUI/Combine code
  (`@Published`, `ObservableObject`, subjects, `AnyCancellable`) needs a real Combine. Combine is resilient, Logic's classes embed `Published<T>` through metadata at run time, so a reimplementation with the same
  public API (OpenCombine, MIT) built as module `Combine` with `-module-abi-name Combine -enable-library-evolution` is a candidate; not started.

### Round 26: Combine (OpenCombine), Network, more Foundation; the open problem is heap corruption

* **Combine:** `swift/combine.sh` builds OpenCombine (MIT, 3.5 MB) as module `Combine` with library evolution into `combine.dylib` (165 of the 179 Combine symbols Logic imports have identical mangling); the C++ helper is replaced by
  `swift/overlay/combine-helpers.c` (PureDarwin has no libc++ `recursive_mutex`). `fo.dylib` links against it (FoundationEssentials' JSONDecoder is a `TopLevelDecoder`). `swift/foundation-objc.sh` now also compiles Apple's Combine glue from the 5.3
  overlay against it (Dispatch `SchedulerOptions`, NotificationCenter/Timer/KVO publishers, RunLoop/OperationQueue schedulers); `NotificationCenter` is ambiguous between FoundationEssentials and the SDK's class, so the Essentials import is per file and left out of that one file.
* **Network:** `swift/overlay/Network.swift` (`NWPathMonitor`, `NWPath`: always `unsatisfied`). **Foundation:** `Locale.Language.region` (via reflection), `URLRequest`, `URLQueryItem` bridging, `Bundle(__dsoHandle:)`.
* **Tooling traps found:** `swiftov.py` built no supplement for a stub library whose symbols are all provided by an overlay, so the stub kept re-exporting an old trap file; it now always builds one, and `swift/deploy-overlay.sh` deletes stale `usr/lib/sov` files in the image.
  `macapps/crashpc.c` got `CRASHPC_PEEK`, a stack scan, safe reads (a dereference inside the exception thread had killed the report) and `LOGIC_ENV="MallocCheckHeapStart=… MallocScribble=1"` runs through `macapps/logicrun.sh`.
* **`os_log_create`:** objects handed out by PureDarwin are now retained once more in the interposer (they are cached and were freed by ARC).

State (arm64 VM): Logic gets through the factories, MAUniversalContent's async tasks and a lot of Foundation/Combine/Dispatch use, and then dies of **heap corruption**: `free_list_checksum_botch` in `malloc`, or, with `MallocScribble`, `swift_retain` of a scribbled
(`0x55…`) pointer inside `URL.baseURL.getter` called from `URL._bridgeToObjectiveC` with a `self` pointing at freed memory (a Logic class instance whose `URL?` ivars are read after the object was released). This points to an over-release earlier
(Swift object released through the ObjC path, or an ownership convention mismatch of one of my bridging functions: owned vs guaranteed arguments of `_unconditionallyBridge…`, `Optional<URL>` handling) rather than to a missing symbol.
Next step: log retain/release of that class (interpose `swift_release` for objects whose class name matches), or compile the bridging functions with `@_effects`/explicit `__owned` to match Apple's conventions, and compare with the disassembly of Logic's callers.

Untested: x86_64, windows or menus of Logic, `Jump.logicx`, audio, rendering.

### Round 27: tracking the freed block (`CRASHPC_FREELOG`)

`macapps/crashpc.c` can now answer "who freed this block?": with `CRASHPC_FREELOG=1` it interposes `free`, keeps a ring of the last 2 M frees (address, size, four return addresses) and, in the exception report, lists the most recent frees of
every block that a register or a stack word points into (`LOGIC_ENV="CRASHPC_FREELOG=1 MallocScribble=1" macapps/logicrun.sh`). Also: `CRASHPC_PEEK=<hex offset>` prints a pointer slot of the crashed image; reads in the reporter are guarded (a dereference
inside the exception thread had silenced the report several times).

Findings with it (arm64 VM, same crash as round 26: `swift_retain` of `0x5555…` inside `URL.baseURL.getter`, called from `URL._bridgeToObjectiveC`, in a Swift task of MAUniversalContent):
* The object holding the URL is `Logic.CLgContentPathProvider` (Swift class with `universalContentLibraryBundleURL`, `basicContentBundleName`, `databaseName`, `universalContentLibraryBookmark`); its block (0x30 bytes) was last freed by CoreFoundation's
  `finalizeCFBurstTrie` / `CFBurstTrieAddUTF8StringWithWeight` (plist parsing), i.e. the address belongs to CF now while Logic still uses it: the provider was released while a reference was still held. Blocks of the `_URL` objects themselves are freed from
  `_URL.deinit` → `_URLInfo` destroy, as expected when the owner goes away.
* So the over-release is of the *provider* (or of a reference path leading to it), not of a URL. Candidates: the object is created or released through ObjC (`object_dispose` seen earlier from `Logic.framework`), where a Swift class with ObjC-visible
  layout is released one time too often; or the Swift async task keeps an unowned/unretained reference to it.
Next step: trace `swift_release`/`swift_retain` and `objc_release` for exactly this address (interposers in crashpc keyed by the block that the report names, second run), and read the disassembly of the Logic.framework code that creates the provider.

### Round 28: tracing tools, and a structural suspect

* `macapps/swifttrace.c`/`swifttrace.sh`: a wrapper `libswiftCore.dylib` in front of the real runtime (renamed `libswiftCoreReal.dylib`) that logs allocation/retain/release of instances of a class (`SWIFT_TRACE_CLASS`). It loads, but
  Logic's calls do not go through it: PureDarwin's dyld resolves their binds (chained fixups) straight to the re-exported real library, and its interposing does not apply to such images either (`free` and other classic binds work,
  `swift_allocObject`/`swift_retain`, `objc_release` called from Logic do not). Not usable as is; patching the entry points inside the runtime build would be the working route. The image must be restored with the original `libswiftCore.dylib`
  afterwards (done; `swtrace` is not part of the normal deploy).
* `macapps/crashpc.c`: `CRASHPC_TRACE_CLASS` (interposer based, same limitation), free ring now skips CoreFoundation's reuse of a block, `SWIFTCORE=<path> macapps/crashpc.sh` links the reporter against the runtime.
* What the free ring does show: the freed block (0xb0 bytes, a `_URL` box) is destroyed by `swift_release` → `doDecrementSlow` → `swift_release_dealloc` from `MATower` +0x3a86c, called by `MAUniversalContent` +0x5c08. That MATower function is a generic
  Objective-C round trip: it calls `_unconditionallyBridgeFromObjectiveC` into a temporary, `_bridgeToObjectiveC` on it, then runs the value witness `destroy` on the temporary. So the box dies with the temporary while the provider object
  (`Logic.CLgContentPathProvider`) still points at it.
* Why my bridging differs from Apple's: in swift-foundation `URL: _ObjectiveCBridgeable` exists only under `#if FOUNDATION_FRAMEWORK` (`_url.bridgeToNSURL()`, `URL(reference:)`); Apple's `URL` created from an `NSURL` **wraps and retains that NSURL**,
  mine builds a fresh `URL(string:)` from `absoluteString`. The same split exists for `URLComponents`, `URLQueryItem`, `Data`, `Date`, `UUID`, `Locale`… (everything compiled with `FOUNDATION_FRAMEWORK` in Apple's build and absent from my FoundationEssentials).
  Next step: a small Swift test program (round trip `NSURL → URL → NSURL` and storing `URL?` in a class, under `MallocScribble`) against `fo.dylib`/`fo2.dylib` in the VM to reproduce the over-release without Logic, then change `Bridging.swift` to keep the NSURL
  (store it in a wrapper and bridge back to the same object).

### Round 29: reference-count history of the freed objects (`CRASHPC_RRLOG`)

Test programs against `fo.dylib`/`fo2.dylib` in the VM (`NSURL ↔ URL` round trips ×3000, relative URLs, `URL?` in a class, arrays/dictionaries/sets of URL/Data/Date/UUID through `NSArray`/`NSDictionary`, under `MallocScribble`) **do not
crash**: my single-value bridging functions are memory safe on their own. So the fault is in how Logic's code uses them (conventions or sequence), not in a missing release inside them.

New tool: `CRASHPC_RRLOG=1` (macapps/crashpc.c). The runtime has a built-in switch for Instruments (`_swift_enableSwizzlingOfAllocationAndRefCountingFunctions_forInstrumentsOnly` plus the function pointers `_swift_retain`,
`_swift_release`, `_swift_allocObject`, …); the reporter installs `preserve_most` hooks there (this works where dyld interposing does not) and logs every allocation/retain/release into a ring (2 M events: object, caller, caller's caller).
The crash report then prints the history of each freed block that registers or stack words point to (`LOGIC_ENV="CRASHPC_RRLOG=1 CRASHPC_FREELOG=1 MallocScribble=1" macapps/logicrun.sh 110`; the log is in `/tmp/crashpc.log` in the VM,
`debugfs -R "dump /tmp/crashpc.log …"` gets it out after `sync` and `vmctl quit`).

What it shows (same crash, `URL.baseURL.getter` reading `0x5555…`):
* A `_URL` box (0xb0 bytes, `URL(string:)`) is created by `MATower` +0x3a780 (a generic NSURL → `T` → NSURL round trip), destroyed by the same function (`MATower` +0x3a86c, the value-witness destroy of the temporary), freed, **and the address is
  allocated again** by `URL._unconditionallyBridgeFromObjectiveC` (`fo2.dylib` +0x3fc → `URL(string:)`); Logic.framework (+0x85524 copies with `__swift_initWithCopy_strong`, +0x8553c destroys) then retains/releases that new box in balanced pairs.
* The string storage inside (`_URLInfo`, 0x80 bytes) has a balanced history as well and is released by `_URL.deinit`.
* So both histories are internally balanced; the pointer the crashing `URL.baseURL.getter` follows belongs to the *first* life of that address (freed by the temporary's destroy) or points into memory that Logic copied without retaining.
  That fits a value that was bit-copied out of a `URL` (e.g. `Optional<URL>` or the `URL` handed back by `_bridgeToObjectiveC`'s *round-trip partner* in MATower) while the temporary was destroyed, i.e. an **ownership difference between Apple's URL and mine**
  (Apple's `URL` made from an `NSURL` wraps that same `NSURL`, so the temporary's destroy only drops a reference to an object the provider also holds).

Next steps: make `URL`'s bridge keep the source `NSURL` (store it in a side field and return it from `_bridgeToObjectiveC`, as Apple's `URL(reference:)` does), rerun; if the corruption persists, log the `URL` value words (box pointer) at
MATower +0x3a86c and at Logic +0x85524 to see which pointer is copied without a retain.

### Round 30: keeping the source NSURL did not help (reverted)

Tried: `URL` keeps the `NSURL` it was made from (associated object on the `_URL` box, `_bridgeToObjectiveC()` returns that same NSURL, as Apple's `URL(reference:)`). The crash only moved into the new helper (`swift_unknownObjectRetain` on the box
with `isa == 0x5555…`): the `URL` value handed to `_bridgeToObjectiveC` already has a dangling box pointer. Reverted (associated objects keyed by address would also return a stale NSURL when an address is reused).

With `CRASHPC_RRLOG` the crashing box (`_URL`, 0xb0) shows two lives at the same address: life A is created by `URL(string:)` inside `MATower` +0x3a780, bridged (my `_bridgeToObjectiveC`, balanced) and destroyed by the value-witness destroy at `MATower` +0x3a86c;
life B is created later by my `_unconditionallyBridgeFromObjectiveC` for `MAUniversalContent` +0x5c1c and stays alive with a balanced history (no further free is recorded). The memory is nevertheless scribbled when the crash reads it, and three free records for the
address exist (the ring prints the three newest). So a pointer to life A (or a later unrecorded free of life B) survives somewhere in Logic's async task state. The recorded RR/free rings cannot tell which of the two without ordering between the two rings;
next step: one ring for both (events carry a global sequence number), additional hooks for `swift_unknownObjectRelease`/`swift_bridgeObjectRelease`/`swift_deallocClassInstance`, and logging the 8-byte `URL` value at `MATower` +0x3a780/+0x3a86c and `Logic` +0x85524.

### Round 31: ownership bug found (`__shared`), offline NSURLSession, os_log, next stop CoreData

* **The use-after-free was an ownership mismatch of mine** (found with the combined free/refcount ring, `CRASHPC_RRLOG`): Swift initializer parameters are *owned* (+1) by default, but Apple's `Data.init(contentsOf: __shared URL, …)` and
  `URL.init(resolvingBookmarkData: __shared Data, …, relativeTo: __shared URL?, …)` take them guaranteed (+0). Logic passed its `URL` at +0, my initializer released it once too often. Fixed with `__shared` in `swift/overlay/Bridging.swift`.
  Rule for every bridging function: copy `__shared` from the demangled Apple signature.
* `URLSession.data(from:delegate:)` (async) fails with `NSURLErrorNotConnectedToInternet`; `macapps/availfix.c` now also silences PureDarwin's `_os_log_impl` (it crashed in `strlen` on Apple's binary log buffers).
  `macapps/crashpc.c` symbolizes registers (`x<n> -> symbol`).
* Logic now runs past its URL/Data/Combine/Network/Dispatch/os code and stops at **Core Data**: `NSManagedObjectContext.performAndWait` (`sov/20.dylib`, trap). CoreData imports: 35 symbols, 4 of them Swift
  (`performAndWait`, `fetch`, `count`, `NSManagedObject.objectWillChange`); Darling's tree has a CoreData directory to start from.
* Cutefish's blog post about Mac apps ("Reef") describes the same approach (own AppKit/Foundation/SwiftUI, Mach-O loader), but Reef is closed source ("not for now"); nothing to reuse.

Untested: x86_64, windows or menus of Logic, `Jump.logicx`, audio, rendering.

### Round 32: Core Data and FileManager done; the wall is SwiftUI

* `swift/overlay/CoreData.swift` (module `CoreData`, in `fo2.dylib`): `NSManagedObjectContext.performAndWait/fetch/count` (empty results, there is no store behind the stub) and `NSManagedObject.objectWillChange` (`ObservableObjectPublisher`, one per object).
* `swift/overlay/FileManagerEnum.swift`: `FileManager.enumerator(at:includingPropertiesForKeys:options:errorHandler:)`, through an ObjC helper (`clk_fm_enumerator` in `dstub.m`); the file imports only `URL` from FoundationEssentials so that `FileManager` means NSFileManager.
* Stop now (arm64 VM, MAUniversalContent / Logic async startup): `swift_getGenericMetadata` instantiating `Combine.Published<SwiftUI.Image>`: the type metadata of `SwiftUI.Image` (`$s7SwiftUI5ImageVN`) is a trap label in `sov/107.dylib`,
  so the runtime reads a `brk #1` word (`0xd4200020`) as metadata and faults in `areAllTransitiveMetadataComplete_cheap`.
* SwiftUI imports from Logic: 2102 Swift symbols (322 nominal type descriptors, 193 metadata accessors, 48 type metadata symbols, the rest functions, protocol conformances, witnesses). Every type Logic puts into a generic argument or a stored property
  needs a real Swift type with the right name, generic arity and conformances; views need real layout and drawing on top. Cutefish's own write-up ("Reef") describes SwiftUI as the hard part too and builds it from documentation, comparing pixels against a real Mac (closed source).
  A first step could generate placeholder structs/classes with the right mangled names for the data symbols only (so metadata exists and instantiation works) and keep functions as traps until they are reached.

Untested: x86_64, windows or menus of Logic, `Jump.logicx`, audio, rendering.

### Round 33: SwiftUI placeholder types generated; now the first SwiftUI function is reached

* `swift/swiftui-gen.py` reads the SDK's `SwiftUICore` and `SwiftUI` `.swiftinterface` files (SwiftUICore has `-public-module-name SwiftUI`, so its types mangle as `SwiftUI.…`) and generates, for the types whose descriptors Logic imports,
  placeholder declarations with the right kind (struct/class/enum/protocol), nesting and generic parameter count (protocols with primary associated types get the matching `associatedtype`s). 392 of the 394 wanted types are found
  (`KeyPress.Phases`/`.Result` are missing); `swift/foundation-objc.sh` compiles them as module `SwiftUI` (library evolution) into `fo2.dylib`, and the SwiftUI stub re-exports it.
* With that, `Published<SwiftUI.Image>` instantiates and Logic runs on to the first SwiftUI **function**: `SwiftUI.Image.init(_: String, bundle: Bundle?)` (trap in `sov/107.dylib`).
* What comes next is of a different size: Logic imports 977 SwiftUI functions plus 198 conformances and 96 opaque result types (`some View`). And many SwiftUI types are `@frozen` (`Image`, `Text`, `Color`, …): Logic's code has their
  exact memory layout compiled in, so placeholders need the same stored properties as Apple's (the `.swiftinterface` lists them for frozen types; their internal types such as `AnyImageProviderBox` would have to be generated too).
  Views would then also need layout and drawing on top of AppKit. This is the SwiftUI reimplementation that Cutefish describes as the hardest part of their (closed) Mac layer.

Untested: x86_64, windows or menus of Logic, `Jump.logicx`, audio, rendering.

### Round 34: frozen layouts, first SwiftUI functions, next stop: protocol requirement layouts

* `swift/swiftui-gen.py` now builds a full index of both interfaces and, for `@frozen` types, emits their stored properties and enum cases (closure over the types they use, private ones as classes; `#if`-duplicate declarations, associated types
  of generic parameters and closure fields handled). 429 types, typechecks. `swift/swiftui-fields.py` lists the stored fields of frozen types.
* `swift/overlay/SwiftUIFuncs.swift` (hand-written, same module): `Image(_:bundle:)` (keeps name and bundle) and `NSHostingView<Content: View>`, a real `NSView` subclass holding `rootView` (draws nothing yet).
* Next stop: a Swift runtime fatal error in `instantiateWitnessTable` while checking a generic requirement — a Logic type's conformance to a SwiftUI protocol (e.g. `View`) has Apple's witness-table layout, my placeholder protocols have no requirements.
  Next step: generate the protocols with their requirement lists (associated types, `body`, `_makeView…`) from the interfaces so that witness tables line up.

### Round 35: SwiftUI protocols with their requirements; Logic now loads its first window (nib)

* `swift/swiftui-gen.py` now copies each protocol's requirements in declaration order (associated types, `body`, `_makeView`, `_makeViewList`, …), so that witness tables of Logic's conformances (e.g. its views to `View`) line up; 453 types, typechecks.
  The runtime error in `instantiateWitnessTable` is gone.
* `macapps/crashpc.c`: the deep stack scan (512 KB) skips libSystem/CoreFoundation/libobjc frames; selector/receiver printing only with `CRASHPC_OBJC=1`.
* **New stop: Logic opens its first window.** Chain: `-[NSWindowController window]` → `-loadWindow` → `+[NSBundle loadNibFile:externalNameTable:withZone:]` → `-[NSNib instantiateNibWithExternalNameTable:]` → `-[NSKeyedUnarchiver initForReadingWithData:]`.
  Logic's nibs are Apple's compiled **NIBArchive** format (see round 16, `macapps/nibarchive.py`), which Cocotron cannot read; the failure path logs the nib data as a huge formatted string and `-window` is entered again and again until the stack overflows.
* Next step: a NIBArchive reader in AppKit (an NSCoder that decodes NIBArchive's object/key/value tables into the objects Cocotron's nib loader expects; `macapps/nibarchive.py` documents the format), plus a re-entrancy guard in `-[NSWindowController window]`.

Untested: x86_64, `Jump.logicx`, audio, rendering; no window is visible yet.

### Round 36: NIBArchive reader; Logic's nibs load, Logic idles in its event loop

* `macapps/appkit/nibarchive.m` (in AppKit): `ClkNibArchiveToKeyed()` converts a NIBArchive nib in memory into an NSKeyedArchiver archive (binary plist with CF UIDs) that Darling's `NSKeyedUnarchiver` reads as usual;
  `patches/darling-cocotron/AppKit/nib.subproj/NSNib.m.sed` calls it before unarchiving, other nibs pass unchanged. Mapping: object *i* → `$objects[i+1]`, object 0 (the `NSObject` holding `IB.objectdata`) → `$top`;
  `NSString`/`NSLocalizableString` → string, `NSMutableString` → `{NS.string}`, `NSNumber` → number/bool, `NSData` → data, runs of `UINibEncoderEmptyKey` → `NS.objects` (dictionaries: keys and values alternate → `NS.keys`/`NS.objects`),
  `NSDecimalNumberPlaceholder` → `NSDecimalNumber`. Geometry stays a reference to a string (`{{x, y}, {w, h}}`), which `decodeRectForKey:` reads through `decodeObjectForKey:`.
  Classes missing in the process get a stand-in by name (`…View` → `NSView`, `…Cell` → `NSCell`, …, otherwise `ClkNibUnknownObject`); `CLOKE_NIB_LOG=1` lists them.
* Checked on the host against Apple's own `NSKeyedUnarchiver` + AppKit: 1156 of 1169 nibs in the Logic bundle convert and decode; the 13 others are storyboard pieces that need their storyboard's placeholders, plus one old pre-NIBArchive nib.
* `patches/darling-cocotron/AppKit/NSWindowController.m.sed`: `-window` is not re-entered while the window loads (per-object flag), so a failing nib can no longer recurse into a stack overflow.
* `macapps/deploy-files.sh` writes single files from the arm64 build root into the image; it and `swift/deploy-overlay.sh` now refuse to write while the VM on that image runs (a write during a run damaged the file system once; repaired with e2fsck on a copy of the partition).
  After `macapps/appkit.sh`, run `swift/deploy-overlay.sh` too: swiftov re-adds the supplement re-export to the new AppKit (otherwise `_NSFontDescriptorSystemDesignRounded` is missing at launch).
* Result: MainMenu and Logic's window nibs load (only `IBClassReference` needs the stand-in; `NSLayoutConstraint` decoding is a Cocotron stub that logs and returns), `didFinishLaunching` runs, and Logic sits idle in its event loop with no crash.
  No Logic window is visible and the menu bar still shows the previous app. `CLOKE_NO_UNTITLED=1` suppresses the untitled document; the project chooser Logic shows instead is a storyboard (`MANewProjectDialogBoard.storyboardc`), which Cocotron does not support.
* Next step: find out which window Logic wants to show at this point (and why it does not appear), then storyboard loading (`NSStoryboard` with the placeholders) or opening `Jump.logicx` directly.

Untested: x86_64, `Jump.logicx`, audio, rendering; no Logic window visible yet.

### Round 37: Logic's first windows on screen

* `macapps/appkit/lenient.m` (with `CLOKE_TRACE=1`): traces nib loads, `-[NSWindowController window/showWindow:]`, `orderFront:`/`makeKeyAndOrderFront:`/`orderWindow:relativeTo:`, a window list after 8 s and 30 s,
  and every thrown ObjC exception with reason and call chain (image + offset, via `objc_setExceptionPreprocessor`). Hooks are plain C functions: PureDarwin's libobjc has no trampolines for `imp_implementationWithBlock` (it aborts).
* Startup exceptions found that way and fixed (each one ended a launch step inside the event loop or killed Logic):
  * nil font for `NSFontAttributeName`: `+[NSFont systemFontOfSize:weight:]` and `+preferredFontForTextStyle:options:` were missing → `macapps/appkit/modern.m` (also `monospaced…SystemFontOfSize:weight:` and the system/label colors, all added only where AppKit has none).
  * nil key while MACore builds its key-code table (`+[ECKeyCodeConverter translateKeyCode:modifierFlags:]`): `macapps/carbon.c` now has `kTISPropertyUnicodeKeyLayoutData`, `TISGetInputSourceProperty` returning a layout object, `UCKeyTranslate` with a built-in US (ANSI) layout, `LMGetKbdType`.
  * `-[NSNumberFormatter stringForObjectValue:]` "requires a subclass implementation" (Darling) → `modern.m` maps it (and NSDateFormatter's) to `stringFromNumber:`/`stringFromDate:`.
  * nil GUI catalog (`-[MAAssetManager uiCatalogFromURL:withContext:]`): Logic's GUI images are TIFFs whose `ImageDescription` holds a plist. Onyx2D now reports `{TIFF}` (ImageDescription, Software, Orientation), reads ASCII tags as UTF-8,
    and decodes Deflate-compressed TIFFs (compression 8 and 32946, zlib; Logic's are Photoshop TIFFs with the horizontal predictor). `kCGImagePropertyTIFFImageDescription`/`…Software` are defined in ImageIO.
  * `CGImageCreateWithImageInRect(NULL, …)` crashed in Onyx2D; it returns NULL now, as on macOS (Logic passes a missing @2x image on).
  * `-[NSColor colorUsingColorSpaceName:]` raised "abstract" (thousands of times per launch); it forwards to `…device:nil` now, which `NSColor_CGColor` implements.
* `macapps/deploy-files.sh` also takes `source=target` pairs (used for libclokeos_carbon, CoreServices and Carbon from a `macapps/carbon.sh` stage).
* **Result:** Logic Pro's own windows appear on the ClokeOS desktop: the splash window ("Searching for Audio Unit plug-ins", copyright line) with the "Beta version" alert in front, and "Logic Pro" with File/Window in the menu bar.
  Logic also creates its Settings panel. The process keeps running (busy). Layout is approximate (`NSLayoutConstraint` is a Cocotron stub; anchors go to the lenient sink).
* Still thrown: `-[NSAttributeDescription init]` (Cocotron's CoreData is abstract there) on the Audio Unit scan thread.

Untested: clicking through the alert, the project chooser, x86_64, `Jump.logicx`, audio, rendering.

### Round 38: Logic's event loop runs; its alert dialog draws and reacts

* `macapps/crashpc.c`: `CRASHPC_SAMPLE=<s>` stops the main thread every *s* seconds and logs its call chain (frame pointers) — shows where a program spins that takes no events.
* Main thread spun over garbage: `CGSGetSymbolicHotKeyRepresentationList` (private SkyLight, called first with no buffer to get the count) was a generated stub returning 0 without writing the count, so Logic sized its hot-key table from an uninitialized value.
  `macapps/appkit/cgsprivate.c` (in CoreGraphics) reports 0 system hot keys; `CGSIsSymbolicHotKeyEnabled` returns false.
* `-[X11Window setAlphaValue:]` (abstract in Cocotron; the exception inside a `dispatch_once` ended in `std::terminate`) sets `_NET_WM_WINDOW_OPACITY` (`macapps/appkit/x11present.m`).
* `-[NSWindowController windowNibPath]` returned an arbitrary nib when the controller has no nib name (`pathForResource:nil ofType:@"nib"`): Logic's `LgAlertCollector`, which builds its window in code, loaded `Beta.nib` with itself as owner — that was the "Beta version" panel of round 37 whose OK did nothing. Now nil, as on macOS.
* Unknown nib connector classes (e.g. `NSNibAXAttributeConnector`) no longer raise in `-[NSNibConnector establishConnection]` (which dropped all later connections of the nib); they are skipped and listed with `CLOKE_NIB_LOG=1`. The NIBArchive converter maps unknown `…Connector` classes to `NSNibConnector` and leaves `_NSCornerView`/`NSIBHelpConnector` to NSNib's own class mapping.
* `CARenderer`: `layer.contents` may be an NSImage (converted via `CGImageForProposedRect:`); anything else that is not an image is skipped instead of crashing in `O2DataProviderCopyData`.
* **Views draw without layers:** Cocotron renders layer-backed views through OpenGL, which ClokeOS does not have, so they stayed black. `NSViewLayersEnabled` is now off unless `CLOKE_VIEW_LAYERS=1`; every view draws through `drawRect:` into the window.
* `X11Window` flushes the X connection after unmapping, raising, lowering and restacking; before, an ordered-out window stayed on screen because the event loop then blocks in `poll()` with the request still in Xlib's buffer.
* **Result:** Logic sits in its normal `-[NSApplication run]` loop. Its startup alert dialog (`LgAlertCollector`, "Continue") draws, clicking Continue runs `closeDialog:` → `stopModal` → order-out. Behind it Logic has an untitled 578×302 window and the Settings panel visible.
  The dialog's list (a view-based `NSTableView`) stays empty grey.
* The dialog then stayed on screen: after order-out AppKit made the same panel key again and `-[X11Window makeKey]` mapped it back (`ensureMapped`). `makeKey` no longer maps a hidden window (as on macOS);
  `hideWindow` withdraws with `XWithdrawWindow` (ICCCM synthetic UnmapNotify, which CloakServer needs when it had reparented an already-mapped window). Now Continue closes the dialog,
  and Logic's splash window ("Searching for Audio Unit plug-ins") and the Settings panel remain.

Untested: what the untitled window is (project chooser?), x86_64, `Jump.logicx`, audio, rendering.
* Next stop: the splash stays at "Searching for Audio Unit plug-ins". The only exception left is on the Audio Unit scan thread (`MAAudioUnitSupport`, `XAUCache_ThreadFunction`): it builds a CoreData model in code
  (`-[NSAttributeDescription init]`), which Cocotron's CoreData has as an abstract stub. Likely the scan thread ends there and the splash waits for it (not yet verified with the sampler).
  Next step: CoreData model classes that can be created in code (`NSEntityDescription`/`NSAttributeDescription`/`NSRelationshipDescription` setters) plus whatever store the AU cache uses, or a way to let the scan finish without its cache.

### Round 39: CoreData models built in code; the alert's modal loop ends

* Logic's Audio Unit cache (`MAAudioUnitSupport`) builds its CoreData model in code and uses `NSBinaryStoreType`. Cocotron's CoreData could only load models from `.mom` files. Patches in `macapps/patches/darling-cocotron/CoreData/`:
  `NSPropertyDescription` gets `init`, `setName:`, optional/transient/userInfo (associated objects); `NSAttributeDescription` derives the value class from the attribute type; `NSRelationshipDescription` is to-one when `maxCount == 1` (as on macOS) and keeps `optional`;
  `NSEntityDescription` gets `init`, `setProperties:` (back-references, accessor methods on the object class, like loading from `.mom`), model back-reference via `-[NSManagedObjectModel setEntities:]`;
  `+[NSFetchRequest fetchRequestWithEntityName:]`/`-initWithEntityName:`/`-entityName`, resolved in `executeFetchRequest:error:`; `NSBinaryStoreType` and `NSSQLiteStoreType` map to the in-memory store (nothing is written; the AU cache is rebuilt each launch).
  Result: the scan thread no longer throws; the splash moves on to "Updating information about Audio Unit plug-ins".
* `+[NSImage imageWithSystemSymbolName:accessibilityDescription:]` and `imageWithSymbolName:…` return an empty 16×16 image instead of nil (no SF Symbols; Logic puts the result into a dictionary unchecked).
* `macapps/crashpc.c`: `CRASHPC_SAMPLE_ALL=1` samples every thread, not just the main thread. Reading logs: copy them to `/var/root` in the VM, `sync`, stop the VM and `debugfs dump` them (the serial console mangles long output).
* Modal loops: Cocotron's `stopModal` stopped the session begun last; Logic begins another one while its alert dialog runs, so "Continue" stopped that one and the dialog's loop ran on invisibly. `stopModalWithCode:` now stops the innermost *running* `runModalSession:` (as on macOS).
  The main thread now gets back to `-[NSApplication run]` after the dialog.
* `macapps/logicrun.sh`: `LOGIC_ARGS` passes arguments (e.g. a project path). `Jump.logicx` (1.3 MB) is copied into the image as `/var/root/Jump.logicx` with debugfs (not part of the repository).

### Round 40: opening Jump.logicx gets into the document setup

* `LOGIC_ARGS="-NSOpen /var/root/Jump.logicx"` hands the project to Cocotron's `-[NSApplication openFiles]` → `-[CLgAppManager application:openFiles:]`.
* `macapps/vmlogs.sh` copies `/tmp/logic.out` and `/tmp/crashpc.log` out of the VM through the image (sync, stop VM, debugfs dump).
* `CLOKE_TRACE` now also logs NSErrors for missing files (codes 2/4/260) and every `+[NSAlert alertWithError:]` with its call chain.
* Fixes found on the way, each one the next stop:
  * "No such file or directory" when opening: Darling's CoreFoundation dlopens `CoreServices.framework/Frameworks/LaunchServices.framework/LaunchServices` for a URL's type identifier, which PureDarwin lacks.
    `macapps/launchservices.c` (built by `macapps/carbon.sh`) provides `UTTypeCreatePreferredIdentifierForTag`/`kUTTagClassFilenameExtension`: types the running app declares in its Info.plist, then common system types, else `dyn.ext-<ext>`.
  * `CFRelease(NULL)` in Darling's `CFURLCreatePropertyForKey` for a URL without extension (`patches/darling-corefoundation/NSURL.m.sed`).
  * `+[NSString localizedStringWithFormat:]` was a Darling stub returning nil (`patches/darling-foundation/src/NSString.m.sed`).
  * `-[NSView trackingAreas]` returns a copy (MAToolKit removes areas while iterating).
  * `-[NSArray setValue:forKey:]` sets the value on every element (Cocotron's `NSObjectController` selection proxy writes through an array; binding `selection.keySignature`).
* Now: Logic loads `TemplateAccessoryView.nib` and goes on into project setup; next stop is a crash in OpenCombine's `AnyPublisher.init` called from `MALiveLoops`, plus a nil value put into a dictionary.

### Round 41: building Logic's main window for the project

Opening `Jump.logicx` now gets into the main window (transport bar, `CLgTransportBarView`). Fixes on the way:
* `swift/combine.sh`: `AnyPublisher`, `AnySubscriber`, `Subscribers.Completion`, `Subscribers.Demand` are `@frozen` as in Apple's Combine (same stored fields). With library evolution they were returned indirectly; Logic (MALiveLoops) expects `AnyPublisher` in `x0` and crashed writing through an unset `x8`.
* `+[NSColor colorWithColorSpace:components:count:]`, `-[NSColor type]` (`modern.m`).
* `-[NSPopUpButtonCell]`: Cocotron's own calls of `synchronizeTitleAndSelectedItem` go through a guard; `MAPopUpButtonCell`'s override calls `selectItem:`, which synchronized again (endless recursion, stack overflow).
* Named colours: `macapps/carcolors.py` reads every `Assets.car` of an app on the Mac with Apple's `assetutil --info` and writes `Assets.colors.plist` next to it (`macapps/logicimage.sh` does this when it copies Logic in);
  `+[NSColor colorNamed:]`/`colorNamed:bundle:` read it (names as in the catalog, plus `A.B` → `Colors/A/B` as Logic's MAAssetManager asks). Not a general Assets.car reader (images are not covered).
* CoreText: `CTFontManagerRegisterFontsForURL` adds the font file to Onyx2D's fontconfig configuration (Logic registers its own fonts); `CTFontCreateWithName(AndOptions)` returns an `NSFont` (on macOS CTFont and NSFont are one object), never NULL (system font for unknown names); `CTFontCopyPostScriptName` works on it.
* `-[NSControl font]` for cell-less controls keeps its own font, default the system font.
* `-[NSCell _textAttributes]` (private Apple method that MAToolKit cells call through `super`) returns a mutable dictionary with font, colour and paragraph style.
* `macapps/deploy-files.sh` quotes paths (spaces in "Logic Pro.app").
* Next stop: `NSRangeException` (index -1) while the window is built; also ~29 non-fatal nil-in-dictionary exceptions from `-[MAButtonCell setAssetSet:]`.
* Further fixes in this round: `-[NSSegmentedCell setSelectedSegment:-1]` selects nothing (was an out-of-range index); `-[NSFontDescriptor fontDescriptorWithDesign:]` returns the descriptor (no design variants);
  `-[NSMenu itemArray]` returns a copy (Logic changes the menu while iterating); `NSIBObjectData` uses only matching key/value pairs when an object in the nib decoded to nil (MALiveLoopsUI nibs).
* State: with `Jump.logicx` Logic no longer crashes during the project open; the splash's status line goes empty and Logic keeps working. No project window visible yet. Non-fatal: ~250 nil-in-dictionary exceptions from button asset images (Assets.car images are not read), `CAShapeLayer` lacks `strokeColor`/`lineWidth`.
* `O2BitmapContextCreate` returns NULL for impossible sizes (sides over 32768 or over 1 GiB, also with caller-supplied memory), and `O2BitmapContextCreateImage(NULL)` is NULL: MALiveLoopsUI asked for a
  4713012 × 6633128 bitmap (views without layout — constraints are a Cocotron stub), callocated it lazily and copied for minutes. `CRASHPC_SAMPLE_REGS=1` prints x0–x3 of each sampled thread (that is how the 16 TB copy showed up).
* **Now: Logic orders the project window front** (`-[NSDocumentController openUntitledDocumentAndDisplay:error:]` → `makeKeyAndOrderFront:`) and draws its views; it then crashes inside a MAToolKit draw function (`CFArrayGetCount(NULL)`).

### Round 42: drawing the project window

* `macapps/appkit/ctline.m` replaces Cocotron's CTLine.m/CTRun.m (all stubs returning NULL): a line keeps its attributed string, splits it into runs at attribute boundaries, glyphs and advances from the run's font
  (no ligatures, kerning, bidi or font fallback); typographic/image bounds, string index/offset, truncation (returns the line unchanged), `CTLineDraw` through AppKit when the context is the current one.
  MAToolKit's segmented scrubber crashed on `CFArrayGetCount(CTLineGetGlyphRuns(NULL))`.
* `-[NSGraphicsContext CGContext]` (modern name of `graphicsPort`; Logic drew into NULL), `backingScaleFactor` = 1 on NSWindow/NSScreen, `CALayer contentsScale`, `+[NSDate now]`,
  `+[NSImage imageWithSize:flipped:drawingHandler:]`, `-[NSImage imageWithSymbolConfiguration:]` (`modern.m`).
  The missing `backingScaleFactor` was the cause of the huge MALiveLoopsUI bitmaps: the lenient sink only zeroed `x0`, so a CGFloat result was whatever was left in `d0`.
  **The lenient sink is now an assembly stub that also zeroes `d0`–`d3`** (CGFloat and small float-struct results such as NSSize/NSRect).
* Oversized bitmap contexts now become 1 × 1 contexts instead of NULL (Logic draws into the result unchecked).
* Next stop: `-[MAButtonCell _updateBufferedStateImages]` puts nil images into a dictionary during drawing (uncaught there). `CLOKE_TRACE` now lists images not found (`imageNamed:`, `-[NSBundle imageForResource:]`, `initWithContentsOfFile:`).
* Catalog images: `macapps/carimages.m` (a Mac tool) loads every named image of each `Assets.car` with Apple's AppKit (`-[NSBundle imageForResource:]`) and writes PNGs to `Resources/Assets.images/<name>.png`
  (1236 images, 42 MB for Logic; `CARIMAGES_2X=1` adds `@2x`). `macapps/logicimage.sh` builds and runs it and copies the tree in with the new `macapps/deploy-tree.sh` (mirror a Mac folder into the image).
  In ClokeOS' AppKit, `-[NSBundle URLForImageResource:]` (and with it `imageForResource:`/`pathForImageResource:`, `appkit/nsbundle_images.m`) and `+[NSImage imageNamed:]` fall back to these files.
  Like the colour files, nothing derived from Logic goes into the repository.
* `+[NSTabViewItem tabViewItemWithViewController:]`, `-viewController`/`-setViewController:` (Logic's inspector tabs had no items; nil identifier into a dictionary).
* Still open: ~250 non-fatal nils from `-[MAButtonCell _bufferedImageForMAButtonState:…cellSize:]` (likely zero cell sizes: no Auto Layout).
* `-[NSMutableDictionary setObject:forKeyedSubscript:]` with nil removes the key, as documented by Apple; Darling's threw (it forwarded to `setObject:forKey:`). That was the source of the ~270
  "Cannot set nil" exceptions (MAToolKit button images, inspector tab identifiers, …), fatal when raised while drawing (`patches/darling-corefoundation/NSDictionary.m.sed`).
* `-[NSBitmapImageRep bitmapImageRepByRetaggingWithColorSpace:]` returns the same pixels (was a stub returning nil); `CGRectApplyAffineTransform` computes the bounding box (was a stub returning the input).
* Now Logic opens `Jump.logicx` without crashing and shows an error: "The operation couldn't be completed. (NSCocoaErrorDomain error 256)" (a generic read error) — next to find out which read fails.
* `-[NSWorkspace type:conformsToType:]` (equality, plus the `UTTypeConformsTo` lists the app declares, transitively; everything conforms to public.item/data/content) and
  `isFilePackageAtPath:` (directory with an extension; Cocotron's MacWorkspace had it unimplemented). `CLgSongDocument` picks its read path with `type:conformsToType:`.
* `-[NSDocumentController makeDocumentWithContentsOfURL:ofType:error:]` uses `-initWithContentsOfURL:ofType:error:` (passes the error back, as on macOS).
* `CLOKE_TRACE` also logs `typeForContentsOfURL:`, `documentClassForType:`, `-[CLgSongDocument readFromURL:ofType:error:]` and `_readLogicXProjectFromURL:error:` with results.
* `MacWorkspace` (Cocotron's only NSWorkspace subclass) declared `type:conformsToType:`/`typeOfFile:error:`/`isFilePackageAtPath:` as unimplemented stubs, so the NSWorkspace versions never ran; they are now replaced there too.
* `CAShapeLayer` gets stored, KVC-compatible properties (path, fill/stroke colour, line width, caps, dash, stroke start/end); Logic's asset manager sets them with `setValue:forKey:` while building the
  project window, and the exception, caught inside Logic, became the "error 256". Nothing draws them yet.
* Still error 256: `-[CLgSongDocument readFromURL:ofType:error:]` is never reached; tracing `init` and `initWithContentsOfURL:ofType:withAlternativeIndex:error:` next.
* Document types are the UTIs from `LSItemContentTypes` (as on macOS since 10.5): `typeForContentsOfURL:` returns `com.apple.logicx.project` instead of "Logic Pro Project", and
  `documentClassForType:`/`_infoForType:` accept either. Logic checks the type with `type:conformsToType:` against UTIs; with the name it never loaded the project.
* With that, Logic goes into project loading and hits its Session Player database (`GenInstGenreCharacterDatabase`): bridging an **empty** NSDictionary to `[String: [String: Any]]`
  crashed in the Swift 5.3 Foundation overlay (`keys.baseAddress!` is nil for capacity 0 with the Swift 6 standard library). `swift/foundation-objc.sh` now returns `[:]` early for empty dictionaries
  (forced and conditional bridging). Why the genre cache is empty is not looked at yet.
* `-[NSKeyedUnarchiver versionForClassName:]` returns the class's version (Darling had a `DEBUG_BREAK()` trap there; MACore asks while decoding project data).
* **`Jump.logicx` is read successfully on ClokeOS:** `-[CLgSongDocument _readLogicXProjectFromURL:error:]` → YES and `readFromURL:ofType:error:` → YES. Logic then builds the project window and crashes in
  `-[NSBitmapImageRep initWithCGImage:]` (image without colour space) while MAToolKit prepares images. Not rendered, no audio yet.
* `initWithCGImage:` with a NULL image returns nil; a missing colour space counts as 1 sample. `+[UTType _typeOfCurrentDevice]` returns a "Mac" device type
  (Logic fills `%DEVICE%`-style placeholders with its `localizedDescription`; nil crashed in `CFStringFindAndReplace`). Next stop: `objc_msgSend` in MACore `UISyncManager::GetInternalName` (dispatch_sync).
* **The project window opens:** after fixing fast enumeration of `NSHashTable` (the GSIMap version precounted `nodeCount` and left garbage in the buffer when weak entries
  had gone; `macapps/patches/darling-foundation/src/NSConcreteHashTable.m.sed`), Logic shows "Jump.logicx - Tracks" with the tracks, region/track inspector and MIDI regions
  with notes, plus a second alert (empty list, "Continue"). Auto Layout is still a stub, so sizes and some controls are off. No playback or rendering yet.
* `crashpc` prints the first four words behind x0 and x22. `macapps/vmlogs.sh` copies via the serial console: if `logicrun.sh`'s command is still running there (wait time ≥ send timeout),
  the copy does not happen and old logs in `/var/root` are read. Keep the wait under 150 s.
* **Logic's audio engine runs on ClokeOS.** `ScanCoreAudioDrivers` (MAAudioEngine) dropped the shim's device until it answered `kAudioDevicePropertyStreamFormats` ('sfm#',
  at least one ASBD) and `kAudioDevicePropertyPreferredChannelLayout`; now added along with the 12 HAL functions Logic imports but the shim lacked (host time, listeners, the old
  `AudioDeviceGetProperty`/`AudioHardwareGetProperty` API). Logic then calls `MDCoreAudioNewDriver`, creates its IOProc and starts the device. With `CLOKE_CA_DEVICE=/tmp/out.wav`
  the shim writes a WAV in real time (125 s in a test run, valid header) – still **silent** (all samples 0); whether the transport plays is the next question.
* Traces: `CLOKE_CA_TRACE=1|2` (CoreAudio: unknown properties | every query with value), `CLOKE_AT_TRACE=1|2` (AudioToolbox), `libclogictrace` reports Logic's driver
  calls and dumps MAAudioEngine's device table after the scan (offsets for Logic 12.3 arm64).
* **Playback does not start yet.** Space reaches Logic (key code 49, key window "Jump.logicx - Tracks"). Logic turns input monitoring off (`MDCA::SetInputMonitoring` →
  `AudioDeviceStop`) and runs `MDCA::PrepareForStart` (MD's "primed" field at +0xfa84 goes 1 → 2), but `MDCA::Start` (vtable +0xa8 → `CAStart` → `AudioDeviceStart`) is never
  called, so the device stays stopped and the WAV silent. Also with `CLOKE_CA_KEEPRUNNING=1` (the shim ignores stops) nothing renders. Next: find who should call Start
  after PrepareForStart (caller chain in Logic.framework). Metronome on (K) changes nothing; Jump uses Sampler patches (need the sound library) and Retro/Vintage Synth.
* CoreAudio shim additions: `kAudioDevicePropertyIOThreadOSWorkgroup` ('oswg', from libcompat's `os_workgroup_interval_create`, the IO thread joins it and runs one interval
  per cycle), clock domain, byte-based buffer sizes, stream ranged formats; file output appends after the first start; `CLOKE_CA_WATCH=<hex offsets>` logs fields of the IOProc
  client (Logic's MD object) every 2 s; `AudioDeviceStop` traces its caller.
* `libclogictrace` hooks MDCA's vtable (Prime +0x88, PrepareForStart +0xa0, Start +0xa8; image base from the dyld list, checked against `MDCA::Start` = 0x71f210)
  and prints the caller chain. On Space: Logic +0x39b9f4 → +0x2c0020 calls `Prime` (+0x2bfccc) and `PrepareForStart` (+0x2c032c) on the main thread (via
  `performSelectorOnMainThread`, chain +0x858684 … +0x10fae08) and returns; **`Start` is never called** in the run. Start is expected later from another place (sequencer/sync
  thread or a callback) – that is the open point.

### Round 43: Logic plays Jump.logicx on ClokeOS – first audio

* **Cause of the missing start:** after `PrepareForStart` Logic waits for its sequencer timer ("SeqTimer", "SeqTimer 2"), a Carbon **Time Manager** task (`InsXTime`/`InsTime`
  + `PrimeTime`, TMTask packed to 2 bytes). CoreServices had only stubs for them, so the timer never fired and `MDCA::Start` was never called. `macapps/carbon.c` now
  implements `InsTime`, `InsXTime`, `PrimeTime` (ms for positive counts, µs for negative ones, drift-free for extended tasks) and `RmvTime` (remaining time in `tmCount`),
  one thread per task. Deploy: `macapps/carbon.sh <stage>` → `deploy-files.sh <stage>/usr/lib/libclokeos_carbon.dylib=/usr/lib/libclokeos_carbon.dylib`.
* **Result:** Return + Space in the project window → `MDCA::Start` → `AudioDeviceStart`; the shim's WAV (`CLOKE_CA_DEVICE=/tmp/out.wav`) contains **51 s of music** in a test run
  (RMS 3000–7000, no clipping, stereo, zero-crossing rates of tonal material). The file is not in the repo (it is derived from Logic content).
* Not yet: the whole song (the run ends when the VM is stopped), comparing the audio with a bounce on the Mac, which tracks actually sound (Sampler patches need the sound
  library), offline bounce (File > Bounce), x86_64.

### Round 44: whole song, compared with a bounce on the Mac

* ClokeOS (arm64 VM, real time, CoreAudio shim → WAV): Jump.logicx plays from bar 1 to the end, **291 s of audio without a single dropout** (no silent gap ≥ 10 ms).
  The beat period is 0.230 s throughout (= the project's 260 BPM), identical to the Mac.
* Reference: Logic Pro 12.3 on the Mac, File > Bounce > Project or Section, bars 1–149 (the project end marker; the regions continue after it, hence the longer ClokeOS run).
* Neither machine has the sound library: the 125 Sampler sample files and 4 Space Designer impulse responses the project names are missing on both, so the
  Sampler/reverb parts are missing in the same way.
* Comparison (log level envelope, octave bands): first 20 s envelope correlation 0.73–0.90; afterwards the coarse structure matches (offsets ±3 s, r 0.4–0.8 on
  10 s windows) but fine detail does not. Spectrum: ClokeOS +5…7 dB at 62 Hz, −5…10 dB from 250 Hz up. So one instrument or effect behaves differently on ClokeOS
  (not yet found; candidates: plug-ins that fail to load or run differently, e.g. Space Designer without IR, Drum Kit / Sampler fallbacks).
* Not done: listening comparison per track (solo each track on both systems), offline bounce on ClokeOS (File > Bounce), installing the sound library, x86_64.

### Round 45: offline bounce on ClokeOS (File > Bounce > Project or Section)

* The real-time recording (round 44) does **not** play the song straight: after ~20 s Logic's song position jumps and the song restarts (heard by the user, confirmed
  by envelope alignment). Logic primes its sequencer Time Manager task every 7 ms (`PrimeTime(t, -7000)`); in the VM the callbacks arrive every 10–56 ms
  (`CLOKE_TM_TRACE=1`). Likely cause of the position jumps; not fixed (timer precision under the VM / PureDarwin scheduler).
* **Offline bounce works** and is the way to render on ClokeOS: global menu File > Bounce > Project or Section (Command shortcuts do not reach Logic through X11 yet;
  the menu works the second time it is opened after start), tick the first destination, Return. Two Cocotron fixes were needed:
  `NSParagraphStyle.m.sed` (copied/decoded paragraph styles keep a mutable tab-stop array; the dialog threw "attempting to mutate an immutable object") and
  `NSSavePanel.subproj/NSSavePanel.m.sed` (Save builds folder + `nameFieldStringValue` + extension; before, without a selected row the path was empty).
  `CLOKE_SAVEPANEL_DIR=<folder>` answers save panels without UI. Logic shows its "Bouncing" progress window and writes `/var/root/Jump.aif`
  (AIFF, 24-bit, 44.1 kHz, **136.6 s = the project's 2:16**, the same length as the bounce on the Mac).
* Compared with the Mac bounce: level envelope correlation **0.755 over the whole song** (real time: 0.09), no restart. Remaining difference: ClokeOS +12 dB at 62 Hz,
  slightly less mids; waveforms do not match sample by sample (free-running synth phases). Which track/plug-in causes the extra low end is open (next: solo per track
  on both systems; the Mac per-track bounces Jump-T1/T2 exist).
* Dialog gaps: Start/End fields draw black, the destination list has no labels (view-based NSTableView), OK/Cancel are cut off (no Auto Layout).

### Round 46: per-track comparison (not reached yet)

* Solo bounces did not isolate tracks: on ClokeOS the `S` key solos the selected *region* (yellow bar) and the bounce still contains the whole mix; on the Mac,
  solo clicks sent to Logic in the background did not reliably take effect. All "per-track" files on both systems have the full-mix spectrum.
* Side result: the full-mix bounces on ClokeOS are **not reproducible in the low end** (62 Hz band 13.8–21.3 dB above 1 kHz across 8 bounces of the same
  project), while the Mac's three bounces agree to 0.1 dB (20.2 dB). So the "+12 dB at 62 Hz" from round 45 is mostly this scatter: something in the low end is
  nondeterministic on ClokeOS (candidates: a plug-in reading uninitialised memory, denormal/FTZ handling, random seeds; untested).
* File > Export > All Tracks as Audio Files runs on ClokeOS ("Exporting all Tracks", NSOpenPanel for the folder), writes the first track and then crashes in
  `MD::DisposeFileList` (called from `MD::FileRecordDisable`, garbage FileIO pointer). The export options view (`ExportRegionAsAudioFileView`) is not drawn, so the
  files are named "Untitled".

### Round 47: per-track export on ClokeOS, first per-track comparison

* **Export crash fixed (workaround):** after a track, `MD::NewFile` calls `MDFile::CheckDisposeFiles`, which releases every file no stream references – also
  the file of the track just finished, which Logic still passes to `FileRecordDisable` (use-after-free, found with crashpc's free log and `CRASHPC_DUMPREG=19`:
  the freed object was reused for Logic's tempo-analysis JSON). On the Mac pending async writes make `CheckDisposeFiles` return early; on ClokeOS the file
  thread is done sooner. `libclogictrace` replaces that one call with `nop` (MAAudioEngine +0x6ee2f4, checked before patching; `CLOKE_LOGIC_KEEPFILES=0` turns it off).
* **Global input fixes in AppKit:** (1) `NSWindow.m.sed`: hiding the key window passes key status to the main window / next visible window (an invisible
  NSOpenPanel kept swallowing all keys). (2) `X11.backend/X11Display.m.sed`: Carbon key codes (`-[NSEvent keyCode]`) come from the X keysym instead of a fixed
  evdev table – ClokeOS's X server uses XFree86 keycodes, so all arrow/Home/End/Page keys arrived wrong in every Mac app (Down was "keypad Enter").
* `clokeos-xtest` in the image was older than `cloakserver/xtest.c` (no key chords); rebuilt, so `vmctl.py xkey super+shift+e` (Cmd+Shift+E) works.
* File > Export > Tracks as Audio Files (Cmd+E) works on ClokeOS (files named after the track). Comparison with the Mac's export of the same tracks:
  track 1 "Evolving Shades" (synth) **identical** (RMS 0.0490 vs 0.0491, every octave band within 1 dB); track 2 "SoCal" (drums, sampler/Drum Kit Designer,
  samples missing on both machines) **differs**: 4× louder on ClokeOS, +27…+34 dB below 250 Hz, −10…−20 dB above 2 kHz. That is the low-end difference of
  rounds 44–46. Open: what the drum instrument plays on ClokeOS without its samples; tracks 3–8 not exported yet (remote control of the export was flaky:
  X focus stays on the closed panel, error alert NSCocoaErrorDomain 4 on the third export).
* Tools: `CRASHPC_DUMPREG=<n>` (memory around a register as text), crashpc free log skips libc++/CF reuse; F12 in Logic dumps the view tree (`[viewdump]`).

### Round 48: the drum track and Logic's sound library

* The Mac render has real drums: the sound library is not missing there, it lives relocated in `~/Music/Logic Pro Library.bundle` (45 GB), found through
  `~/Library/Application Support/com.apple.musicapps.content/Logic Pro Library.bookmark`. On ClokeOS the drum track (Drum Kit Designer, SoCal kit) plays a
  substitute: a pure ~92 Hz tone per hit (spectral flatness 0.004), identical in every bounce.
* **Bookmarks in CoreFoundation:** Darling's `CFURLCreateBookmarkData`, `CFURLCreateByResolvingBookmarkData`, `CFURLCreateBookmarkDataFromFile`,
  `CFURLWriteBookmarkDataToFile` and `CFURLResourceIsReachable` were stubs (NULL/false). `macapps/cfcompat.m` implements them in Apple's binary format
  ("book" header, TOC key 0x1004 = path components; reads bookmarks made on a Mac), the stubs are renamed by `darling-corefoundation/NSURL.m.sed`.
  With it Logic reads its library bookmark (before: "cannot find the Logic Pro Library.bundle"; then the one-time "copy your existing sounds" migration,
  skipped by the library's `Application Support/TransitionDone` marker).
* Image (not in the repo): `/var/root/Music/Logic Pro Library.bundle` with the 128 sample files Jump's instruments reference (458 MB: Drum Kit Designer,
  Vintage Strat, Fingerstyle Bass), the Drum Kit Designer plug-in settings and the library's `Application Support`; content metadata in
  `~/Library/Application Support/com.apple.musicapps.content`; bookmarks (both places Logic looks) pointing to the library.
* Still open: Logic does not open any `.caf` (traced with `CLOKE_LOGIC_PATHS=.caf` in `libclogictrace`, which logs open/stat/lstat/access/getattrlist),
  so the drum kit is decided "not available" before any sample access; the drum track stays the substitute tone.
* `macapps/deploy-tree.sh` now creates the target folder and its missing parents (before, a missing target silently wrote nothing) and reports debugfs errors.

### Round 49: drum samples load, drum track matches the Mac

* **Cause:** Darling's `CFURLEnumeratorPushURL` opened folders with the URL path (`CFURLCopyPath`), which is still percent-encoded
  (`/var/root/Music/Logic%20Pro%20Library.bundle/Plug-In%20Settings/Sampler/`). So `-[NSFileManager enumeratorAtURL:…]` found nothing in any folder with
  a space, and Logic's sampler scan ("Scanning Sampler Instruments") never saw an `.exs`; the Drum Kit Designer then played its ~92 Hz substitute.
  Fix: `darling-corefoundation/CFURLEnumerator.c.sed` uses `CFURLGetFileSystemRepresentation`. Affects every Mac app that enumerates a folder with spaces.
* Found with `libclogictrace`: `CLOKE_LOGIC_PATHS` now also logs `opendir`/`fopen`, and a failed `opendir` of a path with `%` prints its call chain.
* Image (not in the repo): Drum Kit Designer kits are `Plug-In Settings/Sampler/z_Internal/Drum Kit Designer/…` in the library (20 MB, found by name
  through the scan); Jump also needs `Sampler/Guitar/Vintage Strat.exs` (once the scan works, a missing instrument shows NSCocoaErrorDomain 4 at load).
* **Result:** exported drum track vs the Mac's: RMS 0.0319 vs 0.0317, envelope r 0.98, waveform r 0.90, every octave band within 0.1 dB, offset 0.
* Dead end: copying Logic's content database (`~/Music/Audio Music Apps/Databases/ContentDatabaseV01.db`, Core Data + SQLite FTS) crashes Logic in
  MASearchKit (`dispatch_sync` on a NULL queue; ClokeOS's Core Data has no store). Not needed: the folder scan is enough.
* Note: Darling's Foundation maps `NSDirectoryEnumerationSkipsSubdirectoryDescendants` to `kCFURLEnumeratorDescendRecursively` and CF descends when the flag
  is *not* set; both inverted, so Foundation callers work, direct CF callers get the opposite. Left as is.

### Round 50: tracks 3–8

* **Second export in one session crashed** (double `free` in `MDFile::CheckDisposeFiles`, this time from `MD::Prime`): the function has five callers, none uses
  a return value. `libclogictrace` now makes the whole function return at once (`ret` at MAAudioEngine +0x711058, checked against `sub sp, sp, #0x80`)
  instead of removing only the call in `NewFile`. Unreferenced file records stay until Logic quits. Several exports per session work since.
* `CLOKE_LOGIC_STRBT=<text>`: call chain when Logic fetches a localized string containing the text (`CFBundleCopyLocalizedString`), to find where an alert
  such as "Disk is too slow. (Bounce)" comes from. That alert appeared once on the bass track and did not come back after the crash fix.
* Remote control: Logic drops arrow keys sent faster than ~1 s apart (selection then lands on the wrong track); clicks on track headers or regions do not
  select. Export files are named after the track, which is the only reliable check. The VM display sometimes stops updating while Logic keeps working.
* **Comparison with the Mac:** track 3 "Fingerstyle Bass" (Sampler, Session Player bass) **identical** (envelope and waveform r = 1.000, every band within
  0.4 dB). Tracks 4, 5, 7, 8 (guitars: Session Player guitar `CbGuitarStrummer`, Sampler "Vintage Strat", Pedalboard) are **silent** on ClokeOS although
  the instrument and its sample load (`Vintage Strat.exs` opened, `Vintage Strat_consolidated.caf` opened 32 times). Open: whether the strummer produces no
  notes or Pedalboard silences the signal. Track 6 "Bright Synth Strings" not exported yet.
* `CLOKE_LOGIC_FXPROBE=1` (libclogictrace): level before/after every Logic effect (`CBasicFX::Process` MADSP +0x7a9a10 and `CPlugInShell::Execute`
  +0x202b8, patched with trampolines). Real-time playback showed the bass chain (Pedalboard with Face Fuzz, Bass Amp, compressor, tape delay) with
  plausible levels; no guitar chain (second Pedalboard, `CGuitarAmpShell`) appeared, but the playback window may not have reached the guitar entries
  (first guitar note at 15 s on the Mac). Next: run the probe through an offline bounce of the whole song. Blocker for remote tests: the VM display
  freezes after a few minutes and Logic then ignores keys.

### Round 51: guitars play – ALAC in AudioToolbox

* **Cause of the silent guitars:** the guitar sampler instrument ("Vintage Strat") streams `Vintage Strat_consolidated.caf`, which is **ALAC** (Apple
  Lossless, 4096 frames/packet, packet table). Our AudioToolbox had no codecs and rejected non-`lpcm` CAF, so the sampler got no audio; Logic then also skips
  the channel's plug-ins (no Pedalboard/Amp in the effect probe). Drum and bass samples are plain PCM, hence they worked.
* Found with two new probes in `libclogictrace`: `CLOKE_LOGIC_SMPPROBE=1` (per `MDSampler`: MIDI events and `Process` calls; all six samplers got notes)
  and `CLOKE_LOGIC_FXPROBE=1` (level before/after each effect). Both patch function entries with a generic trampoline (`tramp()`, refuses PC-relative
  instructions).
* **Fix:** `macapps/audiotoolbox/alac/` = Apple's open-source ALAC decoder (Apache 2.0, unchanged, LICENSE included), `alac_glue.cpp` = C interface
  (placement new, no exceptions/RTTI, no libc++ needed). `convert.c`: AudioConverter ALAC -> any PCM (magic cookie via
  `kAudioConverterDecompressionMagicCookie` or defaults, VBR packet descriptions, `at_conv_skip` for positions inside a packet). `file.c`: CAF with any
  format (`kuki` cookie, `pakt` packet table, also after the data chunk), VBR `AudioFileReadPacketData` with descriptions, packet-table/cookie properties;
  ExtAudioFile hands the cookie to the converter, seeks to packet + skip (with priming frames) and stops at the valid frame count.
* **Checked on the Mac:** our decoder + ExtAudioFile on `Vintage Strat_consolidated.caf` (26.3 M frames) is **bit-identical** to Apple's
  (`afconvert`), also after seeking to 0, 4095, 4096, 123457 and the end.
* **Result on ClokeOS:** the guitar chains now run (Guitar Amp, speaker/mic, Pedalboard with Chorus/Echo/Overdrive/Flanger). Whole-song bounce vs the
  Mac's: every octave band within 0.1 dB, envelope correlation 0.94 (2:16 vs 2:21; the Mac bounce has a longer tail). Not checked track by track yet
  (remote track selection is unreliable).

### Round 52: the project window draws (much) more like the Mac

Logic's window was drawn wrong (empty track headers, black arrange area, white inspector tables, empty control bar). Compared with
Logic on the Mac (macOS in Dark mode). All causes were gaps in Cocotron/Onyx2D/Darling, fixed globally (no Logic-specific code):

* **Asset colours:** Logic writes its colours as `"{r,g,b,a}"` / `"{gray,alpha}"` and parses them with `NSRectFromString`/`NSPointFromString`.
  Darling only accepted `{{x, y}, {w, h}}` (and scanned floats into CGFloat fields), so every asset colour was (0,0,0,0): invisible
  text, missing tints. Now the numbers are read in order like Apple's (checked on the Mac; `darling-foundation/src/NSGeometry.m.sed`).
* **Missing NSColor API:** `colorWithCGColor:`, `colorWithHue:saturation:brightness:alpha:`, `colorWithDisplayP3Red:…` (modern.m);
  catalog colours forward `getRed:…`/`getComponents:`/`colorWithAlphaComponent:` to their real colour; base `colorWithAlphaComponent:`
  returned nil for alpha < 1.
* **Onyx2D/CoreGraphics:** `CGContextGetClipBoundingBox` was always empty; `CGLayerCreateWithContext` called a method that does not
  exist (every CGLayer had no context); `CGGradient` returned NULL and linear/radial gradients, `CGContextDrawTiledImage`,
  `CGContextShowGlyphsAtPositions` were stubs (`appkit/cggradient.m`); `convertRectToUser/DeviceSpace`.
* **CoreText:** `CTLineDraw` only drew into the current NSGraphicsContext; now glyphs go through CoreGraphics into any context
  (CGLayer, bitmap) with the attribute colour; `CTRunDraw`; simple CTFramesetter/CTFrame (line breaks at newlines/spaces).
* **AppKit:** no layout pass at all (`setNeedsLayout:`, `layout`, `layoutSubtreeIfNeeded` before each display: `appkit/layout.m`);
  layers drawn in software without OpenGL (background, contents, `updateLayer`, sublayers, CATextLayer/CAShapeLayer/CAGradientLayer:
  `appkit/softlayers.m`); `NSDrawThreePartImage`/`NinePartImage`; `ancestorSharedWithView:`; `drawInRect:…respectFlipped:hints:`;
  `-[NSImage CGImageForProposedRect:context:hints:]`; table cells from `tableView:dataCellForTableColumn:row:` / the outline variant,
  values via `setObjectValue:`; NSCell's numeric setters no longer call an overridden `setObjectValue:` (Apple doesn't; Logic's
  panner cell recursed until the stack overflowed); NSPopUpButtonCell value setters; `NSSplitView` `setPosition:ofDividerAtIndex:` clamped
  widths to coordinates, `arrangedSubviews`/`removeArrangedSubview:` (Logic removes the Live Loops pane that way).
* **Appearance:** `NSAppearance` was a stub. Mac apps now follow ClokeOS' system appearance like on macOS (`CLOKEOS_DARK` from
  CloakServer, else `AppleInterfaceStyle`), with dark system colours (`appkit/appearance.m`); custom appearances (Logic's
  `MALogicAppearance`) follow the system.
* **Result (system dark):** ruler numbers, track names (`Evolving Shades`, `SoCal | Ch1` …), button outlines in the track headers,
  regions over the full arrange width (Live Loops pane narrower), dark inspector with Logic's own cells (check boxes, pop-ups,
  disclosure triangles), LCD shows the key. Screenshots are not in the repository (Logic content).
* **Still open:** instrument icons and M/S/I/transport glyphs (`Vector Glyph` assets in Assets.car are not extracted; the transport
  shows button titles instead), pop-up values (Logic's `MyPopUpButtonCell` shows an empty title), sliders/knobs in the track headers,
  control bar background, a remaining narrow Live Loops strip, dialog text fields (item 5) and the display freeze after dialogs
  (item 6) not looked at.
* Traces: `CLOKE_CT_TRACE=1` (CTLineDraw font/colour/position, call chain for transparent colours), `CLOKE_CG_TRACE=1` (call chain of
  CGColors created as (0,0,0,0)), `CLOKE_LAYER_TRACE=1` (layer background per view); `CLOKE_TRACE` logs `colorNamed:` misses,
  transparent MAAsset colours with their source string; F12 dump shows table delegates, prepared cells and backgrounds.
* Onyx2D transparency layers now draw into their own surface and are composited at the end (before, everything went straight to the
  destination and an empty layer was laid over it). No visible change in Logic yet; the instrument icons are still missing: Logic draws
  them through AppKit/layers, not with its own `CGContextDrawImage`/`CGContextClipToMask` calls (`CLOKE_CGCALL_TRACE=1` logs the image,
  mask and blend-mode calls an app makes, with caller offsets).
* Trap: two scripts typing into the same VM serial console at once garble each other (zsh stuck in a `dquote>` prompt).

### Round 53: symbols and button images

* **SF Symbols:** `+[NSImage imageWithSystemSymbolName:…]` now loads template images from `/Library/ClokeOS/Symbols/<name>.png`;
  `macapps/sfsymbols.sh <list>` renders them on the Mac with Apple's AppKit (`macapps/sfsymbols.m`) and writes them into the image
  (part of macOS, so never in the repository). Names come from `CLOKE_TRACE` ("Symbol fehlt: …"). Logic asks for only 12 at start;
  the editor toolbar's grid and action-menu symbols now show. Bundle symbols (`imageWithSymbolName:bundle:…`) use the bundle's images.
* **Button image position like Apple** (checked on the Mac): `setImage:` on a cell with `NSNoImage` makes it `NSImageOnly`, `setTitle:` on
  an image-only cell without title makes it `NSImageOverlaps`. Logic's transport and view buttons are built in code and showed only
  their titles ("Play", "Stop"); now they show no titles, like on the Mac, but also no images yet.
* **Why the transport images are still missing:** the button images themselves are fine (37×30 + 74×60 bitmaps with the play symbol,
  pulled out of the VM). Logic's `MAButtonCell` draws from buffered per-state images built from its asset sets (`offImages`, `onImages`,
  `glyphImage`, `_updateBufferedStateImages`), not through NSButtonCell; that path yields nothing yet (open since round 42). The
  instrument icons in the track headers are still missing too. Note: the 2× bitmap Logic renders contains the 1× drawing in one
  quarter (2× drawing scale not applied when rendering into a 2× representation).
* `-[NSCell _contents]` (private; the cell's value, a button's title, nil for image cells, as on the Mac).
* Traces: `CLOKE_BUTTON_TRACE=1` (button cells drawing an image: image, rect, view), `CLOKE_CT_TRACE` now prints the font name;
  F12 dump lists buttons with title, image size, image position and image representations.
* Seen once: Logic at full CPU with no events reaching it for minutes (no `sendEvent` in the log) while the main thread was not
  sampled; in the next run the main thread idled in the event loop. Not reproduced, cause unknown (may relate to item 6).

### Round 54: Logic's buttons draw (control bar like the Mac)

* **Lenient mode broke the app's own forwarding:** with `CLOKE_LENIENT` the sink answered `forwardingTargetForSelector:`, which the runtime
  asks *before* `forwardInvocation:`. Classes with their own forwarding (Logic's `MAButtonCell`, `MATextFieldCell`, `MASliderCell`,
  `MAButton` … forward `resourceBundleID`, `assetSetPath` to their asset helper) never got the call. Now the sink steps aside when the
  app's class (not a Darling placeholder) has its own `forwardInvocation:` and a signature for the selector; if that forwarding comes
  back to the same object with the same selector deeper in the stack (a loop because an AppKit method is missing), the sink takes over
  and logs `[clokeos-lenient] Weiterleitung im Kreis: …`.
* Missing AppKit methods found that way: `-[NSButtonCell _imageRectWithRect:]` (private; **every button image was drawn into an empty
  rect**), `_buttonType`, `-[NSCell calcDrawInfo:]`, `-[NSSegmentedCell _labelRectForSegment:inFrame:withView:]`; NSAccessibility
  properties as stored values on NSAccessibilityElement/NSView/NSCell/NSWindow/NSMenu (`appkit/accessibility.m`; Darling's
  NSAccessibilityElement was a stub and Logic's accessibility helpers forwarded in a circle until the stack overflowed).
* **Result:** the control bar shows Logic's view buttons, transport (stop, play, record, capture, cycle), LCD, count-in, tuner, solo
  and the right-hand buttons, laid out like on the Mac; the editor's back button shows. Still missing: M/S/I and the sliders/knobs
  in the track headers, instrument icons, pop-up values in the inspector.
* Traces: `CLOKE_BUTTON_TRACE=1` also logs `-[NSControl drawRect:]` of button controls and `drawInRect:…respectFlipped:` calls
  (image size, rect, operation, CTM, clip).
* Also added: `-[NSImageRep drawInRect:fromRect:operation:fraction:respectFlipped:hints:]` (10.6; Logic draws representations directly),
  `-[NSView prepareContentInRect:]`/`preparedContentRect`, NSTextFieldCell `_setVerticallyCentered:`/`_typesetterBehavior`;
  `NSCachedImageRep` paints into its own bitmap of its size instead of a hidden X11 window (whose surface need not match the content).
* Track headers, still open: the instrument icon is a 68×68 `NSCachedImageRep` drawn in full at x = 32, but only its top-right corner
  shows (cause not found: content of Logic's cached image or clipping); the volume slider is a 1×18 bitmap stretched to 108×18 at
  x = 443, outside the 368-point header view (layout/width differs from the Mac, where the header is narrower); M/S/I letters missing.
  `CLOKE_BUTTON_TRACE=1` also logs `[clokeos-rep]` (image representations drawn: size, pixels, source/target rect).

### Round 55: track headers – sliders and knobs, Live Loops pane gone

* **`removeArrangedSubview:`** now removes the view from the split view, as Apple documents for `arrangesAllSubviews` = YES (the
  default); before it stayed a subview and covered the track headers. Result: the black Live Loops block is gone, the track headers
  span their full width and show **volume sliders and pan knobs**; regions start right after them. (The header is wider than on the
  Mac: Logic's divider position.)
* **Vector glyphs from Assets.car:** `macapps/carimages.m` now also exports the type "Vector Glyph" (`NSSymbolImageRep` on the Mac;
  Logic's `Tools/…`, `Glyphs/…`, `ActionBarIcons/…`: 529 images, 2 MB) and writes an empty `<name>.template` next to template images;
  ClokeOS' `imageForResource:`/`imageNamed:` set `isTemplate` for them. `CARIMAGES_TYPES="Vector Glyph"` limits a re-run to one type.
* **Template images are tinted** in Cocotron's button and image cells (`controlTextColor`, disabled colour when disabled), as AppKit
  does; `-[NSImage _clkTintedWithColor:]` in `modern.m`. Logic's own cells (`MAButtonCell` & co.) draw their glyphs themselves, so
  the editor tool pop-up and the M/S/I letters are still not visible.
* Still open in the track headers: instrument icons (Logic's 40×40/68×68 icon images it paints itself come out empty; the source
  images are not found yet: they are in no `.car` file), M/S/I letters (`CharacterRGBImage/M`, `/S` are looked up and exist in no
  asset catalog, so Logic generates them some other way).

### Round 56: instrument icons in the track headers

* **Cause 1 – `bestRepresentationForRect:context:hints:`** answered with Cocotron's `bestRepresentationForDevice:`, which can return the
  image's `NSCachedImageRep`. Logic tints bitmap icons with `-[NSImage ma_imageRep:tintedWithColor:]` (MACore; CoreGraphics: bitmap
  context, draw, `SourceAtop` fill), which accepts only an `NSBitmapImageRep` and otherwise returns nil, so nothing was drawn. It now
  picks among the image's own representations like AppKit (smallest that covers the rect, else the largest, never the cache).
* **Cause 2 – `-[NSImage drawInRect:]`** (macOS 10.9) was missing. Logic draws SVG icons (e.g. `Icon_e_synth_1`, exported as a white
  glyph) in a drawing handler with `drawInRect:` + `NSRectFillUsingOperation(…, SourceAtop)` in the track colour; under `CLOKE_LENIENT`
  the call was swallowed. Added in `modern.m`.
* Result: drum, bass, guitar and synth icons show in the track headers (synths tinted in the track colour) and in the inspector.
* Checked step by step with a Mac-SDK probe in the VM (bitmap tint, squaring via `imageWithSize:flipped:drawingHandler:` +
  `CGImageForProposedRect:`, SVG branch); bytes match the Mac.
* `CLOKE_IMAGE_TRACE=1` logs every `-[NSBundle imageForResource:]` (bundle, name, file found, size).
* Found on the way, not fixed: Onyx2D treats `kCGBitmapByteOrderDefault` as little-endian for 8-bit RGBA images and contexts; Apple
  treats it as byte order (RGBA in memory). Raw bytes written by apps with default order are read with swapped channels.

### Round 57: M/S/I letters in the track headers

* Logic makes the letters with `+[MAAssetManager characterImageFromString:fontSize:color:]` (MACore): rounded system font via
  `fontWithDescriptor:size:0.0`, centred paragraph style, attributed string drawn with `drawAtPoint:` into a `lockFocus` image of
  its `size`. Two Cocotron bugs left the image empty:
  * **Font size 0** made a 0-point font (image 2 × 1). Apple uses the descriptor's size, else 12 pt; same for `fontWithName:size:`
    (`patches/…/NSFont.m.sed`).
  * **`drawAtPoint:` with right/centred alignment** laid the text out in a 1,000,000-pt container and drew it far to the right.
    Apple starts the text at the point; Cocotron now shifts by the used rect (`patches/…/NSStringDrawer.m.sed`).
* Result: M, S and I show on every track header; the selected track's input-monitoring button is orange as on the Mac.
* Found, not fixed: **`NSTextAlignment` values differ on arm64.** Apple uses the iOS order there (Left 0, Center 1, Right 2), Cocotron
  the old macOS order (Left 0, Right 1, Center 2), so text a Mac arm64 app centres in code comes out right-aligned. Nib decoding uses
  the stored values too, so a fix needs care.

### Round 58: inspector pop-up values

* Logic's inspector (`TableViewInspectorTile`) shows a row's value with `-[NSPopUpButtonCell setTitle:]`; items are only built when
  the menu opens. Cocotron added the item and selected it, but Logic's `-[MAPopUpButtonCell synchronizeTitleAndSelectedItem]` then
  looks for the item whose **state is On** and found none, so it deselected again (title stayed "dummy").
* **`altersStateOfSelectedItem`** (default YES) added to `NSPopUpButtonCell` (and forwarded by `NSPopUpButton`): `selectItem:` now
  turns the old item off and the new one on, as on macOS (`patches/…/NSPopUpButtonCell.m.sed`, `modern.m`).
* **Dark bezels:** Cocotron's push-button/pop-up bezel was always white; with a dark appearance it is now drawn dark (fill white 37 %,
  edge white 25 %), so Logic's light text is readable (`patches/…/NSGraphicsStyle.m.sed`).
* Result: the inspector shows Quantize/Off, (Undefined), Inst 6, All, Pre Fader, Delay …; the LCD shows the time signature.
* `CLOKE_BUTTON_TRACE=1` also logs pop-up `setTitle:` (items, selection, menu).
* Build note: `macapps/appkit.sh` stops when a patch's sed script is invalid (e.g. an unescaped `/` in a replacement); check its exit
  code before deploying.

### Round 59: text alignment on arm64

* **`NSTextAlignment` values:** on arm64 Apple uses the iOS order (left 0, center 1, right 2, justified 3, natural 4), on x86_64 the
  old macOS order (left 0, right 1, center 2). Cocotron always used the old order, so centred text from arm64 Mac apps came out
  right-aligned. `NSText.h` now has the arm64 order on arm64 (`patches/…/include/AppKit/NSText.h.sed`, plus the `NSTextAlignment…`
  names).
* **Archives keep the old order on every architecture** (checked on the Mac: `NSParagraphStyle` "NSAlignment" center = 2, right = 1;
  `NSCellFlags2` bits 26–28). `_CLKTextAlignmentFromArchive`/`…ToArchive` convert.
  * `NSCell` read the alignment from bits 16–18 of `NSCellFlags2`, so alignment from nibs was lost (everything left); now bits 26–28.
  * `NSParagraphStyle` read only Cocotron's own key "Alignment"; it now reads Apple's "NSAlignment" too and writes both.
* Checked with a probe in the VM against the Mac: text drawn left/center/right lands at 5 / 49 / 94 % of the width, cells and
  paragraph styles archived on the Mac decode to 0 / 1 / 2.
* Result in Logic: inspector labels are right-aligned against their controls as on the Mac; LCD texts centred.
* Build: `macapps/appkit.sh` now rebuilds all objects when a patch changes a header (before, only files whose own `.m` changed were
  rebuilt, so the old enum stayed compiled in).
* Probe trap: `vmctl.py send` stops reading at a line containing "% " (it looks like the shell prompt); keep "%" out of probe output.

### Round 60: idle CPU, display freeze, dialog title

* **Logic used 80–99 % CPU while idle** on the VM's single core (`CRASHPC_SAMPLE=3` on the main thread, 54 samples):
  * About half: `-[NSFont fontDescriptor]` called `-[NSFont maximumAdvancement]`, which measured **every glyph** of the font through
    FreeType on each call. The value is now cached per font (name, size), and `fontDescriptor` no longer adds
    `NSFontFixedAdvanceAttribute` (Apple only does that for fixed-pitch fonts) (`patches/…/NSFont.m.sed`).
  * About a quarter: `-[X11Window isMiniaturized]` asked the X server (`XGetWindowProperty`, a round trip) on every `displayIfNeeded`.
    It is now cached and dropped when `_NET_WM_STATE` changes (`PropertyNotify`; `PropertyChangeMask` added) (`X11Window.m.sed`).
  * Result: idle CPU about 6 %; the main thread mostly waits in `mach_msg`.
* **Display freeze (item 6):** likely the same cause: Logic's busy main thread starved Xorg and CloakServer on the single core. After
  the fix, opening and closing the Bounce dialog and clicking for 4 minutes kept the display updating. Not proven for longer runs.
* **Dialog title:** Cocotron set only `WM_NAME` from a lossy C string, so „Bounce “Jump”“ showed boxes. `setTitle:` now also sets
  `_NET_WM_NAME` in UTF-8 (CloakServer reads it first).
* **Dialog layout (item 5), not fixed:** the Bounce dialog's nib window is 671×400 and relies on **Auto Layout** (`NSViewConstraints`)
  to grow it and size the checkbox titles. ClokeOS has no constraint solver (`NSLayoutConstraint` and `NSGridView` are stubs), so
  OK/Cancel sit below the window (y = −107), the grid's checkboxes are 100 pt wide ("Include Aud…") and the view-based destination
  list has no labels. The Start/End fields are Logic's own dark scrubbers and look plausible.
