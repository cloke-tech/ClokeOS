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
