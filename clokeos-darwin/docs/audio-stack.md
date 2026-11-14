# Audio on ClokeOS: what exists, what is missing, how to get to Core Audio

Status 29.10.2026. Everything below was measured on the PureDarwin 20.5 images that ClokeOS is built on
(x86_64 desktop `puredarwin.img`, arm64 `clokeos-2.5-arm64-desktop.img`) with Homebrew QEMU, except where
marked *untested* or *from memory*. Long-term goal: Logic Pro (CoreAudio, AudioToolbox, AudioUnit,
AVFAudio, CoreMIDI, CoreAudioKit, Accelerate; Audio Units hosted via XPC).

## 1. What PureDarwin 20.5 has

| Piece | x86_64 | arm64 |
|---|---|---|
| `IOAudioFamily` (Apple's, APSL, vendored unmodified, `src/Kernel/Extensions/IOAudioFamily`) | in `/System/Library/Extensions` and in the kernel collection (`pd-rebuild-kc` lists it) | **absent**: not in `Extensions`, not in the kernel |
| `RavynHDAudio.kext` (Intel HD Audio controller + codec, playback only) | loaded, attaches | file is in `Extensions` but is not in the kernel (no IOAudioFamily to link against), never matches |
| matches | PCI `8086:2668` (ICH6 HDA = QEMU `intel-hda`), `8086:293e` (ICH9), `8086:3198` (Gemini Lake) | the same IDs, never used |
| `/dev/dsp0` | yes, character device (21,0), `write()` only, fixed 48 kHz / 16 bit / stereo, no ioctl | no |
| AppleHDA, IOUSBAudio, virtio-sound, AC97, ES1370, HDA codecs other than the one `RavynHDAudio` enumerates | none | none |
| user-space audio (coreaudiod, CoreAudio.framework, AudioToolbox, AudioUnit, CoreMIDI) | none: `/System/Library/Frameworks` has only CoreFoundation, CoreServices, DiskArbitration, DriverKit, IOKit, Kernel, OpenGL, Security, System, SystemConfiguration | same |

`kextstat`/`kextload`/`kmutil` do not exist: kexts are linked into the kernel collection at image build time
(`pd-rebuild-kc`, `kc-builder`), nothing is loaded at run time. A driver is therefore "there" only if it is in
that list. The arm64 kernel is a different, monolithic build (`/System/Library/Kernels/kernel.release.virt`)
and ClokeOS has no arm64 kext build path yet (`kext/build.sh` is x86_64 only).

### What the guest kernel sees (ioreg)

QEMU devices attached: `intel-hda` + `hda-duplex`/`hda-output`, `virtio-sound-pci`, `usb-audio` on the xHCI.
All four exist in the installed QEMU (also `ac97`, `ES1370`, `ich9-intel-hda`, `virtio-sound-device`);
audio backends there: `none`, `coreaudio`, `dbus`, `wav`.

* **x86_64 (TCG):** `pci8086,2668` gets child `RavynHDAudio` → `RavynHDAudioEngine` (flavor 1,
  "Intel HD Audio", 48000 Hz, 65536 sample frames per buffer) → one output `IOAudioStream` (16-bit signed
  linear PCM, 2 ch, mixable). `IOAudioDevice`, `IOAudioEngine`, `IOAudioStream` instances exist in
  `IOKitDiagnostics`; `/dev/dsp0` appears. `virtio-sound` (`1af4:1059`) is a bare `IOPCIDevice` without a
  driver; the USB audio device enumerates as `IOUSBDevice` only (no interface driver).
* **arm64 (HVF):** the same PCI devices are enumerated (`pci8086,2668`, `pci1af4,1059`, USB device 0x46f4),
  all without a driver; there is no `/dev/dsp*`. Audio on arm64 therefore needs the kernel pieces to be built
  first, not just a new driver.

## 2. Design options for a Core Audio HAL

What Logic needs from the system: `AudioObject`/`AudioDevice` HAL API with IOProc callbacks driven by the
device clock (`AudioDeviceStart`, `kAudioDevicePropertyBufferFrameSize`, `…SafetyOffset`, `…Latency`, device
and stream formats, `AudioHardwareCreateAggregateDevice`), AudioToolbox (`AudioUnit` graph/`AUGraph`,
`AudioConverter`, `ExtAudioFile`, `AudioFile`), AUv2 component registry and out-of-process AUv3 hosting, and
CoreMIDI (`MIDIClient`, ports, endpoints, USB MIDI).

### Kernel side: IOAudioFamily-based kext vs. own kext vs. user-space driver

1. **Keep IOAudioFamily + one `IOAudioEngine` subclass per hardware (recommended first step).** It is already
   in PureDarwin, it gives per-client buffers (`IOAudioEngineUserClient`), mixing, format conversion and the
   sample-clock timestamps (`takeTimeStamp`, `IOAudioEngineUserClient::performClientIO`) that a user-space HAL
   needs, and it is exactly the interface Apple's own HAL (`AppleHDA` → `coreaudiod`) sits on. Cost: a
   `coreaudiod` substitute must speak this user-client protocol (shared sample buffer + clock mach port) – the
   headers are in the tree. New drivers (virtio-sound, USB audio class, more HDA codecs and capture) follow
   the style of `kext/ClokeI2CHID`/`ClokeRtw88`: a single `.cpp` + `Info.plist` built by `kext/build.sh`.
   Matching lists in `Info.plist` and a line in `pd-rebuild-kc` are all that registers it.
2. **Own minimal kext with a simple device node (`/dev/dsp0`-style, ring buffer + `read`/`write`/`ioctl`).**
   Less code per device but no clock timestamps, no mixing, each driver reinvents the node, and Core Audio
   would still need an adapter. This is what `RavynHDAudio` does today for `/dev/dsp0`; fine for a sine wave,
   wrong for a DAW.
3. **User-space driver (DriverKit-style or `IOUserClient` on `IOPCIDevice`).** `DriverKit.framework`
   headers are in the image, but nothing in the image implements dext hosting (no `kernelmanagerd`, no
   `IOUserServer` clients seen: `IOUserServer`=0 in diagnostics). PCI access from a normal process needs
   `IOPCIDeviceUserClient`, not verified here. Attractive only for USB audio class (libusb-style via the
   existing `IOUSBDevice` user client). *Not recommended now.*

### User-space side

* `coreaudiod` substitute: a daemon that opens the `IOAudioEngine` user client(s), publishes the HAL
  objects over a mach service (Apple uses `com.apple.audio.coreaudiod`; the client library
  `CoreAudio.framework` talks to it with MIG/XPC), and keeps per-device IO threads at real-time priority.
  Real Mac frameworks are binary-incompatible with a re-implementation of a private protocol, so the plan is a
  *new* `CoreAudio.framework` with the public HAL API (`AudioObjectGetPropertyData`, `AudioDeviceCreateIOProcID`
  …) that is a thin client of our own daemon, not Apple's.
* AudioToolbox/AudioUnit/CoreMIDI: pure user-space API surfaces on top of the HAL. `AudioConverter`,
  `ExtAudioFile`, `AudioFile` are the bulk (codecs: PCM, ALAC/AAC need Apple's closed codecs; Logic ships
  mostly PCM/CAF/AIFF). AUv2 component lookup works off bundle `Info.plist` (`AudioComponents`) and is
  straightforward; AUv3 needs XPC + `NSExtension`, which depends on Foundation XPC that does not exist yet.
* **Darling** (checked against `darlinghq/darling` master, GitHub API, not cloned): `src/frameworks/` has
  `CoreMIDI`, `AVFAudio`, `CoreAudioKit`, `Accelerate`, `AVFoundation`, `AudioVideoBridging` and
  `dev-stubs/AudioToolbox`; `CoreMIDI.m` is a generated stub that prints "not implemented" with
  `STUB_VERBOSE`. No `CoreAudio` or `AudioUnit` directory in `src/frameworks`. So Darling contributes
  **headers and exported-symbol lists (link-time stubs), not an implementation**. The three repos that
  `macapps/` builds (`darling-corefoundation`, `-foundation`, `-cocotron`) contain no audio. This lets
  Mac apps *link* (`macapps/machcheck.py` can list what is missing), but every call still needs our own
  code. Accelerate (vDSP) is big, mostly math; needs a real implementation (or Apple's if ever possible).

### Latency expectations for a DAW

A DAW wants an IO buffer of 32–128 frames at 44.1–96 kHz (< 3 ms), in a real-time thread that is woken by the
device clock with a safety offset of a few tens of frames. What we have is far from that:

* `RavynHDAudio` uses a **65536-frame ring** (1.36 s at 48 kHz) and **polls LPIB from `writePCM`**; there is no
  interrupt-driven position update used yet and the engine's timestamps are coarse. Interactive latency today
  is therefore on the order of **tens to hundreds of ms**, set by the write-ahead the writer chooses, not
  by hardware.
* Under QEMU the host adds its own buffering (CoreAudio/`wav` backend, `hda` timer 10 ms by default);
  realistically **20–50 ms** round trip in a VM is the floor, usable for a demo, not for tracking.
* On real hardware an HDA codec can do 128 frames/48 kHz (2.7 ms) if the driver uses BDL interrupts at that
  granularity, takes timestamps from them, and the scheduler can give the IO thread a real-time class. XNU 20.5
  has `THREAD_TIME_CONSTRAINT_POLICY`, so this is feasible; it needs the PureDarwin scheduler to behave on
  one core (`CLOKEOS_VM_SMP` = 1 today). *Untested.*
* Plug-in hosting out of process (AUv3) adds one buffer of latency per hop; Logic's default in-process AUv2
  path avoids that.

## 3. Smallest proof

**Result: first sound works on x86_64 (QEMU TCG), with a two-line driver fix.** The captured WAV of a 2 s
sine written to `/dev/dsp0` contains a 480 Hz tone with the written peak (20000), 2.03 s long, measured
from QEMU's `wav` backend. Fix: `kext/audio/ravynhda-amp.patch` (against the pinned PDSRC revision).
Cause, found with QEMU's `hda-duplex,debug=2` verb trace and `hw/audio/hda-codec.c`: QEMU keeps the amp
state **per stream**, the DAC and its output pin share it, and `RavynHDAudio` (a) sent gain 0 (the minimum, not
0 dB; the 0 dB step is the *offset* in the amp capabilities, 74 here) and (b) then sent the same amp verb to
the pin, which has no amplifier, which overwrote the DAC's gain again. The patch reads the capabilities
(parameter 0x12), programs the offset, and skips widgets without an amp. It also shrinks the `dsp0_write`
stack buffer to 2 KB (see the `___chkstk_darwin` note below). Not verified on real hardware, where
`SET_AMP_GAIN_MUTE` semantics are the same by spec but codecs differ.

**Built in:** `kext/build.sh` extracts `RavynHDAudio` (plus the `IOAudioFamily` and `IOPCIFamily` headers) from the
pinned PDSRC tarball, applies `kext/audio/ravynhda-amp.patch`, compiles with the same `cflags` as the other kexts
and fails if the binary references `___chkstk_darwin`. The kext replaces PureDarwin's `RavynHDAudio.kext` in the
kernel collection (`pd-rebuild-kc` list) and `brand.sh` installs it into the image like the other own kexts.
Checked with a full `brand.sh` desktop build (`CLOKE_NO_MACAPPS=1`): in a clone booted under TCG with
`-audiodev wav,id=snd0,path=out.wav,out.frequency=48000 -device intel-hda -device hda-duplex,audiodev=snd0`, a
480 Hz sine (peak 20000) written to `/dev/dsp0` was captured as peak 20000 / 479 Hz (zero-crossing estimate).
Still untested: real hardware, arm64 (no driver there).

### Earlier attempts (kept for the record)

**Done (x86_64 only, TCG):** booted the unmodified PureDarwin desktop image with
`-device intel-hda -device hda-output,audiodev=snd0` and `-audiodev wav,out.frequency=48000`, then wrote a
3 s, 440 Hz, 12000-peak s16le stereo sine to `/dev/dsp0` from the guest's `python3`.

* `open`/`write` succeed: all 576000 bytes accepted, the write blocks at roughly real time, and QEMU's WAV
  file grew to 4.6 s at 48 kHz. So the HDA stream is running and DMA is being consumed.
* **The captured WAV is all zeros (peak 0). No tone was heard or recorded, so this is *not* a working output
  path.** An earlier run with a 2 s tone and the default 44.1 kHz backend was likewise silent (and was cut
  off by quitting QEMU before the WAV was finalised).
* A second run with `hda-duplex`, a 480 Hz tone at peak 20000 and 2 s of 20000-peak data also gave an all-zero
  WAV (96549 frames). `dmesg` shows only `RavynHDAudio: published /dev/dsp0` from the driver (no codec
  enumeration lines at the default log level), so codec setup could not be confirmed from the log.
* **Third round, driver rebuilt from the pinned source** (x86_64, kernel collection rebuilt with the
  `kc-builder` recipe from `kext/build.sh`, only `RavynHDAudio.kext` replaced, installed into a cloned image's
  ESP, with register logging added). Findings, all with `intel-hda` + `hda-duplex` on QEMU:
  controller and codec init succeed (codec vendor 0x1af40022, DAC nid 2 -> pin nid 3, engine "active");
  the output stream descriptor is at `0x100` (4 input streams), `CTL=0x...100002` (RUN, stream tag 1),
  `FMT=0x11` (48 kHz/16 bit/stereo), `CBL=262144`, `LVI=3`; **LPIB advances while writing, the data
  the writer stages is non-zero, and the codec reads back `FMT=0x11`, stream/channel `0x10`, pin
  `CTL=0x40`, power D0**. So the controller DMA side and the stream routing are correct.
  The driver sets the DAC amp gain to 0 and calls it "0 dB"; in HDA the gain field is a step count from the
  minimum and the 0 dB step is the *offset* in the amp capabilities (QEMU's DAC: caps `0x80034a4a`, offset
  74). Gain 0 is the minimum, which QEMU's codec maps to silence. A change that reads the capability and
  programs the offset (74) was built, **but the amp readback still returned 0 and the WAV was still silent
  (peak 0)**, so either the set verb is not taking effect (readback form or ordering unverified) or there is
  a second cause. Read the `hda-codec` source of the installed QEMU to see how it maps the verb to the
  host volume; that is the next step.
* Side finding: building this kext with the host clang makes `dsp0_write` (4 KB stack buffer) call
  `___chkstk_darwin`, which does not exist in the kernel; the first write panics (GP fault) – use frames
  under 4 KB (the image's own binary does not have this). The driver changes were **not** committed.
* Not determined why. Candidates, none checked: the image's `RavynHDAudio` binary is older than the source
  that was read here (image 23.08., source rev 1bbaeb2 25.08.; the source has fixes for garbled playback);
  codec routing/amp verbs not taking effect on QEMU's `hda-output` codec; ring data written but not seen by
  DMA; or `python3`'s unbuffered write path. Next step: boot with `IOLog` visible (`dmesg | grep Ravyn`),
  read back the SD registers, try `hda-duplex`, and rebuild `RavynHDAudio` from the pinned source with
  `kext/build.sh` (x86_64) to rule out the stale binary.
* arm64: nothing to test, there is no driver in the kernel (section 1). Missing there, exactly:
  (1) `IOAudioFamily` and `RavynHDAudio` built for arm64 and added to the arm64 kernel/`pd-rebuild-kc`
  equivalent (no arm64 kext build path in `kext/build.sh`), (2) a driver for something QEMU `virt` offers
  that is not PCI-HDA-specific-risky: `virtio-sound-pci` (1af4:1059, no PureDarwin driver, would be a new
  ~500-line `IOAudioEngine` over the virtio queues, `IOVirtIOFamily` exists) is the natural one.
  `intel-hda` also works on `virt` in QEMU, so (1) alone may be enough for a first sound, *untested*.
* Not done: any user-space client of `IOAudioEngineUserClient`, any CoreAudio API, capture, MIDI.

## 4. User-space Core Audio shim (`macapps/coreaudio/`)

**Result: a Mac program that links Apple's `CoreAudio.framework` from the SDK plays sound on an x86_64 ClokeOS
VM through public HAL calls only.** `macapps/coreaudio.sh` builds `CoreAudio.framework` (own C code, no Apple
code, `CoreAudio.c`, x86_64 and arm64; arm64 builds but was not run) into the `MacFrameworks` directory next to
Darling's CoreFoundation (it needs it for CFString properties) and `satest.c`, a test tool built against the SDK
that plays a 2 s, 440 Hz Float32 sine. Run in the VM with `DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks`.

Implemented (signatures checked against the SDK headers; the framework exports exactly these):
`AudioObjectHasProperty`, `AudioObjectIsPropertySettable`, `AudioObjectGetPropertyDataSize`,
`AudioObjectGetPropertyData`, `AudioObjectSetPropertyData` (buffer frame size only), `AudioDeviceCreateIOProcID`,
`AudioDeviceDestroyIOProcID`, `AudioDeviceStart`, `AudioDeviceStop`. Objects: system object (1), one device (2),
one output stream (3). Properties: system `Devices`, `DefaultOutputDevice`, `DefaultSystemOutputDevice`; device
name, manufacturer, UID, class, owner, transport type (built-in), alive, running, nominal rate (48000, the only
available rate), `BufferFrameSize` (default 512, settable 32..4096) and its range, latency and safety offset (0),
`Streams`, `StreamConfiguration` (1 buffer, 2 channels); stream `VirtualFormat` = Float32 packed interleaved
stereo (what the IOProc fills), `PhysicalFormat` = signed 16-bit packed stereo (what `/dev/dsp0` takes), direction,
starting channel. `AudioDeviceStart` opens `/dev/dsp0` and starts one thread (time-constraint policy requested,
result unchecked) that loops: zero a Float32 buffer, call every registered IOProc (summed), clip and convert to s16,
`write()` the block. The block size is the buffer frame size; `write()` blocking on the driver's ring is the only
clock. `CLOKE_CA_DEVICE=<file>` redirects the output to a file, `CLOKE_CA_STATS=1` prints the IOProc spacing.

Measured (x86_64, QEMU TCG, `RavynHDAudio` with `ravynhda-amp.patch`, `hda-duplex` and the `wav` backend,
512-frame blocks = 10.667 ms):
* WAV on the host: 440.0 Hz, peak 16384 (= 0.5 · 32767, exact), L = R. The WAV was 2.29 s long although the
  IOProc was called for 345 blocks (3.68 s): the first ~1.4 s the driver accepts without waiting (its ring is
  262144 bytes = 1.365 s) and **`AudioDeviceStop` (close of `/dev/dsp0`) drops what is still in the ring**, about
  131 blocks.
* IOProc spacing in the second half of the run: mean 10.56 ms (nominal 10.667), i.e. the period follows the
  48 kHz device clock; the first half is faster (ring fill, mean ~2.6 ms). Over the whole run: min 0.28, max 79.8 ms
  (TCG with other VMs running; a max over one period means the thread was not scheduled in time and would be an
  audible dropout with a real ring of a few ms).
* Latency: the output latency is the driver's ring, ~1.36 s, not the 10 ms block. No lower figure can be measured
  until the driver's ring is shortened (`CBL`/`LVI` in `RavynHDAudio`). The shim reports latency 0 and a
  timestamp one block ahead, which is not true.

Missing for Logic Pro (see `docs/logic-pro-gap.md`): everything above the HAL (AudioUnit, AudioToolbox,
AudioComponent, AUGraph, ExtAudioFile, CoreMIDI); input streams (the driver is write-only); more than one device or
format, sample-rate and format setters, hog mode, aggregate devices; property listeners
(`AudioObjectAddPropertyListener`); real device clock (`AudioDeviceGetCurrentTime`, `AudioDeviceTranslateTime`,
`AudioGetCurrentHostTime`/`HostTime.h` functions; timestamps here are the thread's `mach_absolute_time`, sample
time counts blocks written, not frames played); `AudioDeviceCreateIOProcIDWithBlock`; correct latency and safety
offset; a tight ring in the driver (a DAW wants 64..256 frames end to end); a drain on stop; a one-shot
multi-client mixer (two apps open `/dev/dsp0` today? untested); real-time thread behaviour with SMP (the VM runs
one CPU).

Not tested: arm64 (no audio driver there), real hardware, other apps than `satest`, `kAudioObjectPropertyName`
string retrieval (compiled, not called by `satest`), `SetPropertyData`, several IOProcs, run on the shipped desktop
image (test used the plain PureDarwin image plus the amp-patched kernel collection from another session).

## 5. Accelerate subset (`macapps/accelerate/`)

`macapps/accelerate.sh` builds `Accelerate.framework` (own C code, no Apple code; x86_64 and arm64, plain C with
`-O3`, no SIMD intrinsics; reductions use `#pragma clang fp reassociate`) into `/Library/ClokeOS/MacFrameworks`
(also called from `macapps/install.sh`). It needs only PureDarwin's libSystem; `ccompat.c` supplies `__muldc3`,
`__divdc3`, `cabs`, `csqrt`, which that libSystem lacks. Source files: `vdsp.c` (+ templates `vdsp_t.h`, `vdsp_c.h`
for float/double), `vforce.c`, `blas.c` (32-bit CBLAS), `lapack.c` (`name$NEWLAPACK$ILP64` LAPACK + `cblas_dgemv`),
`vimage.c`. Every file is compiled against the SDK headers, so signatures are checked by the compiler (the double
variants are not all declared in the headers).

**Logic Pro 12.3 imports** (`dyld_info -imports` over the 13 Mach-O files of the app that import Accelerate,
x86_64): 179 symbols, 177 C symbols + 2 Swift overlay symbols. **Implemented: 161 of the 177 C symbols** (all
`vDSP_*` but `wiener`, `vvexp/vvexpf/vvexp2f/vvlog/vvlog2f/vvlogf/vvsqrt/vvsqrtf/vvfloorf/vvfmodf`, `vfabsf`,
`cblas_sgemv/scopy`, `cblas_dgemv$NEWLAPACK$ILP64`, `dgeev/dgesv/dgesvd/dgetrf/dgetri/dgetrs/dsysv/zcgesv`,
five of the seven vImage functions). About 120 more symbols are exported (D variants, more vForce, cblas
dot/axpy/gemm, biquad, `vDSP_fft_zropt`...), none checked against Logic.

**Missing:** the 13 `BNNSGraph*` functions (Core ML graph runtime, not attempted), `vDSP_wiener`,
`vImageBuffer_InitWithCGImage` / `vImageCreateCGImageFromBuffer` (need CoreGraphics), the 2 Swift symbols
(`vDSP.maximumMagnitude`, `UnsafeBufferPointer` conformance, from the Swift overlay), FFT radix 3 and 5
(`vDSP_create_fftsetup` returns NULL), `vDSP_biquadm*`, 2D FFT (`fft2d_*`), DCT, non-`NEWLAPACK` LAPACK names.

**Semantics are not guessed:** `acctest` runs ~170 result groups; run on the Mac host once against Apple's
Accelerate (`-framework Accelerate`) and once against our code, `acctest-cmp.py` finds **0 of 168 groups
different** (float tolerance 2e-5, FFT 1e-4). The first run found real differences that were fixed from Apple's
behaviour: `mmul` argument order (M,N,P = A is MxP), `nzcros` returns 0 as index if fewer than the requested count
were found, `vqint` nodes (j-1,j,j+1), `imgfirD` leaves a P/2 border zero, real FFT scale (forward = 2x the DFT).
Not a bit-for-bit match: rounding of float sums (order of the additions) and Bunch-Kaufman vs LU in `dsysv`.
`dgesvd` (one-sided Jacobi), `dgeev` (complex Hessenberg QR + inverse iteration; eigenvalue order differs from
LAPACK, left/right vectors in LAPACK's normalisation) and `dsysv` (LU, so `ipiv` is not Bunch-Kaufman) are
checked by residuals, not against Apple output; `dgeev` is for small, non-defective matrices.

**Speed** (`acctest bench`, 16384 floats; native Mac host M-series, our code vs a naive loop in the same compiler
output; Apple's Accelerate for comparison): vadd 17.6 vs 18.7 GElem/s (memory bound), vsmul x2.1, dotpr 11.1 vs
1.2 (x9), sve x20, conv with 64 taps x7.7, sgemm 128x128 3.9 vs 1.1 GFLOPS (x3.6), FFT 4096 complex 102 µs.
Apple's version is much faster on the host (sgemm 626 GFLOPS via the matrix coprocessor, FFT 6.6 µs, dotpr 20
GElem/s), so these are correctness-first numbers; a real-time DSP load needs SIMD kernels and a split-radix or
radix-4 FFT. In the x86_64 QEMU TCG VM (`acctest bench`; absolute numbers say little): vadd x1.9, dotpr x1.8, sve
x2.3, conv x1.7, sgemm x1.4 vs naive. In the arm64 VM (HVF, near native): vadd x1.6, dotpr x15, sve x19, conv x6.4, sgemm 11.7 vs 1.8 GFLOPS (x6.4), FFT 4096 35 µs.

**Tested:** native host (x86_64 and arm64 build against Apple's SDK; Mac run is arm64); `acctest` self-tests
(naive references: elementwise ops, FFT vs direct DFT, mmul, conv, biquad, BLAS, LU/SVD/eigen residuals) in the
x86_64 and the arm64 ClokeOS VM: **all pass** (plain PureDarwin minimal image plus the framework via `CLOKE_EXTRA_STAGE`, which
`brand.sh` now accepts). Run: `DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks /Library/ClokeOS/Probe/acctest`
(`dump`, `bench` as arguments). **Not tested:** Logic itself, the `vDSP_*` calls with its real data and strides,
thread safety of the FFT setup (read-only after creation, so it should be fine), the full dump comparison inside
a VM (only the self-tests ran there; the arm64 VM used the minimal arm64 image the same way), real hardware.

## 5. User-space AudioToolbox subset (`macapps/audiotoolbox/`)

**Result: a Mac program linked against Apple's SDK `AudioToolbox.framework` writes a WAV/AIFF/CAF through
`ExtAudioFile`, reads it back, loads an AUv2 plug-in from a `.component` bundle and plays through the
DefaultOutput unit on an x86_64 ClokeOS VM; QEMU's `wav` backend captured 440.12 Hz with L peak 16384 and R peak
8192 (written 0.5 and 0.25).** `macapps/audiotoolbox.sh` builds `AudioToolbox.framework` (own C code, no Apple
code; `AudioToolbox.c` component registry + AudioUnit API + output unit, `convert.c` AudioConverter,
`file.c` AudioFile/ExtAudioFile/AudioFormat), the test tool `attest` (public API only) and `ctgain.component`
(a test AUv2 effect). It needs `foundation.sh` and `coreaudio.sh` first; x86_64 and arm64 build, arm64 was not run.
Run in the VM: `zsh /Library/ClokeOS/Probe/at.sh self` (everything, output unit goes to the HAL) or
`at.sh play 3`. A test program must link **Foundation** (or anything that triggers Darling's `CFInitialize`):
with only `-framework CoreFoundation` the Darling CF is not initialised (`kCFAllocatorDefault == NULL`, every
`CFURLCreate*`/`CFStringCreate*` returns NULL or crashes). Real Mac apps link Foundation, so this is a test-tool
detail. VM test set-up: QEMU `-audiodev wav,... -device intel-hda -device hda-duplex` (the "Can not open adc"
line is only a warning), and an image whose kernel collection contains the amp-patched `RavynHDAudio` (a
desktop image built before 1dc2abd is silent: the 2.6 image tested first gave peak 0 for both `satest` and
`attest`; swapping `EFI/BOOT/kernel` for the patched collection fixed it).

Design: as in Apple's, a component is a factory returning an `AudioComponentPlugInInterface` (Open/Close/Lookup);
every `AudioUnit*` call looks up the selector and calls the plug-in method, so built-in units and AUv2 bundles
take the same path. Registry: built-ins + `Info.plist` `AudioComponents` (type/subtype/manufacturer as string or
number, `name`, `version`, `factoryFunction`; the bundle executable is `dlopen`ed on first instance) scanned in
`/Library/Audio/Plug-Ins/Components`, `/System/Library/Components`, `~/Library/Audio/Plug-Ins/Components` and
`$CLOKE_AU_PATH` (colon list, tests).

Implemented (62 of the 95 non-QuartzCore AudioToolbox/CoreAudio symbols Logic Pro 12.3 imports, counted with
`nm -u` over its binary and frameworks): `AudioComponentFindNext/Count/GetDescription/GetVersion/CopyName/
Register/Instantiate/InstanceNew/InstanceDispose/InstanceCanDo/CopyConfigurationInfo(stub)`,
`AudioUnitInitialize/Uninitialize/GetPropertyInfo/GetProperty/SetProperty/GetParameter/SetParameter/Render/Reset/
ScheduleParameters/AddPropertyListener/RemovePropertyListenerWithUserData/AddRenderNotify/RemoveRenderNotify`
(all forwarded to the plug-in), `AudioOutputUnitStart/Stop`, `MusicDeviceMIDIEvent/SysEx`, `OpenDefaultAudioOutput`;
built-in output unit for `DefaultOutput`, `SystemOutput` and `HALOutput` ('def ', 'sys ', 'ahal'): input format any
PCM, render callback or `kAudioUnitProperty_MakeConnection` source, `MaximumFramesPerSlice`, `IsRunning`,
`CurrentDevice` (only device 2), `EnableIO` (output on, input off), `kHALOutputParam_Volume`; not 48 kHz/Float32/
stereo goes through an `AudioConverter`. `AudioConverterNew/Dispose/Reset/FillComplexBuffer/ConvertBuffer/
GetProperty/GetPropertyInfo/SetProperty`: PCM to PCM, 8..32 bit int (packed or in a larger container, either
endianness, unsigned 8) and Float32/64, interleaved or not, any channel count (mono to all, to mono = mean, else
copy and zero), linear sample-rate conversion. `AudioFileCreateWithURL/OpenURL/OpenWithCallbacks/
InitializeWithCallbacks/Close/Optimize/ReadBytes/WriteBytes/ReadPacketData/ReadPackets/WritePackets/
Get/SetProperty/GetPropertyInfo` and (empty) user-data calls for WAV (PCM, float, extensible), AIFF, AIFC
(`NONE`/`sowt`/`fl32`/`fl64`) and CAF (`lpcm`); `ExtAudioFileOpenURL/CreateWithURL/WrapAudioFileID/Read/Write/
WriteAsync/Seek/Tell/Dispose/Get/SetProperty` with client data format conversion; `AudioFormatGetProperty(Info)`
for PCM (FormatInfo, FormatName, IsVBR, IsExternallyFramed).

Measured: all checks of `attest self` pass identically on the macOS host (also under AddressSanitizer, against
the same sources) and in the x86_64 VM: 8 files (WAV 16/24/float32, AIFF 16/24 at 44.1 kHz, AIFC, CAF 16/float32)
write, re-read with the right format, length, peak and 440 Hz; 48 kHz stereo to 44.1 kHz mono Int16 gives 22050
frames for 0.5 s (peak 0.375 = mean of the two channels), to 96 kHz non-interleaved Float32 47999 of 48000 frames;
big-endian Int16 `ConvertBuffer` exact; AAC is refused with `kAudioConverterErr_FormatNotSupported`; the AUv2 test
plug-in is found by type/subtype/manufacturer, name and version read from the plist, parameter and render work and
`Render` before `Initialize` returns `kAudioUnitErr_Uninitialized`. Output unit in the VM: 48 kHz Float32 and
44.1 kHz mono Int16 input both reach the HAL (IOProc spacing in the second half 10.5 ms, as in section 4).

Missing: AUv3/`AUAudioUnit`, `AUGraph`, `AudioQueue` (Logic imports 6 calls), `AudioFileStream`, `MusicSequence`/
`MusicTrack`/`MusicPlayer`, `AUEventListener`/`AUParameter*`/`AUListener*`, `AudioServices`, `AudioComponentCopy
ComponentURL`, `AudioFileOpen` (the older callback-less variant name Logic imports), `MusicDeviceMIDIEventList`,
property listeners on built-in units, `AudioObjectAdd/RemovePropertyListener` (CoreAudio shim), the CAF/AIFF
user-data chunks, `kAudioFilePropertyChannelLayout` (accepted, not stored), channel layouts in the converter,
codecs (AAC, ALAC, MP3, FLAC; every non-PCM format), priming/dither, a better rate converter than linear (no
anti-alias filter: downsampling aliases), mixer and effect units (`MultiChannelMixer`, `AUDelay`, ...; Logic ships
no AUs, third-party AUv2 bundles bring their own), 64-bit-offset RF64/W64, the real-time safety of the IO path
(`malloc` in `FillComplexBuffer` per call; fine for a test, not for a DAW with 64-frame blocks).

Not tested: arm64 (builds only), real hardware, third-party AUv2 plug-ins (only our own `ctgain`), threads calling
the unit API concurrently, `AudioComponentRegister` with an in-process factory (code path shared with the
built-ins, no test), files over 2 GB, big-endian or 8-bit files other than via the round trip, the
`AudioFileOpenWithCallbacks` path, `Seek` with sample-rate conversion.

## 6. CoreMIDI (`macapps/coremidi/`, 2026-11-14)

`macapps/coremidi.sh` builds `CoreMIDI.framework` (install name and version of Apple's, 105 exports) from
`macapps/coremidi/CoreMIDI.c` and replaces the stub framework from `stubs.sh` (`install.sh` runs it after the stubs).
There is no MIDI server and no driver: everything happens inside the process, as if only Apple's IAC-style virtual
endpoints existed.

* Clients (`MIDIClientCreate`, `…WithBlock`) with notifications: creating or disposing a virtual endpoint sends
  `kMIDIMsgObjectAdded`/`Removed` and `kMIDIMsgSetupChanged` to every client, delivered on the main run loop.
* Input ports (read proc, read block, receive block with protocol), output ports, `MIDIPortConnectSource`/`Disconnect`.
* Virtual sources and destinations (all create variants, also with protocol); `MIDIGetNumberOfSources/Destinations`,
  `MIDIGetSource/Destination` list them in creation order. No devices or entities (`MIDIGetNumberOfDevices` = 0,
  `MIDIEndpointGetEntity` = `kMIDIObjectNotFound`, as Apple answers for a virtual endpoint).
* `MIDIReceived(EventList)` goes to every input port connected to the source, `MIDISend(EventList)`/`MIDISendSysex` to
  the destination's read callback, on a delivery thread of its own (in order; Apple also delivers on a separate thread,
  and synchronous delivery could deadlock a program that is both source and reader). Time stamps are not waited for.
* MIDI 1.0 packet lists and UMP event lists are converted in both directions with the packet boundaries kept: MIDI 1.0
  channel voice as UMP type 2, system messages as type 1, SysEx as type 3, and MIDI 2.0 channel voice (type 4) with the
  spec's min-center-max scaling (7 to 16 and 32 bit, 14 to 32 bit) for ports and endpoints created with protocol 2.0.
* `MIDIPacketListInit/Add` and `MIDIEventListInit/Add` follow Apple (same time stamp and no SysEx: appended to the current
  packet; 4-byte packet alignment on arm64 through the SDK's `MIDIPacketNext`; NULL when the list is full).
* Properties are a dictionary per object (name, unique ID, protocol, anything set by the program); `displayName` falls
  back to `name`; `MIDIObjectFindByUniqueID`; errors as Apple's (`kMIDIUnknownProperty`, `kMIDIWrongPropertyType`, …).
* `CLOKE_MIDI_TRACE=1` logs clients, ports, endpoints and connections.

**Checked.** `macapps/coremidi/mtest.c` (43 lines of output: packet and event lists, notifications, properties, unique IDs,
error codes, delivery to read procs and receive blocks in MIDI 1.0 and 2.0, sending to destinations) gives the same output
against Apple's CoreMIDI on the Mac (three runs), against `CoreMIDI.c` compiled for the Mac, and in the arm64 VM against
the framework (`/Library/ClokeOS/Probe/mtest`). Logic Pro 12.3 in the arm64 VM: the "Error initializing CoreMIDI / MIDI
Services not available" alert is gone; Logic creates a client, an input and an output port, a virtual source and a virtual
destination, and opens `Jump.logicx`.

Found on the way: a program that links only CoreFoundation (no Foundation) crashes in Darling's CoreFoundation on arm64
at the first `CFDictionaryCreateMutable` (even with NULL callbacks); with Foundation linked it works. Mac apps always load
Foundation, so `mtest` links it; the cause is not investigated.

Not done / untested: MIDI hardware (no USB MIDI class driver in PureDarwin), scheduling by time stamp, `MIDIThruConnection`,
`MIDISetup`, MIDI-CI and the UMP endpoint APIs, MIDI 2.0 messages other than channel voice when converting down, x86_64 in
the VM (builds only).

### MIDI between programs: `clokemidid` and the MIDI Keyboard (2026-11-14)

macOS has a MIDI server that makes the virtual endpoints of one program visible to all others. ClokeOS has
`clokemidid` (`macapps/coremidi/clokemidid.c`, built by `coremidi.sh` into `/usr/libexec`, launchd job
`com.clokeos.midiserver`, socket `/var/run/clokemidi.sock`, mode 0666). The protocol is in
`macapps/coremidi/clokemidi-proto.h`: a fixed header (length, type, kind, unique ID, format) plus payload; endpoints are
addressed by their MIDI unique ID. Messages: ADD/REMOVE an own endpoint, SUB/UNSUB a foreign source, DATA (MIDI from an own
source, fanned out to subscribers), SEND (MIDI to a foreign destination, forwarded to its owner), EP_ADD/EP_REMOVE (the
service announces endpoints of other programs; all existing ones when a program connects). Payload formats: raw MIDI 1.0
bytes, `MIDIPacketList` or `MIDIEventList`.

CoreMIDI connects at the first `MIDIClientCreate` (`CLOKE_MIDI_SOCKET` overrides the path; without the service everything
stays in the process as before). Own virtual endpoints are registered; foreign ones appear as proxy sources and
destinations with the usual notifications, so `MIDIGetSource`/`MIDIGetDestination` list them. Connecting an input port to a
foreign source subscribes (once per source), `MIDISend` to a foreign destination goes through the service, `MIDIReceived`
on an own source also goes to the subscribers in other programs; `MIDIReceived` on a foreign source and disposing it answer
`kMIDINotPermitted`. A reader thread per process takes the service's messages.

**MIDI Keyboard** (`clokeapps keyboard`, `/System/Applications/Utilities/MIDIKeyboard.app`, icon
`cloakserver/icons/midikeyboard.svg`): two octaves to play with the mouse (also gliding over keys) or the computer keyboard
(A W S E D F T G Y H U J K O L P like Logic's Musical Typing, Z/X octave, C/V velocity), octave buttons and a velocity slider.
It speaks the protocol directly (GTK program, no CoreMIDI) and registers the source "ClokeOS Keyboard". It ignores SIGSYS:
on arm64 PureDarwin has no SysV shared memory and Cairo's XShm attempt otherwise killed the program.

**Checked.** On the Mac: two processes over `clokemidid` (virtual source in one, input port in the other; `MIDISend` to a
destination in the other process; raw bytes from a protocol-only client) all arrive; `mtest` still matches Apple. In the arm64
VM with Logic Pro 12.3: `clokemidid` starts at boot; Logic finds "ClokeOS Keyboard" as soon as the keyboard starts and
connects its input port to it by itself; eight held keys (C major scale, QMP key events with 450 ms hold) arrive in Logic as
note on/off, the meters of the selected track ("Bright Synth Strings") and of the output move, and the CoreAudio shim's output
file (`CLOKE_CA_DEVICE`) has 7.5 s of sound (peaks about 0.2) exactly while playing and silence before and after.

Not checked: x86_64, recording the notes into a region, several programs sending at once, latency (delivery is immediate but
goes through two sockets and the receive thread), MIDI out from Logic to another program, the Keyboard app from Launchpad as
the guest user (started from the console in the test).
