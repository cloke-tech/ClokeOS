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
| 1 | `DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks DYLD_PRINT_LIBRARIES=1 "./Logic Pro"` | `Library not loaded: /System/Library/Frameworks/DeveloperToolsSupport.framework/Versions/A/DeveloperToolsSupport` (needed by `Logic Pro` itself) | none, see below | stop |

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

## What blocks, in order

1. **Cheap, one afternoon:** stage AudioToolbox, add stubs for the 5 small frameworks above and `DeveloperToolsSupport`,
   copy `libswiftCore`/`_Concurrency` from the Swift session into `/usr/lib/swift`. With this the launcher gets to the next
   dyld error.
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
