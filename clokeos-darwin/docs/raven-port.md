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

Result: Inno Setup 6.7.4 installs under Raven (`/VERYSILENT /CURRENTUSER`): files, Start menu links, uninstaller (checked in the VM). Still open: elevation (all users) and the
windows of the non-silent wizard.
build.sh rebuilds the controls components when `raven-shell.dll` lacks `SHGetImageList`.

## Status (2026-11-07): the Inno wizard is visible

The "no window" of the non-silent Inno Setup was a stale `/usr/bin/raven` in the test image (without `--desktop=x11`): children
without a display drop every frame (`RAVEN_TRACE_PRESENT=1` prints one line per frame with `display=0|1`). With the wrapper
from the repo the language dialog and the wizard (license page) show on the CloakServer desktop. Visible glitches left: the
radio buttons of the license page sit on a black strip with black text (TNewCheckListBox background), and the page header is
clipped. Clicking OK on the language dialog works. Rule for tests: copy `overlay-desktop/usr/bin/raven` into a VM that was
not rebuilt, or the GUI parts do not show.

### Inno wizard: radio strip and header fixed (2026-11-07)

Both glitches had one cause in `DrawThemeParentBackground` (theme.cpp). Window positions are screen positions, so the shift
`window->x - parent->x` was right, but the control's DC kept its bounds, clip and visible region in the control's own
coordinates while the origin moved to the parent's: every pixel of the parent's background was then rejected by
`DesktopDC::contains` (trace: `fill ... changed=0`) and the control kept its black initial pixels (the radio strip) or got a
misplaced fill (the clipped header). The bounds and clips now move with the origin for the duration of the call. The license
page of the Inno Setup wizard now shows both radio buttons and the whole header. (A first attempt that shifted by the child's
position alone looked right for the header only by accident; it was wrong, as the trace showed screen positions.)

Diagnostics added: `RAVEN_TRACE_FILL=1` prints every FillRect (dc, rect, origin, colour, whether a pixel changed) and every
`DrawThemeParentBackground` shift. Test notes: the dialog opens at a different place each run, so press Return
(`vmctl.py xkey Return`) instead of clicking OK; the desktop session logs out to the greeter after about 40 minutes without input.

## Status (2026-11-07): the Inno Setup wizard installs from first page to last

Clicked through in the VM: language, license, folder, Start menu folder, tasks (check boxes), summary, install with progress bar,
finish page. One bug showed at the very end of the install ("Could not load ISCmplr.dll: BCryptExportKey result invalid"): the
exported ECDSA/ECDH public blob had magic `ECS0`+1 added to its lowest byte instead of the digit in its fourth byte (`ECS1`).
Fixed in `ec_magic` (bcrypt_api.cpp); a 32-bit test (open ECDSA_P256, import and export the generator point) now returns the
imported blob unchanged. Still open: installs for all users (elevation); the greeter came up after Finish (launching the
installed Compil32.exe) — not checked whether that was the idle logout or a session crash; the folder page's icon was blank
(`SHGetImageList` hands out empty image lists).

## Status (2026-11-07): the installed Inno Setup Compiler runs

Finish of the wizard did not crash the session (the greeter seen earlier was the idle logout; the desktop stayed up in every
later run). `Compil32.exe` (32-bit Delphi IDE with Scintilla) needed two more calls: `IsEqualGUID` (guest-readable 16-byte
compare) and `ResizePalette`; then its main window and Welcome dialog show with menus, toolbar and tabs (about 8 minutes under
TCG). Seen and open: the title shows `?` for the em dashes, the two image buttons at the bottom of the Welcome dialog are black
(their bitmaps do not draw), and the first start takes minutes. Note for running it: `raven` takes a host path for the program
(`/var/root/Library/Raven/drive_c/Users/root/AppData/Local/Programs/Inno Setup 6/Compil32.exe`), not a C:\ path.
Test tip: other sessions kill every x86 QEMU on the Mac now and then; run the VM with its own `CLOKEOS_WORK` (for example
`~/ClokeOS-work-raven-vm`) and the image from the shared `out/` directory with `--snapshot`.

### The black image buttons of Compil32's Welcome dialog (open)

Findings (`RAVEN_TRACE_FAIL=1` now also prints the first colour-table entries of every indexed `CreateDIBSection`): the two buttons
are `TBitmapButton` controls that draw 62x31 images with a Delphi `StretchBlt` after `SelectPalette`/`RealizePalette`. Four
8-bit sections of that size are created with an all-zero colour table, so everything quantizes to black. On Windows the table
comes from `RealizePalette` of the palette selected into the context. `SelectPalette` now keeps the palette in the context and
`RealizePalette` copies it into the colour table of a selected indexed DIB section (gdi_api.cpp), but the buttons stayed black
in the next run, so the colours are lost on another path (the image bits are copied into the section by a blit or `SetDIBits`
that maps through the empty table before the palette is realized, or the palette itself holds black entries after `ResizePalette`).
Next step: trace `SelectPalette`, `RealizePalette`, `ResizePalette`, `BitBlt` and `SetDIBits` calls for those four sections with
`RAVEN_TRACE_X86=1`, but read only the last 3 MB of the trace file (a full 11 MB trace wedges the single-CPU VM's shell).
The em dash `?` in the title is also still open.

Update (trace of the button images, last 3 MB of an `RAVEN_TRACE_X86=1` run): Delphi copies each 8-bit image like this:
`GetDIBits(dc, source, 0, h, NULL, info)` (format query), `CreateDIBSection(dc, info)` (the table comes from `info`),
`PatBlt`, then `SelectPalette`/`RealizePalette` on the *source* context and `BitBlt` source -> new section; the new section is later
selected with `SelectPalette`/`RealizePalette` and drawn with `StretchBlt` (two calls, one per button). The format query of
`GetDIBits` did not hand back the colour table of an indexed bitmap; it does now (gdi_api.cpp), which is what Windows does, but the
buttons stay black, so the table of the source bitmap is itself empty or the blit into the section does not quantize through
the table. Still open, with the next step: trace the source bitmap (`GetObjectW` shows a 0x54-byte DIBSECTION for it) and print its
palette in `GetDIBits`.

### The Compil32 image buttons draw now (2026-11-07)

Cause found with `RAVEN_TRACE_FAIL=1` (prints `getdibits table ...`): Delphi asks `GetDIBits(dc, bitmap, 0, h, NULL, info)` with
`biBitCount` already 8 and a *32-bit* source bitmap, and builds the colour table of its new 8-bit section from the answer. The
call returned at once, leaving the table zero (black). Without a buffer `GetDIBits` now fills the colour table of an indexed
format: the bitmap's own colours when it has that depth, else the grey ramp (as Windows' default answer for such a request).
PayPal "Donate" and "Subscribe" show in the Welcome dialog. The earlier changes of this section (`SelectPalette` keeps the palette,
`RealizePalette` copies it into a selected indexed section, the colour table in the format query) stay: they are what Windows does.
Still open: the `?` for the em dashes in the Compil32 title.

### Dashes and quotes in titles and menus (2026-11-07)

The `?` in `Untitled — Inno Setup Compiler` came from the frame's text drawing (canvas.h `draw_text`): with a width limit (caption and
menu labels have one) it took the fast path whose `printable()` replaces every character outside Latin, Greek and Latin Extended
Additional with `?`, and only an unlimited line used the fallback faces. A 32-bit test program (title with U+2014 and U+2013) showed
it; the text was intact in the window (`GetWindowTextW` returned it). Text with such characters now takes the fallback faces
whatever the limit and is shortened with an ellipsis when it is wider than the limit. Checked with the test program (title with em and en dash); the Compil32 title itself was not looked at again.

## Status (2026-11-07): installed Windows programs in the application menu

`raven --install-menu [folder]` (native/menu.h) reads the Start Menu shortcuts the installers left in the prefix
(`ProgramData/.../Start Menu/Programs` and every user's `AppData/Roaming/.../Programs`), and writes one `Name.app` per program
into `~/Applications` (or the folder named), which CloakServer lists in the launchpad and rescans every two seconds:
`Contents/MacOS/launch` (a start script: `cd` to the shortcut's folder, `exec raven '<host path>' <arguments>`),
`Contents/Info.plist` and `Contents/Resources/icon.png` (the biggest image of the program's first icon group, converted from the
DIB to PNG here, no library needed; an `.ico` or another file named by the shortcut's icon location is used when given).
Skipped: shortcuts that are not .exe (documentation, folders), uninstallers, programs that no longer exist. A bundle this tool
wrote has the key `RavenShortcut` and is removed when its shortcut is gone (an uninstalled program); other bundles are never touched.
The wrapper `/usr/bin/raven` runs `--install-menu` after every program it ran, so an installer's shortcuts show up in the menu
when it ends. The plist must use the layout CloakServer writes (key and value on lines of their own), or its reader ignores the keys.
Checked in the VM: the Inno Setup Compiler appears in the launchpad with its own icon. Not done: the window class of Raven windows
is `raven.native` for every program, so the global menu bar names every Windows program "raven.native" (a per-program class is
the next step, and needs the title-bar session's answer, see below); programs with more than one shortcut get the first.

Layout (2026-11-07): the C: drive is `/NT`, the Windows tree of the system that all users share (the image ships `/NT`, group staff
may write, `/NT/Users` has the sticky bit like /tmp). `C:\Windows` is `/NT/Windows`, `C:\Program Files` is `/NT/Program Files`,
`C:\Users\<name>` is `/NT/Users/<name>`. Raven picks `/NT` when it exists, `RAVEN_C_DRIVE=<folder>` overrides, and without `/NT` the
drive is `NT/` inside the prefix (`$RAVEN_PREFIX`, ClokeOS: `~/Library/Raven`, which keeps `registry.dat`, snapshots and `Volumes/<letter>/`
for other drives). Folders Raven creates in the shared tree are open to the group (0775). Each user's folder is created with the
account: by CloakServer's account setup (`setup.c`) and by brand.sh for the image's user, with `AppData/{Local,Roaming}` and
`Documents`, `Desktop`, `Downloads`, `Pictures`, `Music`, `Videos` as symbolic links to the user's own folders (Videos -> Movies); Raven also makes
the folder on first use (nt/paths.cpp `user_profile`, from `$USER`, which the wrapper sets). `--install-menu` reads the all-users
Start Menu of the tree and the current user's. A prefix from before (`drive_c`, `windows`) is renamed to `NT` when it is opened and no
`/NT` exists. Limits: the registry (also HKLM) is still one per user prefix, so a program installed by one user is in the shared
`Program Files` but registered only for that user; files Raven copies into `/NT/Windows` belong to the first user. Checked in the VM
with a hand-made `/NT` (775, `Users` 1777): `dir C:\` lists the tree, `/NT/Users/root` got its folders and symbolic links. The account code was checked in
parts: CloakServer builds with it (`cloakserver/build.sh`), the `setup.c` block run alone made the folder and links, and the
`brand.sh` debugfs commands made the same on a scratch ext4 image. Image build (brand.sh for the desktop image, with the Raven part taken from the tested binaries instead of
rebuilt): `/NT` (775 staff, `Users` 1777) and `/NT/Users/guest` with `AppData` and the six links are in the image; a first Raven run
filled `/NT/Windows`, `Program Files` ... (775) and `dir C:\` listed them. Not done: account creation through the setup assistant.

### Window class per program, and the title bar answer (2026-11-07)

Every Raven window now has the X11 class `<program name>` (the stem of the .exe, blanks as `_`; `RAVEN_WM_CLASS`, set by Raven for each
program, `raven.native` without one), and the app bundles from `--install-menu` carry `CLWindowClasses` with the same name, so the global menu
bar shows the program's name (checked: a test program shows "em32" instead of "raven.native").

The title-bar session answered (docs/titlebar.md in main): CloakServer draws the new bar for every X window without
`_MOTIF_WM_HINTS` decorations=0 (Raven sets 0 today and so gets only edge, corners and shadow); `_CLOKE_TITLEBAR` (CARDINAL[6]) sets who
draws the top area; title from `_NET_WM_NAME`; no icon in the bar; dialogs without zoom via bit 5 of [4] and min=max size hints; move,
resize, zoom and close are the window manager's (ConfigureNotify, WM_DELETE_WINDOW, `_NET_WM_MOVERESIZE`). So the frame can leave
Raven: the X window would be exactly the client area, Raven would stop painting caption, border and caption buttons and handling
their hits, and give CloakServer the title, style (dialog, tool window, no maximize box) and fixed-size hints; the menu bar stays inside
the client area. Not started: it touches frame.cpp, hit testing, window sizing (outer vs client rectangles) and every caption-related path.

### The window manager's title bar for Raven windows (2026-11-07)

On by default now; `RAVEN_FRAME=raven` keeps Raven's own frame (xcb.cpp, `presentation.frame_insets` provided by `NativeWindows::set_presentation`).
CloakServer frames a window that has a caption: the host X window is the outer frame minus caption and border (the Windows menu bar stays in
it); only that part of the pixels is sent; pointer coordinates get the cut-off offset added back, a host resize adds it to the size, popups,
driver surfaces and the pointer confinement subtract it; `_MOTIF_WM_HINTS` decorations=0 is not set for such windows, so CloakServer's own bar
appears (traffic lights, bold title). Windows without a caption keep Raven's drawing and decorations=0. `RAVEN_TRACE_X11=1` prints the frame,
host size and cut-off of every frame sent. Checked in the VM: a test program (bar, title with dashes, zoom and back, close) and the Inno Setup
wizard (language dialog OK, license radio button, Next, with the bar and correct pointer positions) twice in a row.
An earlier image crashed the whole desktop session on the click on OK of the Inno language dialog in this mode (twice); an image built after the
title-bar session's CloakServer commits (b2c0498, with a crash log in /var/log/cloakserver-crash.log) did not, in two tries, and the log stayed
empty. Cause of the earlier crash not found; if it comes back, `RAVEN_FRAME=raven` avoids it and the crash log shows where CloakServer died.
Buttons: Raven sets `_CLOKE_TITLEBAR` from the window style (no WS_SYSMENU greys the close light, no WS_MINIMIZEBOX the minimize light, no
WS_MAXIMIZEBOX the zoom light) and gives a window without WS_THICKFRAME equal minimum and maximum size hints (no resizing, no zoom). Checked
with the Inno Setup language dialog: only the close light is active. Not done: tool windows (a small bar), minimize reporting; the zoom/unzoom test program vanished in two of five tries before (no error), not seen since.

### The menu in the global menu bar, and programs in the Dock (2026-11-07)

**Menu bar.** A window with a caption and a menu (Notepad++, the Inno Setup Compiler ...) puts its menu into the property `_CLOKE_MENU` (xcb.cpp,
`update_menu`, text from `NativeWindows::global_menu_text`: one line per entry, `depth\tkind\tnumber\tflags\tlabel\tkey`, `&` removed, the tab
text becomes the key, grayed/disabled `d`, checked `c`, radio `r`). CloakServer shows it in the top bar; the choice comes back as the client message
`_CLOKE_MENU_ACTIVATE` and Raven posts WM_COMMAND with the entry's number to the window. The menu bar inside the window is cropped away (the top
cut-off grows by the menu's height). Checked with a test program (File: New, Open..., line, Exit; Edit: checked Wrap, grayed Undo, accelerator
text): the bar shows "menu32 File Edit Window", the entries with their keys, and a click on Open... gave WM_COMMAND 2. Limits: WM_INITMENUPOPUP is
never sent, so programs that update their menu only then show stale states; the text is rebuilt with every frame and sent when it changed.

**Dock.** Programs with a bundle (from a shortcut, `--install-menu`) show in the Dock while they run (the window class ties window and bundle).
A program started without a shortcut (double click on the .exe) gets a bundle first: the wrapper runs `raven --add-app <program.exe>`, which writes
`~/Applications/<name>.app` (icon from the .exe, `CLWindowClasses`, key `RavenProgram`, not removed by `--install-menu`). Installers (names with
setup, install, unins), the tools Raven ships and programs that already have a bundle are left out. Checked: the test programs show with their letter
tile (no icon in the .exe) and the running dot. They also stay in the application menu (delete the .app to remove one).

Notepad++ (portable, 64-bit) with the global menu: all top-level menus (File Edit Search View Encoding Language Settings Tools Macro Run Plugins Window ?)
show in the top bar, with accelerators, greyed entries, separators and submenus; File > New made a second tab. Its menu is about 600 lines, which
was more than CloakServer's `GM_MAX` (512; the menus after Language were dropped), now 4096 (cloakserver.c). `RAVEN_TRACE_X11=1` prints the size of the
menu text sent.

## arm64 backend (Unicorn)

On arm64 there is no x86 code to run natively, so the guest's x86-64 code is emulated with [Unicorn](https://www.unicorn-engine.org/) (QEMU TCG, GPL-2.0; Raven itself is GPL-3.0). The patch `raven/raven-arm64.patch` applies on top of `raven-darwin.patch`.

- `native/platform/unicorn_guest.cpp` is a second implementation of `NativeChild` (same interface as `mach_guest`): registers via `uc_reg_*`, guest memory as host memory mapped with `uc_mem_map_ptr` (anonymous, shared objects and the storage of PE images, so host and guest see the same bytes), stops from Unicorn hooks (int3, `syscall` instruction, memory faults) mapped to the Linux wait-status/siginfo conventions Raven already uses. The gate page (`int3; syscall; int3; ud2`) lives at 0x700000000000 in emulated memory, so there is no bootstrap program.
- CMake: `-DRAVEN_UNICORN=ON` enables the x86_64 block on arm64; `raven-bootstrap` is only a dummy target; Unicorn comes from `RAVEN_UNICORN_DIR` (`src/include` and `build-arm64/*.a`). Configure natively (not with a cross toolchain file: custom commands then do not find their generator tools) and with `-DCMAKE_DISABLE_FIND_PACKAGE_Vulkan=ON`.
- The emulated CPU is a Nehalem (SSE up to 4.2, POPCNT, no AVX); CR0/CR4 are set up for SSE as an OS leaves them.
- arm64 versions of the coroutine switch (`context_darwin`), `__rdtsc`/`pause` and the CPU feature report (`IsProcessorFeaturePresent`); the 4 KiB host page check is skipped (the guest has its own pages).
- The runtime PE files (`raven-rtl.dll`, `ucrtbase.dll`, ...) are architecture independent and are built once with mingw; the arm64 `raven` binary sits next to them.

32-bit guests run in compatibility mode inside the same 64-bit engine: the first entry goes through a stub page (`mov ss, [..]; jmp far [..]` with a small GDT whose segments are DPL 0 because the engine runs at CPL 0), because writing CS/SS with `uc_reg_write` does not update the engine's mode flags (a 16-bit stack pointer and 64-bit decoding result). `GETREGS` reports the Linux selectors 0x23/0x2b; FS gets the TEB through `FS_BASE`; `modify_ldt` is a no-op. 32-bit programs find `ucrtbase32.dll` next to `raven`.

Build for ClokeOS arm64: `CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop raven/build.sh <stage>` (work dir `$WORK/raven-arm64`; needs Unicorn in `$WORK/download/unicorn`, built by `xlat/build.sh`). It applies `raven-darwin.patch` and `raven-arm64.patch`, takes the X11 sysroot (xcb, xkbcommon, freetype) from the arm64 desktop image, builds pixman, libpng and Skia (CPU only) for arm64 and links `libXau`/`libXdmcp` (the image has libxcb as a static library). `patch_libcxx.py` also patches the arm64 `libc++` (`mov w0,#0; ret`). `toolchain-x86_64.cmake` is the old `toolchain.cmake`; the arm64 build runs without a toolchain file (as a cross build, custom commands do not find their tools). brand.sh calls `raven/build.sh` for the desktop flavour on both architectures; the `raven` wrapper in `overlay-desktop/usr/bin` is the same.

Status (checked in an arm64 VM with HVF, root, `DISPLAY=:0`, CloakServer): `hostname.exe` (64-bit console), `win.exe` (GDI window with text, 64-bit), `win32.exe` (the same as 32-bit), Notepad++ (portable, 64-bit: window, toolbar, menu in the global bar, typing works, see `screenshots/raven-arm64-notepadpp.png`) and the Inno Setup installer (first wizard page, "Select Setup Install Mode"). Natively on macOS arm64: `hallo64.exe`/`hallo32.exe` (output and exit code 7), `cmd`, `where`, `tasklist`, `reg`, `hostname`, `whoami`, in 32 and 64 bit. Not checked: walking the Inno wizard to the end, speed measurements (Unicorn is an interpreter-like TCG: expect programs to be several times slower than natively on x86_64), AVX programs (the emulated CPU is a Nehalem), threads (`enable_parallel` is not supported: Raven's guest threads run as coroutines as on Darwin x86), Vulkan/Direct3D.

Window-to-app matching in CloakServer: bundles whose `CLWindowClasses` entry equals the window class exactly win over wildcard entries. Without that, `Wine.app` (`notepad*`, `*.exe`) took Notepad++ (class `notepad++`) away from Raven's own bundle: the menu bar said "Wine" and the Dock showed the wine glass. The bundle that `raven --add-app` writes goes to the `~/Applications` of the user who starts the program, which must be one CloakServer scans.

### arm64 performance (Unicorn)

Measured with `RAVEN_TRACE_STATS=1` (counters of restarts, host reads/writes, syscalls) and `sample` on a `cmd.exe` loop (`for /L ... set /a`, 3000 rounds, about 27 000 `uc_emu_start` calls because every Win32 call is a stop and a restart):

| step | time |
|---|---|
| first version | 4.4 s |
| memory fault hook only installed for one re-run of the faulting instruction (any memory hook makes Unicorn treat every address as hooked) | 4.4 s → no change on its own, but needed for the next step |
| `unicorn-fast.patch`: stores no longer look up the memory mapping each time | 1.6 s |
| own watchdog thread for the run's deadline instead of `uc_emu_start`'s timeout (that created a thread per start) | 1.0 s |

Why stores were slow: Unicorn's dirty tracking is stubbed out, so every writable page starts on the slow path, and `notdirty_write` only cleared that for pages the *TLB entry marks as non-executable*, which is never the case for x86 (all TLB entries are executable). A store cost about 16 ns (a flat-view lookup) against 2.7 ns for a load. The patch (`raven/unicorn-fast.patch`, applied by `raven/build.sh` to a private copy of Unicorn 2.1.4 in `$WORK/raven-arm64/unicorn`, so `xlat/ucrun` keeps its own build) clears the flag unless a translated block lives on the page (`uc_page_has_code`) and flushes the TLB when the first block on a page is translated (`tlb_protect_code`). Self-modifying code was checked with a test that rewrites an instruction of its own page 200 000 times (no mismatch, same speed). `pause` is a no-op in the translator: the helper left the translation loop at every spin of a guest lock.

Where it stands (`bench.c`, mingw `-O1`, same source compiled natively for arm64): fib(35) 0.40 s against about 0.02 s, an SSE2 floating-point loop 0.37 s against 0.02 s, random read-modify-write on 4 MiB 1.1 s against 0.02 s. So compute-bound code is **15-50x slower than native** (TCG is a portable emulator, Rosetta 2 is within 1.3-2x); programs that are mostly waiting for the UI (Notepad++ starts to a usable window in under 16 s in the VM, with HVF and other VMs running) are fine. Not possible with this engine: AVX/AVX2 (Unicorn 2.1.4 is QEMU 5.0 based; QEMU got AVX in TCG in 7.2). Programs probe CPUID, so most take their SSE paths; the emulated CPU is a Nehalem.

Path to Rosetta-class speed: `xlat/jit` (block JIT x86_64 -> arm64 with lazy flags, within 0.9-2x of Rosetta on integer kernels, see `docs/translation-study.md` section 14 and 17) replaces Unicorn as the engine of `NativeChild` for 64-bit guests: see "arm64 JIT engine" below (the paragraph after this one lists what was missing at the time of the Unicorn-only measurements). Missing there: SSE/x87 (each fallback to Unicorn costs about 20 us, so SSE code is *slower* than plain Unicorn), memory-protection semantics (`mprotect` is a no-op), 32-bit compatibility mode, and the stop/restart model Raven needs (API traps, faults with `si_addr`). AVX could then be translated to NEON pairs.


### arm64 JIT engine (`RAVEN_JIT=1`, 2026-11-07)

`RAVEN_JIT=1` makes the first guest process of a Raven run execute its 64-bit x86-64 code in the block JIT of `xlat/jit` (section 17 of `docs/translation-study.md`) instead of Unicorn. Unicorn stays in the process as the fallback for instructions the JIT does not translate (`jit_uc_fallback`: registers and XMM state go over, the instructions run, the state comes back; about 20 us per round trip) and as the engine of **32-bit guests**. Without the variable nothing changes. The code is in `raven/raven-arm64.patch` (`unicorn_guest.cpp`: `Emu::jit_mem/jit_cpu`, `run_jit`) and `xlat/jit`; `raven/build.sh` passes `-DRAVEN_JIT_DIR=.../xlat/jit` (the JIT is compiled by a custom command because the directory forces `-include darwin_compat.h`, which is C++).

* **Same interface as the Unicorn engine.** `NativeChild::wait` calls `run_jit` instead of `uc_emu_start`; the stop conventions are unchanged: `syscall` -> `SIGTRAP|0x80` with `orig_rax`, `int3` -> `SIGTRAP`/`TRAP_BRKPT`, `ud2` -> `SIGILL`, a memory fault -> `SIGSEGV` with `si_addr` and `SEGV_MAPERR`/`SEGV_ACCERR` from the protection table, a timeout -> the watchdog sets a word the JIT code polls on backward edges and indirect jumps. Registers (`GETREGS/SETREGS`, XMM and MXCSR in `GETFPREGS/SETREGSET`) are read from and written to the JIT context; a `SETREGS` with `cs == 0x23` moves the state into Unicorn and the guest runs in compatibility mode there from then on. A single step is one instruction in Unicorn.
* **Memory.** All guest memory of the JIT process is one window of the host's address space (guest address + `delta`), 16 GiB or more (up to 128 GiB; below 2^39, because the PureDarwin arm64 kernel has 39 bit user addresses). To fit, the layout moved: the user range ends 2 GiB below the window's top (`user_limit_value()`), the runtime providers (`ntdll.dll`, `kernel32.dll`, ...) start there (`provider_base_value()`), the engine's free choice and the gate page are above. A 64-bit image or section is mapped from its shared-memory descriptor straight into the window (host and guest see the same bytes), anonymous memory is made in place, 4 KiB guest protections are kept in a table (host protection = union over the four guest pages in a 16 KiB host page). Raven's reads and writes of guest memory go through `jit_guest_copy`. Only the first guest process of a Raven run owns the window; further processes (e.g. started by `cmd.exe`) run on Unicorn.
* **Self-modifying code** is detected by write-protecting the host pages translated code came from (section 17).
* **AVX/AVX2**: `RAVEN_JIT_AVX=1` as well (off by default: the emulated CPU stays a Nehalem). Programs then see AVX/AVX2 in CPUID and XCR0; FMA/BMI/F16C are not reported.
* **Counters**: `RAVEN_TRACE_STATS=1` prints blocks, translated instructions, fallbacks, invalidations and the twelve most frequent fallback sites (address, count, first bytes), `RAVEN_TRACE_STOPS=1` the faults.

Measured with `xlat/jit/bench-raven.c` (mingw `-O2` for Raven, the same source with clang for native and for Rosetta 2; ms, one run each, host = Apple-silicon Mac with other sessions' VMs running, so +-10 %; the checksums are identical in all columns):

| kernel | native arm64 | Rosetta 2 | Raven + JIT | Raven + Unicorn |
|---|---|---|---|---|
| fib(35) | 26 | 26 | 22 | 135 |
| crc32 bitwise, 4 x 1 MiB | 30 | 33 | 49 | 77 |
| float sqrt/div, 20 M | 16 | 18 | 28 | 290 |
| random read-modify-write, 20 x 4 MiB | 19 | 18 | 31 | 1070 |
| float vector sum (SSE2, auto-vectorised), 2000 x 64 K | 101 | 97 | 26 | 1241 |
| memcpy 400 x 1 MiB (`rep movsq`) | 7 | 11 | 8 | 11457 |
| pointer chasing, 20 M | 132 | 164 | 140 | 539 |

In the arm64 VM (HVF, other VMs running) the JIT column is the same within noise (22/49/28/32/25/18/138) against Unicorn at 456/155/385/1216/1464/-/784. The JIT is within 0.85-1.75x of Rosetta 2 on these kernels (the vector sum is faster than native because the native build was not vectorised); its code is about 2x the native instruction count on tight integer loops (lazy flags, guest registers in x0-x15, one `add` for the window offset). **Start-up** is not engine-bound: Notepad++ shows its window about 9 s after the process starts in the VM with either engine (screen sampled every 2 s; Raven's own start-up work dominates, e.g. `hallo64.exe` needs 2.3 s on the host with almost none of it inside the engine); the first JIT version needed more than 60 s because every unmap flushed all translations (section 17). After start-up the JIT engine matters for what the program computes, not for what Raven does.

Checked: native macOS arm64: `hallo64.exe`, `hallo32.exe`, `cmd.exe /c "echo ... & ver"`, the benchmark; arm64 VM (HVF, `DISPLAY=:0`, CloakServer, PureDarwin libSystem): `hallo64.exe` and `cmd.exe` on the JIT, `hallo32.exe` on Unicorn (32-bit), Notepad++ (window, toolbar, typing; `RAVEN_JIT=1`), the Inno Setup installer (first wizard page, "Select Setup Install Mode"). How to repeat: native build as in the section above plus `-DRAVEN_JIT_DIR=<repo>/xlat/jit`, run `RAVEN_JIT=1 raven --run-process --execution=native --stdio=inherit prog.exe`; for the VM build the way `raven/build.sh` does (needs `PKG_CONFIG_LIBDIR` pointing at the image's sysroot, otherwise Homebrew's pixman is linked dynamically), write `raven` into `/usr/libexec/raven` of a copy of the image and start Notepad++ with `RAVEN_JIT=1 DISPLAY=:0 raven --run-process --execution=native --stdio=inherit --desktop=x11 npp/notepad++.exe`.

**Open points.** (1) 32-bit guests run on Unicorn: the decoder knows only 64-bit mode (REX, 8-byte stack slots, no `fs:` TEB in LDT form); compatibility mode would be a second decoder/translator configuration (32-bit address wrap, `inc/dec` opcodes `40`-`4f`, `push/pop` of 4 bytes, `call`/`ret` 4 bytes, segment overrides, `int 0x2e`/`sysenter` conventions). (2) x87 is not translated (Unicorn executes it, the state lives there); a 32-bit-style `fld/fstp` loop pays 20 us per run. (3) Only one JIT process per Raven run; the engine has one translation cache and one window per host process, a second process needs its own window or per-process state in `jit.c`. (4) Guard pages and read-only pages smaller than 16 KiB do not fault, see section 17; a program that relies on that (some anti-debug or stack-probing code) behaves differently from Unicorn. (5) `lock cmpxchg`/`lock xadd` on misaligned addresses fault (LSE atomics). (6) Parallel guest threads (`enable_parallel`) are still unsupported. (7) `RAVEN_JIT_AVX=1` was tested with instruction-level tests and a small x86-64 program (`t-avx`), not with a real AVX Windows program (none was available offline); FMA is not reported, so AVX2 code that assumes the whole Haswell set (e.g. built with `/arch:AVX2`) takes its fallback path or fails its feature check. (8) Not measured: Wine-sized programs, Direct3D/Vulkan, the full `brand.sh` image build with the changed `build.sh`, real hardware.
