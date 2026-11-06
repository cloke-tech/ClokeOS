# Windows programs on ClokeOS: Raven instead of Wine

Decision (project owner): ClokeOS stops working on Wine and uses
[Raven](https://github.com/mirkobrombin/raven) (GPL-3.0, C++20, ~134k lines in `native/`) as the base for running
Windows programs. This note records what was measured on ClokeOS x86_64 (PureDarwin 20.5, XNU 7195) and the plan.

Why not Wine: PureDarwin's Wine 11.0 needs a working `wineboot`, which crashes on every start in the QEMU VM
(xmm6-15 are zeroed after some syscalls, see `wine/`), a first start takes over 30 minutes under TCG and the prefix is ~500 MB.
The fixes in `wine/` (stub library so `mountmgr.so`/`winebus.so` load) stay in the tree but are no longer developed.

## How Raven works (and what it needs from the OS)

Raven runs the program's x86-64 machine code natively in a child process and answers every NT call itself
(`native/nt/*`, ~130k lines: files, registry, threads, windows, sockets, ...). The only OS-specific part is
`native/cpu/native_cpu.cpp` (2.5k lines):

| Raven (Linux)                              | ClokeOS (XNU)                                           |
|--------------------------------------------|---------------------------------------------------------|
| `fork` + `ptrace` stops at the API gate    | `fork`/`posix_spawn` + `task_for_pid` + Mach exception port (`EXC_BREAKPOINT`) |
| `PTRACE_GETREGS/SETREGS`, `NT_X86_XSTATE`  | `thread_get_state/thread_set_state` (`x86_THREAD_STATE64`, `x86_AVX_STATE64`) |
| `process_vm_readv/writev`                  | `mach_vm_read_overwrite` / `mach_vm_write`              |
| `memfd_create` + `MAP_SHARED` guest memory | `shm_open` + `mmap`, or `mach_vm_remap`                 |
| injected `clone` for guest threads         | `thread_create_running` in the child's task             |
| seccomp filter (sandbox)                   | none at first (guest raw `syscall` reaches XNU)         |
| 32-bit guests in compat mode               | not supported (XNU 20.5 has no 32-bit user mode)        |

## Measured on ClokeOS (this directory)

* `raven/mach_gate.c` (x86_64 Mach-O, built on the host, run in the VM as root): the parent obtains the child's task port with
  `task_for_pid`, sets an exception port for `EXC_BREAKPOINT|BAD_ACCESS|BAD_INSTRUCTION`, the child runs `mov eax,5; int3; ...`,
  the parent reads and changes `rax` with `thread_get_state/thread_set_state` and replies. Result: both "API calls" were answered,
  the child's exit code showed the changed register (exit 21 = 10*2+1). The key risk (process control on PureDarwin) is gone.
  Note: the host libSystem has no `mach_exc_server`, the spike decodes the exception message by hand.
* Toolchain: PureDarwin ships `libc++` (`/usr/include/c++/v1`, `libc++.1.dylib`); Raven uses `std::span`, `std::bit_cast`,
  `std::ranges`. Homebrew LLVM is on the build Mac.
* `clang++ -fsyntax-only -std=c++20 -target x86_64-apple-macos11` over all 167 non-test sources:
  with only `user_regs_struct` and `memfd_create` shimmed (`raven/darwin_shim.h.draft`) 90 compile; the rest fail on
  small POSIX differences: `SOCK_CLOEXEC`/`SOCK_NONBLOCK` (23), `st_mtim` (9), `ucontext` needs `_XOPEN_SOURCE` (44, same
  cause), `linux/futex.h` (9), `accept4`, `POLLRDHUP`, `SO_PEERCRED`, `F_ADD_SEALS`, `CLOCK_BOOTTIME`, `inotify`, `cpu_set_t`,
  and optional third-party headers (Vulkan, OpenSSL, Skia, libpng, xcb, EGL, Wayland).

## Plan

1. `native/platform/` layer in a Raven fork (keep upstream mergeable): register struct, shared memory, `accept4`/`pipe2`/`eventfd`
   (pipe or kqueue), `inotify` (kqueue), `/proc` reads, `splice`, `st_mtim`.
2. `native_cpu_mach.cpp`: the Mach backend from the table above, starting with `--run-entry` console programs (hello64.exe),
   then `--run-process`.
3. Build: cross-compile on the Mac with PureDarwin's libc++ and libSystem (like `libcompat/build.sh`), no Vulkan/Skia first;
   stage into the image as `/usr/libexec/raven` and `/NT` as the prefix root.
4. Desktop: Raven's experimental X11 desktop (libxcb, present in the image) against CloakServer; later fonts (Skia CPU).
5. Integration: `raven` on PATH, `.exe` double-click in Fundus (the mime and `wine.desktop` entries are reused), launcher app.
6. arm64: not before the translation layer (`xlat/`) can run x86_64 Mach-O; Raven itself needs real x86-64 code execution.

License: Raven is GPL-3.0 with MIT guest adapters (`LICENSES.md`). ClokeOS releases that ship Raven must offer the
corresponding source (Raven's own `packaging.md` describes the two-archive scheme).

## Status (2026-11-06): a Windows console program runs

A MinGW-built 64-bit console program (`printf`, `GetNativeSystemInfo`, exit code 7) runs through `raven --run-process` in the x86_64 VM:
`Hallo von Windows: Zeiger 64 Bit, Architektur 9, Prozessoren 1` and the exit code arrives. The NT service stubs of Raven's synthetic
ntdll use a Mach-class number (`eax | 0x1000000`) because XNU raises `EXC_SYSCALL` for an unknown Mach trap, which replaces
`PTRACE_SYSEMU`; plain numbers would run a BSD system call. The details below are from the first milestone (an import-free entry).

### First milestone: an import-free PE64 runs

`raven/build.sh` builds Raven for x86_64-apple-macos11 on the Mac (clone of upstream commit `387c718`, `raven/raven-darwin.patch` applied,
CMake + Ninja, no Vulkan/OpenSSL/Skia/FreeType/libpng/pixman). In the x86_64 VM, as root:

    raven --run-entry --execution=native entry.exe      ->  Native entry returned 42

`entry.exe` is an import-free PE64 (`mov eax,42; ret`); the PE image is mapped into a Mach task, API/return traps arrive as Mach
exceptions, registers and XSTATE go through `thread_get_state`, memory through `mach_vm_*`. What the port contains:

* `native/platform/mach_guest.{h,cpp}`: `NativeChild` on XNU (posix_spawn suspended + `task_for_pid` + exception port + listener thread),
  an emulated `ptrace()` and `process_vm_*`, Linux `mmap/mprotect/munmap/madvise` mapped to `mach_vm_*`, the guest's GS base set by running
  `_thread_set_tsd_base` at the bootstrap gate. `native_cpu.cpp` keeps its ptrace-shaped logic and only includes this instead of the Linux code.
* `native/platform/darwin_compat.h` (force-included): `user_regs_struct`, `memfd_create` (POSIX shm), `SOCK_CLOEXEC`/`accept4`/`pipe2`,
  `SO_PEERCRED`, `getrandom`, `eventfd` (self-connected datagram socket), `inotify` stub, `sysinfo`, `/proc/self/exe`, process queries via libproc.
* PureDarwin traps found on the way: shared memory objects cannot be mapped executable (PE images are copied into guest pages and the
  host view is re-pointed at them with `vm_remap`), there is no `mach_make_memory_entry_64`/`task_terminate`, `pwrite` fails on shm,
  `fstat` of shm reports page-rounded sizes, and its libc++ aborts in `std::uncaught_exceptions()` and every `std::exception_ptr`
  (`native/platform/patch_libcxx.py` patches the first, `PendingError` in `nt/threads.h` replaces the second).

Not yet: programs with imports (`--run-process` needs Raven's component DLLs: `scripts/provision-*.py`, large downloads), the X11
desktop (pixman, freetype, xcb for x86_64), non-root users (`task_for_pid`), the guest sandbox, time slicing and parallel threads,
32-bit programs, `inotify`, `/proc`-based queries (network tables, memory statistics).

## Status (2026-11-06, later): a window on the desktop

`--desktop=x11` works: Raven's xcb presentation (libxcb, xkbcommon and pixman linked statically from a sysroot that `raven/build.sh` makes
from the PureDarwin image; pixman 0.46.4 is built from source with meson) shows the window of a MinGW GDI test program
(`CreateWindow`, `WM_PAINT`, `FillRect`) on the CloakServer desktop; the program's title bar and text stay empty because GDI text needs
Skia and fonts (`scripts/provision-text.py`, a large download that has not been approved yet). The `raven` wrapper passes
`--desktop=x11` by default (`RAVEN_DESKTOP` overrides). Caveats seen: the guest bootstrap was left suspended once after a killed run, and
the MinGW GUI start code needs `__p__acmdln`, which Raven's ucrtbase lacks (the test program used its own entry point).

## Status (2026-11-06, evening): text in windows

Skia (commit `a21aae8`, CPU only, `target_os=mac target_cpu=x64`, `skia_use_fonthost_mac=false` because CoreText does not exist on
PureDarwin) and the Liberation/Raven Sans fonts are part of `raven/build.sh`. Only FreeType is fetched for Skia (`git-sync-deps` would
download 7 GB, mostly Dawn, ICU and unicodetools); Skia's FreeType needs libpng headers (taken from the image). The GDI test window now shows
a Windows title bar with its caption and `TextOutA` text on the ClokeOS desktop. The stage grows by about 7 MB for the fonts.

## Status (2026-11-06, night): more programs

* `sqlite3.exe` (the fixture MinGW build of SQLite 3.50.4) runs and answers `select sqlite_version(), 6*7;` with `3.50.4|42`.
* A 64-bit installer-like program (MinGW, plain Win32: `CreateDirectory` in `%ProgramFiles%`, `CopyFile`, `CreateFile`/`WriteFile`,
  `RegCreateKeyEx`/`RegSetValueEx` under `Software\Microsoft\Windows\CurrentVersion\Uninstall`, temp file) runs: the files appear in
  `~/Library/Raven/drive_c/Program Files/RavenTest` and the registry in `registry.dat`.
* (superseded below: 32-bit programs and the NSIS installer run now.) MSI still needs Raven's installer components (ReactOS sources,
  `scripts/provision-installer.py`), not fetched.
* PureDarwin's libSystem has no `swapcontext`: `native/platform/context_darwin.{h,cpp}` is a small x86-64 replacement (guest threads are
  coroutines).
* **Notepad++** (the fixture's portable build) runs with its full window: menus, toolbar icons, the editor, and typed text arrives
  (`vmctl type` through X). Two fixes got it there: (1) the `__fastfail(7)` was a `std::bad_alloc` for a 4 GiB request, which Raven rejected
  because the guest stack lay inside the requested range; XNU's "anywhere" mappings start at the bottom of the address space, so they now start at
  `0x100000000000` like Linux; (2) OpenSSL (crypt32: Notepad++ checks plugin signatures) and libpng 1.6.59 (icons) are linked: libcrypto/libssl from the
  image, headers from Homebrew's openssl@3, libpng built from source by `raven/build.sh`.

## Status (2026-11-06, late): files and dialogs in Notepad++

Through the menus (clicks and typing by `vmctl` through X): **File > Save As** opens Raven's own file dialog (Quick access, This PC with
`C:` = `~/Library/Raven/drive_c` and `Z:` = the Unix root), typing `Z:\tmp\docs\saved.txt` and Save wrote `/tmp/docs/saved.txt` (25 bytes, the
typed text); **File > Open** listed `Z:\tmp\docs` with sizes and dates and loaded `unix.txt` (detected as Unix LF). Not tested: keyboard
shortcuts with modifiers (`vmctl xkey` has no Ctrl), clipboard, drag and drop, printing. The log shows `thread N waits for window ... to answer
message 430` lines while a dialog is open; they are Raven's own trace of a cross-thread message and did not hurt.

## Status (2026-11-06, night 2): 32-bit programs

XNU 20.5 cannot start 32-bit processes, but a 64-bit process can run 32-bit code in compatibility mode through an LDT entry (spike:
`raven/` history, `i386_set_ldt` is machine-dependent call 5, `0x3000005`; `LDT_AUTO_ALLOC` = 0xffffffff gives the first free index, 3).
Raven's x86 mode now works on top of that (`native/platform/mach_guest.cpp`):

* the guest thread allocates three LDT entries by running the call itself at the bootstrap gate: a flat 32-bit code segment, a flat data
  segment and the entry for the TEB (FS); Raven's `modify_ldt(1, user_desc)` rewrites the FS entry;
* `thread_set_state` always resets the CPU to 64-bit mode (the kernel forces CS), so every resume of 32-bit code goes through a small stub at
  `0x10000`: `mov m16,%ds / %es / %fs` from memory (no register is touched) and `ljmp *far(%rip)` with the target in the far pointer, written for
  each resume. The ptrace-style `GETREGS` reports `cs=0x23`, `fs=7` and, while the stub is armed, the logical `eip`;
* 32-bit allocations (`MAP_32BIT`) come from a gap search below 4 GiB (`mach_vm_region`); breakpoints (`int3`) in compatibility mode arrive as
  ordinary Mach exceptions with the 32-bit state in the 64-bit registers.

Tested in the x86_64 VM: `hallo32.exe` (MinGW i686) prints `Zeiger 32 Bit` and returns its exit code; a 32-bit GDI window with text is shown on the
desktop; a real NSIS installer (`makensis` 3.13: `File`, `WriteUninstaller`, `WriteRegStr`, `CreateShortCut`) ran and installed `payload.exe` and
`uninst.exe` under `Program Files\RavenTest` (no `.lnk` seen). `raven/build.sh` builds the 32-bit components (`provision-controls.py --arch x86`,
`ucrtbase32.dll`) and installs `x86/` and `ucrtbase32.dll` beside `raven`.

## Status (2026-11-06, night 3): shortcuts and clipboard

`clokeos-xtest key` (and `vmctl.py xkey`) now takes modifiers: `ctrl+a`, `ctrl+shift+s`, `alt+F4`. In Notepad++: Ctrl+A, Ctrl+C and Ctrl+V work
(the copied line was pasted again: `length : 26, lines : 4`, text twice). The text went through Notepad++'s own copy and paste inside Raven; copying
between a Windows program and a Unix program through the X selection was not tested. (A note for the test rig: after the VM starts, wait for
the login screen before pressing Return, otherwise the typed text lands in the password field.)

## Status (2026-11-06, night 4): clipboard between Windows and Unix programs

Raven's X11 presentation owns and requests the `CLIPBOARD` selection like any X client; tested both ways in the VM with `xfce4-terminal`:
Notepad++ Ctrl+A, Ctrl+C, then Ctrl+Shift+V in the terminal pasted `Windows-Text fuer Unix`; the other way, `Unix-Text fuer Windows` typed in the
terminal, Ctrl+Shift+A and Ctrl+Shift+C there, then Ctrl+A and Ctrl+V in Notepad++ replaced the editor text (the whole terminal screen came along, so
the document has 25 lines). Text only; other formats (images, files) and Drag and drop are untested.

## Status (2026-11-06, night 3): MSI and Inno Setup

* **MSI works.** With Raven's installer components (ReactOS sources built by `scripts/provision-installer.py`, now part of `raven/build.sh`;
  needs `widl` from MinGW and bison >= 3.8 from brew) `msiexec /i test.msi /qn` installs a package made with `wixl` (msitools):
  the file lands in `C:\Program Files\RavenTest`, the log shows every standard action, and `msiexec /x` removes it again (both exit 0).
  The `raven` wrapper runs `.msi` files through `msiexec` (`raven setup.msi`), and Fundus/`cloke` use it for MSI packages.
* Three Darwin-specific bugs stood between Raven and a real Inno Setup installer: (1) POSIX shared memory cannot be read or written with
  `read`/`write` on Darwin, which broke the environment/handle blocks passed to child processes (ENXIO, surfaced by Inno as "Error 8: Not enough memory
  resources"), so those blocks and the exit-code file are now unlinked temp files; (2) `fcntl` locks on a POSIX shm object fail, so the
  token storage lock is skipped on Darwin; (3) `IsWow64GuestMachineSupported` was missing.
* **Inno Setup 6.7.4** (the real installer, PE32 stub plus a 64-bit setup process): the stub extracts itself, spawns the setup and its own
  windows (language and install-mode pages) render and take clicks. After the language page the 64-bit setup process stays busy for more
  than 15 minutes in the slow emulated VM without showing the wizard; not diagnosed further, so Inno installers are *not* yet usable.
  Silent installs (`/VERYSILENT`) did not install either. "Install for all users" exits because elevation is not implemented.
* Test-rig trap: the PureDarwin image has no `pgrep`; use `ps ax | awk '/[r]aven/{print $1}'` to kill stray runs (stray runs make Inno say
  "Setup is currently running").
* Diagnosis aid added: `RAVEN_TRACE_FILE=/tmp/tr` makes every Raven process (also the children an installer starts, which have no stderr)
  write its trace to `/tmp/tr.<pid>`; use it with `RAVEN_TRACE_API=1` and `RAVEN_TRACE_STOPS=1`. First result for Inno: the 64-bit
  setup process produces tens of thousands of trace lines in minutes (so it is working through many calls, not blocked), but the
  trace was not read to the end because the single-CPU VM stopped answering shell commands while it ran. Next step: read
  `/tmp/tr.<pid>` of the last process and look for the repeating call.

## Status (2026-11-06, night 4): the Inno Setup stall found and fixed

Cause of "setup then stalls", in three layers (found with per-process traces; 32-bit programs trace with `RAVEN_TRACE_X86=1`,
not `RAVEN_TRACE_API`, and `RAVEN_TRACE_FILE=/tmp/tr` writes one file per process):

1. Without `/CURRENTUSER` or `/ALLUSERS` even `/VERYSILENT` shows the "Select Setup Install Mode" dialog (elevation is not
   implemented, so the all-users choice is not testable). That is Inno behaviour, not a stall.
2. Inno's 32-bit setup stage loops over a bitmap and calls `MulDiv` three times per pixel. Every call was a trap into the
   runtime (a Mach exception round trip), minutes under TCG. The 32-bit kernel32 provider now holds a guest-side `MulDiv`
   (`x86_muldiv_code` in providers.cpp, same rounding and error rules as the 64-bit prelude; checked with 13 cases in a 32-bit
   test program).
3. Inno then needs `SHGetImageList` (new in raven-shell, built on comctl32 image lists) and `IPropertyStore` on the shell
   link object (link.c; the store accepts values and answers every key as empty), or it ends in "Interface not supported".

Result: Inno Setup 6.7.4 installs its files (`/VERYSILENT /CURRENTUSER`) under Raven. Still open: the install has not been
checked to its very end (uninstaller, Start menu links), elevation, and the windows of the non-silent wizard.
build.sh rebuilds the controls components when `raven-shell.dll` lacks `SHGetImageList`.
