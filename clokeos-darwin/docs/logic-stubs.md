# Stub frameworks for Logic Pro's closed imports (packages L and O)

Goal: every closed Apple library that Logic Pro links must *load* on ClokeOS, so dyld stops failing on
"library not found" and "symbol not found". Nothing here works functionally: a stub answers nil/0.

State 30.10.2026. Tested: the arm64 VM (HVF) loads all 33 stubs and `stubtest` passes 51 of 51 checks.
Not run: Logic itself; the x86_64 build in a VM (it builds, and `machcheck.py` is the only check there).

## What is generated

`macapps/stubgen/` (python) and `macapps/stubs.sh`:

| Step | Tool | Result |
|---|---|---|
| 1. inventory | `stubgen/collect.py APP.app imports.json` | per library: install name, compat/current version, imported symbols (x86_64 and arm64 slices merged) |
| 2. generate + build | `stubgen/stubgen.py imports.json OUT --arch A --build PDLIBS CFOBJ` | `OUT/src/<Name>.c` and a framework under `OUT/root/Library/ClokeOS/MacFrameworks` |
| both, per architecture | `macapps/stubs.sh [APP.app]`, `CLOKEOS_ARCH=arm64 macapps/stubs.sh` | frameworks copied into `$WORK/out/macapps[-arm64]/root`, so `install.sh` ships them |

Per symbol (kind from the SDK headers, else from the name):

- **C function**: returns 0/NULL; `void`, `double`, `float`, struct returns (CMTime, CMTimeRange, CGRect, …) and
  status returns are typed (`OSStatus` → -4 `errSecUnimplemented`, `CVReturn` → `kCVReturnError`). With
  `CLOKE_STUB_VERBOSE=1` the first call prints `[clokeos-stub] <Framework>: <function>` on stderr.
- **Constant**: `NSString`/`CFString` constants are a constant string equal to their own name; all others are 128
  zero bytes (so object constants such as `UTTypeAIFF` are nil). `kCMTimeZero`, `kCMTimeInvalid` and the
  infinities have valid CMTime values.
- **ObjC class** (`OBJC_CLASS_$_X`, `OBJC_METACLASS_$_X`): a real subclass of a per-framework root class
  (itself an `NSObject` subclass). Its `+resolveInstanceMethod:`/`+resolveClassMethod:` add a method returning
  nil/0 for every selector that does not start with `_`, so `[[MTKView alloc] initWithFrame:…]`, `[X foo]` and
  similar never crash. Verbose mode logs each new selector once.
- **Swift** (`_$s…`): not generated. They need the real Swift overlays (see below).
- **Hand-written** code goes into `stubgen/overrides/<Name>.c`, first line `// provides: sym sym …`.
  Only `Metal.c` exists: `MTLCreateSystemDefaultDevice()` returns nil, `MTLCopyAllDevices()` and
  `MTLCopyAllDevicesWithObserver()` an empty `NSArray` (looked up through the ObjC runtime, so the stub does not link Foundation).

Frameworks get the install name and versions Logic asks for (`compat` = lowest requested, `current` = highest),
and `Info.plist` has `ClokeStub = true`. Re-exports are copied from the original (read with `dyld_info -dependents` on
the host) when the target is a stub too; only `AVFoundation` → `AVFAudio` applies. Private frameworks
(`PackageKit`, `AssetCacheServices`) are installed under the same `Name.framework/Versions/A/Name` leaf, which is what
`DYLD_FRAMEWORK_PATH` matches. `libMobileGestalt` is a flat dylib: `stubs.sh` leaves it in `root/usr/lib/` and `install.sh`
copies it to `/usr/lib/` in the stage (dyld cannot redirect it without `DYLD_LIBRARY_PATH`).

Frameworks: Metal, MetalKit, MetalPerformanceShaders, ScreenCaptureKit, StoreKit, Network, CryptoKit, UserNotifications,
GameController, MultipeerConnectivity, QuickLookUI, PDFKit, GameplayKit, CoreMotion, iTunesLibrary, DiscRecording,
UniformTypeIdentifiers, CoreML, WebKit, JavaScriptCore, Security, PackageKit, AssetCacheServices, libMobileGestalt,
Cinematic, AVFoundation, AVFAudio, CoreMedia, CoreVideo, VideoToolbox, MediaToolbox, AVKit, CoreMIDI (33).
**CoreMIDI is stub only and has to be replaced by a real implementation** (all 54 imports answer -4 or nil).
Not stubbed because real code exists in `macapps/`: CoreFoundation, Foundation, AppKit, CoreGraphics, CoreAudio, AudioToolbox, Accelerate.

## Regenerate

```bash
CLOKEOS_ARCH=arm64 macapps/stubs.sh "/Applications/Logic Pro.app"     # collects the imports, then builds
CLOKEOS_ARCH=x86_64 macapps/stubs.sh                                  # reuses $WORK/out/stubgen/imports.json
macapps/machcheck.py "/Applications/Logic Pro.app" --arch arm64 --deep --alle \
  --image ~/ClokeOS-work/download/pd-20.5/puredarwin-arm64-virt.img \
  --frameworks $WORK/out/macapps-arm64/root/Library/ClokeOS/MacFrameworks --overlay $WORK/out/macapps-arm64/root
```

The import list is not committed (it is derived from Logic); `install.sh` runs `stubs.sh` once `imports.json` exists.
`stubs.sh` also merges what the already built Mac frameworks import from the stub libraries (`collect.py` over
`MacFrameworks`), see Security below.

Test in the VM (arm64): `stubgen/stubtest.c` dlopens all 33 stubs, calls Metal/CoreMedia/Security/GameController/libMobileGestalt
symbols, checks `kCMTimeZero`, a string constant, and sends `alloc`, `initWithFrame:` and unknown selectors to 11 stub classes.

```bash
clang -arch arm64 -mmacosx-version-min=11.0 -lobjc -framework Foundation -framework CoreFoundation -o stubtest macapps/stubgen/stubtest.c
# into a clone of the image with macapps/dlprobe.sh <clone> <stage> /   (stage = Library/ClokeOS/MacFrameworks, usr/lib, Library/ClokeOS/Probe/stubtest)
DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks CLOKE_STUB_VERBOSE=1 /Library/ClokeOS/Probe/stubtest
```

## Numbers (arm64 slice, `machcheck.py --deep --alle`, PureDarwin arm64 + the built Mac frameworks)

| | before | after |
|---|---|---|
| libraries Logic needs that are missing | 65 | 34 |
| of these in the list above | 31 | 0 |
| C/ObjC symbols imported from the 33 libraries (all unresolved before, their libraries being absent) | 456 | 0 unresolved |
| Swift symbols (`_$s…`) in them | 41 | 41 unresolved |

(The sum of "missing symbols" printed by machcheck rises from 1947 to 1974: with a library absent, its importers' symbols were
not checked at all; the rest are Foundation, CoreServices, AppKit, … gaps outside this package.)

Per framework (symbols imported by Logic and the Mac frameworks; functions / constants / classes / hand-written):

| Framework | imports | Swift left | generated as |
|---|---|---|---|
| Metal | 11 | 0 | 0 / 0 / 7 / 4 |
| MetalKit, MetalPerformanceShaders | 2, 1 | 0 | classes |
| ScreenCaptureKit, UserNotifications, MultipeerConnectivity | 4, 4, 4 | 0 | classes |
| QuickLookUI, PDFKit, GameplayKit, CoreMotion, iTunesLibrary, AVKit, Cinematic, PackageKit | 5, 1, 1, 1, 1, 1, 1, 6 | 0 | classes |
| GameController | 3 | 0 | 0 / 2 / 1 / 0 |
| DiscRecording | 85 | 0 | 18 / 62 / 5 / 0 |
| UniformTypeIdentifiers | 13 | 0 | 0 / 12 / 1 / 0 (`UTType*` objects are nil) |
| WebKit | 6 | 0 | 0 / 1 / 5 / 0 |
| JavaScriptCore | 41 | 0 | 39 / 1 / 1 / 0 |
| Security | 31 | 0 | 29 / 2 / 0 / 0 |
| AssetCacheServices, VideoToolbox, MediaToolbox | 1, 1, 1 | 0 | 1 function each |
| libMobileGestalt | 10 (none imported, MG* added by hand) | 0 | 10 functions |
| AVFoundation | 53 | 0 | 0 / 32 / 21 / 0 |
| AVFAudio | 24 | 0 | 0 / 13 / 10 / 0 |
| CoreMedia | 63 | 0 | 52 / 11 / 0 / 0 |
| CoreVideo | 20 | 0 | 17 / 3 / 0 / 0 |
| CoreMIDI | 54 | 0 | 40 / 14 / 0 / 0 (**replace later**) |
| CoreML | 23 | 16 | 0 / 0 / 7 / 0 |
| StoreKit | 6 | 6 | none |
| Network | 12 | 12 | none |
| CryptoKit | 7 | 7 | none |

## Swift symbols left (41, list in `$WORK/out/stubgen/<arch>/swift-symbols.txt`)

StoreKit 6 (`AppTransaction`, `VerificationResult`), Network 12 (`NWPathMonitor`, `NWPath.Status`), CryptoKit 7 (`Insecure.MD5`,
`HashFunction`), CoreML 16 (`MLShapedArray`, `MLModel.prediction`). Their importers (the Logic frameworks that use them)
cannot be bound until real Swift overlays exist (see `docs/swift-runtime.md`), because chained fixups bind everything at load.
Also still missing and outside the list: the Swift runtime dylibs, `libswiftAVFoundation` and `libswiftUniformTypeIdentifiers`
(overlays that sit next to the C part stubbed here). Loader-only placeholders for the `_$s` names would be possible
(assembler labels), but they would make the importer load and then crash on first use.

## Security: what the stub replaces

PureDarwin's `Security.framework` has 3387 exports, but it links PureDarwin's own `libCoreFoundation`/`libFoundation`
(measured), which clash with the Darling CoreFoundation in the same process. It lacks 16 of the 26 symbols Logic imports
(Authorization*, CMSDecoder*, SecStaticCode*, `kSecCodeInfoTeamIdentifier`, …). Because the framework path
shadows the whole framework, the stub has **only** the symbols that Logic, the Mac frameworks and PureDarwin's
`SystemConfiguration` import (31), and every other Security user inside `DYLD_FRAMEWORK_PATH` loses the real
implementation. It cannot re-export the original: the path would be redirected back to the stub.
Authorization/Sec* calls answer `errSecUnimplemented`, i.e. "no permission", never "success".

## Findings, not fixed here

- In the arm64 VM Darling's CoreFoundation crashes in `CFArrayCreate`/`CFArrayCreateMutable` and returns NULL from
  `CFStringCreateWithCString` in a small C program that links only CoreFoundation (not Foundation). The stubs avoid CF
  calls for that reason (the Metal override goes through `NSArray`). Worth a look by whoever owns `foundation.sh`.
- `CMTimeMake`, `CMTimeAdd` & co. return zeroed structs (invalid time), not real arithmetic; CoreVideo status calls say error.
- Struct- and floating-point returns are typed from the SDK headers; functions missing from the headers (private ones) return 0 (Security, CoreMIDI: -4).
- A stub class lets the program continue after nil answers; programs that test for a GPU through `MTLCreateSystemDefaultDevice()` get the "no GPU" path.
