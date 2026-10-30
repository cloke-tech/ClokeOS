# Installing a macOS `.pkg` on ClokeOS, and the small C libraries Mac programs expect

Status 2026-10-30. Work packages **P** (installer) and **B** (plain C libraries) of [logic-pro-gap.md](logic-pro-gap.md). Nothing of Logic Pro itself is in the repository: the package and every extracted tree were scratch files and are deleted.

## 1. `clokeos-pkginstall`

`macapps/pkginstall.c` (one file, about 600 lines, links only libSystem and libz) installs a flat xar package the way macOS' `installer` does. The name differs from the Recovery's `clokeos-installer` (which installs ClokeOS itself).

```
clokeos-pkginstall [-target /] [-n] [-v] [-appledouble] <package.pkg>
```

| Option | Meaning |
|---|---|
| `-target <root>` | target volume (default `/`); files go to `<root><install-location>` |
| `-n` | list only (`mode size path`), write nothing |
| `-v` | print every path |
| `-appledouble` | keep `._name` files (default: skip them, see below) |

`<package.pkg>` may be a regular file or a block device (the VM test reads the package from a raw virtio disk).

What it does:

1. Reads the xar header and the zlib-compressed TOC (XML), walks the `<file>` tree. Every `Payload` is installed, so product archives with several component packages work (checked with a `productbuild` archive, not with several real components).
2. Takes `identifier`, `version` and `install-location` from the sibling `PackageInfo`.
3. Streams `Payload` through: pread from the package, [xar-level zlib if the TOC says so], gzip (or raw cpio), cpio parser (`odc` and `newc`), file system. There is no temporary file; memory use is independent of package size, so a 1.7 GB package needs only the disk space of the expanded files.
4. Creates files with their cpio mode (including setuid/sticky), mtime, root:wheel ownership when run as root, symlinks (with mtime), hard links (data may sit on the first or the last link, both orders handled). Existing directories keep their permissions (`overwrite-permissions="false"`); new directories get their final mode and mtime after all children exist, so a `0555` directory can still be filled.
5. Rejects `..` path components, device nodes, sockets and FIFOs (none occur in packages).
6. Writes `/var/db/receipts/<id>.plist` (`PackageIdentifier`, `PackageVersion`, `InstallPrefixPath`, `InstallDate`, `InstallProcessName`, `PackageFileName`) and `<id>.files` (`path<TAB>mode<TAB>size` per entry). No BOM is generated, so `pkgutil --files` style tools would not find a bill of materials.

Build: `macapps/pkginstall.sh` (x86_64), `CLOKEOS_ARCH=arm64 macapps/pkginstall.sh`, or `macapps/pkginstall.sh host <file>` for the Mac. `macapps/install.sh` now builds and stages it as `/usr/sbin/clokeos-pkginstall` (71 KB on arm64).

### AppleDouble files

`pkgbuild` on a Mac archives extended attributes as `._name` entries next to the file. ClokeOS' file system has no use for them, so they are skipped by default and counted in the summary. They also share inode and link-count fields with their file, which would confuse hard-link handling. The Logic package contains none.

### Results

The Logic Pro 12.3 package (1,699,772,139 bytes of payload, `PackageInfo` says 104,727 files and 3,140,652 KB):

| Run | Result |
|---|---|
| `-n` on the Mac | 104,727 entries, 3062 MB, 5 s |
| Install on the Mac into a scratch directory, 20 s | 104,726 entries (the install location itself existed) |
| `macapps/pkgverify.py` against `/Applications/Logic Pro.app` | **identical**: 86,781 files with equal SHA-256, 170 symlinks with equal targets, 17,775 directories, equal modes |
| Install in the arm64 ClokeOS VM (HVF) from a raw virtio disk holding the package, onto the ext4 root, about 6 minutes | 104,726 entries, 3062 MB |
| The installed tree read back from the VM image with `debugfs rdump` and compared the same way | **identical** (same counts, SHA-256, modes, symlinks); owner root:wheel (checked on the inode of `Logic Pro.app` and the receipt files only) |

Also tested with small synthetic packages (`pkgbuild` flat packages and a `productbuild` archive; `cpio -H odc` and `-H newc` payloads with hard links, empty files, symlinks, a read-only directory, an XML-special character in a name, a 6 MB file with an old mtime), and once under AddressSanitizer, which found and fixed a heap overflow while writing the code.

`macapps/pkgverify.py A B [--list file]` is the comparison tool (single process, SHA-256 of every file). It does not compare owner or mtime. Timestamps were set from the cpio but not compared.

### How the VM test was set up (recorded because it hit two real problems)

* The desktop image's root is only about 4 GB. The test used an APFS clone of an arm64 desktop image, grown by 7 GB: the GPT's last partition and its backup header were rewritten and `resize2fs -f image?offset=N` run on the host. **Trap:** `resize2fs` on an image file truncates the file to the file system's end, which cut off the backup GPT; the kernel then silently published only the first partition and the boot stopped at the root device. The file had to be re-extended and the backup GPT written again.
* The package was attached as a second, read-only virtio disk (`./run.sh --headless --image … -- -drive if=none,id=pkg,format=raw,file=pkg-clone,readonly=on -device virtio-blk-pci,drive=pkg`). The guest sees it as `/dev/disk0`; `clokeos-pkginstall /dev/disk0` works because it only needs `pread`.
* The new files were written into the image with `debugfs` (the same way `macapps/probe-image.sh` does).
* **Kernel panics found (not fixed):** in the guest, `mount -t ext4 <second disk partition>` panics in `Ext4FileSystemDriver` (NULL dereference at offset 0x78), and `mount -t hfs` on a disk without an HFS+ volume panics in `hfs.kext`. This matches the comment in `recovery/installer.c` about ext4 mounts and the fact that `cloakserver/automount.c` mounts only FAT and HFS+ (HFS+ read-only). So a second data volume as install target is not an option today; installing onto the root file system is. Mounting an HFS+ volume the way `mount(8)` does gave `Permission denied`; automount passes its own argument block.

### Not done / not tested

* **x86_64 was built but never run.** No x86_64 VM run (TCG takes minutes to boot). The code is architecture-independent and the arm64 and host runs agree.
* The archived/extracted checksums in the TOC (SHA-1) are **not verified** while streaming; the gzip CRC protects the payload. The xar TOC checksum is verified by libxar (`libstest` reads the Logic package through it, see below), not by `pkginstall`.
* `Scripts` is not run (Logic's is empty). `pbzx`/xz and bzip2 payloads are refused with a message. Extended attributes, resource forks, ACLs and Finder flags are not restored.
* Package signatures are ignored. Distribution `choices`, `installation-check` and `volume-check` scripts are not evaluated.
* Receipts have no BOM. Nothing registers the app with LaunchServices, Spotlight or Quick Look (not needed to start it, per gap analysis section 4).
* Hard links with nlink > 1 were not present in the Logic payload, so that code path was only checked on synthetic data (newc with data on the last link).
* The process was not tested with a full disk or an interrupted package; it stops with an error at the first write failure and leaves what it wrote.

## 2. Plain C libraries (`macapps/libs.sh`)

Built as dylibs with Apple's install names and versions, for x86_64 and arm64, against PureDarwin's libSystem and libz (as `accelerate.sh` does).

| Library | Install name | compat / current | Source | Notes |
|---|---|---|---|---|
| libsqlite3 | `/usr/lib/libsqlite3.dylib` | 9.0.0 / 377.0.0 | SQLite 3.46.1 amalgamation | FTS3/4/5, RTREE, JSON, math functions, session, preupdate hook, stat4, column metadata, URI filenames; threadsafe=2 |
| libxml2 | `/usr/lib/libxml2.2.dylib` (+ `libxml2.dylib`) | 10.0.0 / 10.9.0 | libxml2 2.9.14 | with zlib, threads, HTTP/FTP, modules; **no iconv, no lzma, no ICU** (only UTF-8/16/ASCII/Latin-1 encodings) |
| libbz2 | `/usr/lib/libbz2.1.0.dylib` (+ `libbz2.dylib`) | 1.0.0 / 1.0.8 | bzip2 1.0.8 | |
| libpcre | `/usr/lib/libpcre.0.dylib` (+ `libpcre.dylib`) | 1.0.0 / 1.1.0 | PCRE 8.45 (PCRE1) | UTF-8 and Unicode properties, no JIT |
| libxar | `/usr/lib/libxar.1.dylib` (+ `libxar.dylib`) | 1.0.0 / 1.3.0 | xar 1.6.1 (mackyle) | digests (md5, sha1, sha256, sha512) from `libs/xar-shim/` instead of OpenSSL; no lzma |

The five sources (9 MB together) are fetched with `fetch_file` and a SHA-256 into `~/ClokeOS-work/download/libs/`. Output: `~/ClokeOS-work/out/macapps/root/usr/lib/` (x86_64) or `…/macapps-arm64/root/usr/lib/`; `macapps/install.sh` copies them into the image next to the other Mac frameworks. `macapps/libs.sh host` builds for the Mac under `out/libs-host/` and runs the test.

Differences from Apple's libraries are tracked honestly: the version numbers are *claimed* to match (dyld only checks compatibility versions) but the code is the upstream project, not Apple's patched tree. The libxml2 version (2.9.14) was picked to match the 2.9.x line behind Apple's `10.9.0`; Apple's exact patch level was not checked, so behaviour differences are possible.

Things the build found missing in PureDarwin's libSystem: `rand_r` (added hidden in `libs/compat.c`, used by libxml2's dict), and `chflags`, `lchmod`, `strmode` (xar's fallbacks used instead, so xar does not restore BSD file flags). Nothing else was missing among the symbols these five libraries use.

### Test

`libs/libstest.c` (installed as `/Library/ClokeOS/Probe/libstest`) calls each library: SQLite create/insert/FTS5/JSON, libxml2 parse + XPath + entities, bzip2 round trip of 200 KB, PCRE compile/exec with captures, libxar write → close (TOC with SHA-1) → read back with checksum verification, and with a package path as argument it reads the package's TOC. Result: **ALL OK on the Mac (arm64), on the Mac with the Logic package (9 TOC entries), and in the arm64 ClokeOS VM** (same output). The x86_64 builds link but were not run.

The digest shim was checked against `md5`/`shasum` for lengths 0, 1, 55, 56, 63, 64, 65, 111, 112, 119, 120, 128, 1000 and 100000 bytes.

### Which of Logic's imports now resolve

Logic Pro's Mach-O files that depend on these libraries (9 files, x86_64 and arm64 slices):

| File | Libraries |
|---|---|
| Logic.framework/Logic | sqlite3, xml2, bz2, xar |
| MAAlchemy | sqlite3, pcre |
| MAFiles, MAContentTagging, MACore, Impulse Response Utility | xml2 |
| MALoopManagement, MAUniversalContent, MASearchKit | sqlite3 |

Every imported symbol from libsqlite3, libxml2 and libpcre (both architectures; `dyld_info -imports` against `dyld_info -exports` of the built dylibs) is exported: 0 missing. `Logic` lists libbz2 and libxar as dependencies but imports **no symbol** from either, so they are only needed so that the load command resolves. This checks names only; it does not mean the call sequences work, and libsqlite3's `compat 9.0.0 / current 377.0.0` is what Logic's load commands ask for.

The gap analysis counted 44 sqlite, 22 xml2 and 4 pcre imports (unique symbols); the check above counts them per file and slice, so the totals differ.

### Not covered

* **libAppleArchive** (19 imports in 2 binaries, named in package B) is not built: it is Apple's own format library with no open source for the SDK-26 ABI.
* Not run on x86_64 ClokeOS; not run inside Logic.
* The libraries are not in the Recovery system (`macapps/install.sh` only runs for the desktop image).
* `machcheck.py` (which needs the image) was not run; the symbol check above used `dyld_info` directly.

## 3. Files

| Path | What |
|---|---|
| `macapps/pkginstall.c`, `macapps/pkginstall.sh` | installer and its build script |
| `macapps/pkgverify.py` | tree comparison (paths, kinds, modes, sizes, link targets, SHA-256) |
| `macapps/libs.sh` | builds the five libraries, `libstest`, and runs the test in `host` mode |
| `macapps/libs/xar-shim/` | `openssl/evp.h` and `evp-shim.c` (MD5, SHA-1, SHA-256, SHA-512 for libxar) |
| `macapps/libs/compat.c` | hidden `rand_r` |
| `macapps/libs/libstest.c` | test program |
