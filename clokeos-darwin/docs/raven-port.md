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
