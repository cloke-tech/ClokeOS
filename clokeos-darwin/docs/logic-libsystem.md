# Logic Pro 12.3 and PureDarwin's libSystem (packages C and Q)

Date: 2026-10-30. Image: `clokeos-2.7-arm64-desktop.img` (PureDarwin 20.5, XNU arm64 virt, QEMU with HVF, 1 CPU, 16 KiB pages), cloned,
patched with `macapps/dlprobe.sh` and booted headless; no full `brand.sh` image was built. Subject: the user's own
`/Applications/Logic Pro.app` (read in place, thinned with `lipo -thin` into scratch, nothing of it is in the repository).
Companion to `docs/logic-pro-gap.md` (packages C and Q) and `docs/logic-dyld.md` (test ladder steps 2 and 3).

## 1. What was missing, exactly

`libcompat/missing.py APP ARCH PROVIDER.dylib…` lists every symbol that a Mach-O file imports from `libSystem` and that none of
the providers exports (`dyld_info -imports/-exports`, all 98 Mach-O files of the app). Providers = `libSystem.B.dylib` +
`libdyld.dylib` (the only library `libSystem.B` re-exports) of the base image + the old `libclokeos_compat`.

| Slice | Imported from libSystem (unique) | Missing in PureDarwin alone | Missing with the `libcompat` that was on main | Missing now |
|---|---:|---:|---:|---:|
| arm64 | 549 | 65 | 57 | **0** |
| x86_64 | not counted | 69 | not counted | **0** (static only) |

The estimate "82 names" in `logic-pro-gap.md` was an upper bound (sub-libraries not followed). Things that estimate listed
as missing are **not**: `_dyld_get_image_*`, `_dyld_image_count`, `__tlv_bootstrap`, `__tlv_atexit`, `_availability_version_check`,
`___isPlatformVersionAtLeast`, `dispatch_block_create` and `thread_policy_set` are exported by PureDarwin's `libdyld`/`libSystem`.
The availability functions are exported but **wrong** (section 3).

The 57 arm64 names, by what they needed (all implemented in `libcompat/`, own code):

| Names | Where | How |
|---|---|---|
| `os_workgroup_join/leave/cancel/max_parallel_threads`, `os_workgroup_interval_start/update/finish` (plus `testcancel`, `create_with_workgroup`, `interval_create`, `parallel_create`, arena calls, `copy_port` = ENOTSUP) | `workgroup.c` | real ObjC objects of a class created on first use (`dlsym` into libobjc, so `os_retain/release` work); bookkeeping in user space, errors as in `<os/workgroup_*.h>` and as measured on the Mac (section 4); **no kernel work interval** |
| `thread_policy`, `thread_policy_get` | `misc.c` | hand-built MIG messages (thread_act ids 3616/3618, read from PureDarwin's `thread_policy_set`); `thread_policy_set` was already there |
| `pthread_qos_max_parallelism`, `pthread_time_constraint_max_parallelism`, `semaphore_signal_all` | `misc.c` | `hw.activecpu`/`hw.physicalcpu`; `semaphore_signal_all_trap` |
| `dispatch_barrier_async` (+`_f`, `barrier_sync_f`), `dispatch_block_cancel` (+`testcancel`), `dispatch_block_create_with_qos_class`, `dispatch_get_current_queue`, `dispatch_get_specific`, `dispatch_queue_set_specific` | `dispatch.c` | PureDarwin's libdispatch (1292) was linked with an export list; these were dead-stripped. Barrier: internal `_dispatch_barrier_async_detached_f`, its offset to `dispatch_barrier_sync` is computed by `build.sh` from the very `libSystem.B` it patches. Block cancel: `DBF_CANCELED` at block+48 (read from `_dispatch_block_async_invoke2`). Current queue: TSD slot 20; target chain: `do_targetq` at +24 |
| `_OBJC_CLASS_$_OS_dispatch_queue`, `_OS_dispatch_source`, `_OS_os_log` | `exporttrie.py` | the classes exist in libSystem but as local symbols. The script rewrites libSystem's export trie (all local `_OBJC_CLASS_$_OS_*` get exported, the `__LINKEDIT` tail moves, signature dropped and re-made). `OS_os_log` does not exist in PureDarwin at all: the symbol is an alias of `OS_object` |
| `os_signpost_enabled`, `_os_signpost_emit_with_name_impl` | `dispatch.c` | disabled / no-op |
| `CC_MD5`, `CC_SHA1` | `misc.c` | one-shot wrappers over the Init/Update/Final that `system.c` already had |
| `OSAtomicEnqueue/Dequeue` (lock-free, 128-bit ABA counter: `ldaxp/stlxp` on arm64, `cmpxchg16b` on x86_64), `OSAtomicFifoEnqueue/Dequeue` (short spin lock), `__atomic_load/store/exchange/compare_exchange` (lock table) | `misc.c` | |
| `strtonum`, `uuid_parse`, `srand48/drand48` family, `sranddev`, `vsscanf_l`, `sqrtf`, `removefile` (+state alloc/free), `vm_page_mask` | `misc.c` | `vm_page_mask` is a constant (0x3FFF arm64, 0xFFF x86_64; a constructor could run before libSystem is initialised) |
| `kevent64`, `fsgetpath`, `getattrlistbulk`, `searchfs` | `misc.c` | straight system calls (measured: `kevent64` and `getattrlistbulk` work; `fsgetpath` and `searchfs` return ENOTSUP from the ext4 driver) |
| `___cospi`, `___muldc3`, `___mulsc3`, `___powidf2`, `___powisf2`, `___isinfd/f`, `___isnand`, `__invert_d3`, `_matrix_identity_double3x3`, `__simd_fma_f4/pow_f4/round_f4` | `mathx.c` | |
| x86_64 only: `fmaxf fminf llroundf lrintf`, `sinl cosl atanl exp2l fmodl` (x87), `___divdc3 ___divsc3` | `mathx.c` | |

## 2. Layout of the patch

`libcompat/build.sh <stage>` now builds, besides `libclokeos_compat.dylib` (re-exported by `libSystem.B`), a patched
`/usr/lib/libSystem.B.dylib` (extra re-export, extra exports) and a patched `/usr/lib/system/libdyld.dylib` (two names hidden).
New scripts: `missing.py` (the list above), `exporttrie.py` (add exports for local symbols), `hidesym.py` (hide an export by
changing the last byte of its trie edge to `~`, no shifting; used for `__availability_version_check` and `___isPlatformVersionAtLeast`
in `libSystem.B` and `libdyld`, so dyld falls through to `libclokeos_compat`). `brand.sh` copies the stage as before.

## 3. Mac identity (package Q)

**Measured before the change.** `kern.osproductversion` is empty on XNU 20.5, `SystemVersion.plist` says 11.7, and PureDarwin's
`_availability_version_check`/`___isPlatformVersionAtLeast` return 0 for **every** question, including macOS 11.7. Any `@available`
check in a program would take the "old OS" path or crash.

**What `libcompat/version.c` reports.** The larger of
1. `ProductVersion` of `/System/Library/CoreServices/SystemVersion.plist` (11.7),
2. the highest macOS minimum (`LC_BUILD_VERSION minos`) of all loaded images (so a process that loaded Logic's code, `minos 15.6`, sees 15.6,
   while Big Sur tools keep seeing 11.7),
3. `CLOKEOS_MACOS_VERSION=15.6` in the environment replaces 1 and 2.
Mac Catalyst (platform 6) compares with iOS 18.6; other platforms are ignored (answer "yes"), as dyld does.

**Why `SystemVersion.plist` stays at 11.7 by default.** `config.sh` documents that Homebrew and `sw_vers` read it; with 15.6 Homebrew would
pick Sequoia bottles for a Darwin 20.5 system. The knob is there: `CLOKEOS_MACOS_COMPAT=15.6 ./brand.sh` (was a fixed `11.7` in `config.sh`).
Measured in the VM with the plist at 15.6: `sw_vers` prints `ProductVersion 15.6`, `ClokeOSVersion 2.7`, `BuildVersion 2E3`; `uname -r` still `20.5.0`;
`__isPlatformVersionAtLeast(1, 15, 6, 0)` = 1, `(15, 6, 1)` = 0, `(26, 0, 0)` = 0. With the default plist and `libcom-api.dylib` (minos 15.6) loaded: 15.6 = 1, 26.0 = 0.
`BuildVersion` stays ClokeOS' (`2E3`), not Apple's `24G84`.

**Not covered:** Darling's Foundation (`NSProcessInfo.operatingSystemVersion`, `isOperatingSystemAtLeastVersion:`) reads the plist through CoreFoundation,
not these functions; Logic's own checks through it see 11.7 unless the image is built with `CLOKEOS_MACOS_COMPAT`.

**`Versions/C`.** Nothing to add: `macapps/foundation.sh` and `appkit.sh` already build `Foundation.framework/Versions/C/Foundation` and
`AppKit.framework/Versions/C/AppKit` with the install names Logic uses, under `/Library/ClokeOS/MacFrameworks`. Measured: `dlopen("MAFiles")`
fails with `Library not loaded: /System/Library/Frameworks/Foundation.framework/Versions/C/Foundation` without
`DYLD_FRAMEWORK_PATH=/Library/ClokeOS/MacFrameworks`, and gets past Foundation with it (dyld keeps the `Foundation.framework/Versions/C/…` tail). A
symlink under `/System/Library/Frameworks` would not help: Darling's Foundation needs Darling's CoreFoundation, which must stay out of
`/System/Library/Frameworks/CoreFoundation.framework` (PureDarwin's daemons use that).

## 4. Tests (arm64 VM, all passed unless stated)

Sources in `libcompat/test/`. Reference runs on the Mac (macOS 27, real libSystem) with the same program.

- **`dlsym-list.c`** + a list of the 549 libSystem symbols all 98 files import: `dlsym` on a `dlopen("/usr/lib/libSystem.B.dylib")` handle (the lookup dyld
  does, through the re-export chain): **549 resolved, 0 missing**. This is the answer to "do Logic's libSystem imports resolve"; each of the 43 files that
  imported a missing name needs only names from that list.
- **`libsystem.c`** (59 output lines, same program on the Mac and in the VM). Identical except: `signpost_enabled` (Mac 1, here 0 by design),
  `__isPlatformVersionAtLeast` for 12.0+ (1 on the Mac; here 11.7 for a minos-11 program, section 3), residual `errno` after `removefile`, and the
  time-constraint readback and `pthread_qos_max_parallelism(0x21)` equal the time-constraint value here but not on the Apple-silicon kernel.
  Covers: MD5/SHA-1 vectors, LIFO/FIFO queues, 24-byte `_Atomic`, `strtonum`, `uuid_parse`, `rand48` sequence, complex `inf` multiplication,
  `powi`, `__cospi`, `simd_inverse`, simd `fma/pow/round`, **barrier order** (two concurrent blocks, then the barrier, then a block after it: `2` and `3`),
  `dispatch_queue_set_specific`/`get_specific` including through a target queue, `dispatch_get_current_queue`, **`dispatch_block_cancel` (the block did not run)**,
  `removefile` flags (a directory without `REMOVEFILE_RECURSIVE` is left alone and returns 0: Apple's behaviour, found by test), `semaphore_signal_all` releasing 3 waiters,
  workgroup join/leave/cancel/interval start/update/finish return codes (`EALREADY` 37 on a second join, `EINVAL` 22 after cancel, on a second start, an update or finish
  after finish, a deadline before the start).
- **`classes-rt.c`**: `OBJC_CLASS_$_OS_dispatch_queue`/`_source`/`_object` from libSystem are the same pointers as `objc_getClass()` returns; a real queue's class chain
  (`OS_dispatch_queue_serial` → …) contains the exported `OS_dispatch_queue`. `OS_os_log` points to `OS_object` (alias, see gaps).
- **Time-constraint policy honoured by XNU 20.5** (`RT=1 classes-rt`, 1 CPU, a busy-looping normal thread, a thread waking every 2 ms, 300 wake-ups):

  | | median late | 95 % | max |
  |---|---:|---:|---:|
  | normal thread | 408 µs | 76.7 ms | 98.5 ms |
  | after `thread_policy_set(THREAD_TIME_CONSTRAINT_POLICY)` | **3 µs** | **18 µs** | **70 µs** |

  `thread_policy_get` returns the values that were set (period, computation, constraint) and `THREAD_EXTENDED_POLICY` reads back `timeshare=1`.
  So real-time audio threads get what they ask for from the kernel; the workgroup objects add nothing the kernel would use.
- **`x86-math.c`** (compiled `-arch x86_64`, run under Rosetta, against the Mac's x86_64 libm via `dlsym`): `sinl cosl atanl exp2l fmodl`, `__divdc3`, `__divsc3`,
  `fmaxf fminf lrintf llroundf`: **0 deviations**.
- `dlopen` of thinned Logic files (`libcom-api` loads; `MAFiles`, `Logic`, `MAMixer`, `MATower`, `MACore`, `MAApplication` fail on their first missing
  library: `Foundation…/Versions/C` without `DYLD_FRAMEWORK_PATH`, then `@rpath/MACore`, `MASwiftUIControls`, `MAStepSequencer`, `MASwiftUtilities`, `CoreMedia.framework`).
  dyld stops there; libSystem symbols are not reached. That is why the `dlsym` check above is the real test.

## 5. Remaining gaps and untested

- **os_workgroup is bookkeeping only.** No `work_interval_create` (XNU 20.5 has it; not used), no port export (`copy_port`/`create_with_port` = ENOTSUP). `os_workgroup_interval_start` by a thread
  that is not a member returns `EPERM`; on the Mac it traps. Semantics for parallel workgroups (nesting, arenas) are only checked for the return codes above. Nothing
  creates these objects for Logic yet: the CoreAudio shim (`docs/audio-stack.md`) has to call `os_workgroup_interval_create`.
- `dispatch_queue_set_specific` destructors run on replace/clear, not when the queue is released. `dispatch_block_create_with_qos_class` ignores QoS. `dispatch_barrier_async` needs
  the build-time offset; without it the fallback is `dispatch_async` (same on serial queues, no barrier on concurrent ones). It is **tied to this libSystem build** (the image is pinned by SHA-256).
- `OS_os_log` is an alias of `OS_object`; the rest of the `OS_*` classes are the real ones. `os_signpost` is off.
- **x86_64 is built and statically complete, never run.** Not exercised there: `cmpxchg16b` queue path, `dispatch_get_current_queue` (`%gs:160`), the barrier offset, x87 math under PureDarwin's kernel (the math was run under Rosetta only).
- Not rebuilt: a full `brand.sh` image. Not run: real hardware, `ucrun`, `dlclose`.
- `getattrlistbulk` returned entries from the ext4 driver, but whether the attribute buffers match what Logic's MACore parses is untested; `fsgetpath` and `searchfs` fail with ENOTSUP, MACore/MAFiles must tolerate that.
- Darling Foundation's `operatingSystemVersion` (section 3), and everything that is not libSystem: Swift runtime, Accelerate, AudioToolbox, CoreAudio, SwiftUI, … (see `logic-pro-gap.md`).

## 6. How to repeat

```
CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop libcompat/build.sh S      # S = empty stage dir (needs the PureDarwin image in $CLOKEOS_WORK/download)
python3 libcompat/missing.py "/Applications/Logic Pro.app" arm64 S/usr/lib/libSystem.B.dylib S/usr/lib/system/libdyld.dylib S/usr/lib/system/libclokeos_compat.dylib
cp -c <desktop image> d.img; macapps/dlprobe.sh d.img S /             # system files over the root
clang -target arm64-apple-macos11 -o t libcompat/test/libsystem.c; macapps/dlprobe.sh d.img <dir with t> Probe
```
