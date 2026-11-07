# cloke: package manager

`overlay-desktop/usr/bin/cloke` (Python 3, one file). Tests: `tools/test_cloke.py` (17, no network, no real package manager).

## Commands

| Command | What it does |
|---|---|
| `cloke search <name>` | every version from every backend, with the version number and a recommendation |
| `cloke info <name>` | id, version, description, where it installs, download size and checksum, installed state |
| `cloke install <name>` | list, ask (`-y` takes the recommendation), install, remember in the state file |
| `cloke remove <name>` | removes what cloke installed (app bundle, `brew remove`, `apk del`) |
| `cloke update` | apps newer in the index are reinstalled; `brew update && upgrade`; `apk update && upgrade` (the whole Alpine) |
| `cloke list` | name, version, OS, backend, scope |
| `cloke backends` | what this system has, sudo, paths |
| `cloke bootstrap brew` | runs `clokeos-install-homebrew` (as a normal user) |
| `cloke cache [clear]` | size of `~/.cache/cloke`; `clear` empties it |

Flags: `-y`, `--from apps|brew|apk|dnf|apt|pacman|flatpak|appimage|winget|choco`, `--os`, `--json`, `--system` / `--user`, `--offline`.
Environment: `CLOKE_APPS_INDEX` (other index, file or URL), `CLOKE_OFFLINE=1`, `CLOKE_LINUX_LAYER`, `TUX_ROOT`.

## Backends

- **apps**: `index.json` of `cloke-tech/ClokeOS-Apps` (cached 10 minutes, offline copy kept). Apps go to `~/Applications` (user) or `/Applications` (`--system`); apps of category `system` and packages such as `webkit` are system-wide only. The package is verified against the SHA-256 of the index before anything is unpacked and stays in the cache.
- **brew**: found in `/opt/homebrew/bin` or `/usr/local/bin` even when PATH lacks it. Installs with `--force-bottle`. Versions come from `brew info --json=v2`; a formula without a Big Sur bottle (`arm64_big_sur` / `big_sur` / `all`) is marked "cannot be installed" because building from source needs a compiler. Versioned formulae (`python@3.13`, ...) are offered as extra choices. As root, brew runs as the owner of its prefix (Homebrew refuses root).
- **apk**: `tux -r /TUX /sbin/apk ...`, Alpine v3.24, aarch64 or x86_64. On a system with `/TUX`, the dnf / apt / flatpak catalog entries are not offered (they cannot be installed in an Alpine).
- Windows: Raven, installers from the winget-pkgs manifests. winget / choco only on a real Windows.

## Scope and root

Not root: user scope. `--system`: write directly where the folder is writable (on ClokeOS `/Applications` is group `staff`), otherwise re-run itself under `sudo` if there is one, otherwise stop with a message. Root: system scope by default; state in `/var/lib/cloke/installed.json`, user state in `~/.local/share/cloke/installed.json`. `apk` always needs root (writes `/TUX`). Nothing in cloke calls sudo unless it exists, so the account work (sudo, admin user) can arrive later without changes.

## Tested (arm64 VM, ClokeOS 2.8 desktop image, HVF, 2026-11-07)

- `cloke install dillo` from the real `ClokeOS-Apps` release into `~/Applications` as the guest user; `list`, `info`, `remove`, `update` (up to date).
- `--system` install of Calculator as root, `remove`; as guest without sudo: clear "not writable ... no sudo" message, nothing downloaded for user-scope refusals.
- `brew`: `clokeos-install-homebrew`, then `cloke install --from brew figlet` (Big Sur arm64 bottle, 677 files), also as root (drops to the prefix owner), `update`, `remove`. `jq`, `htop`, `python` have **no** Big Sur bottle any more: cloke says so instead of failing in a compiler search.
- `apk`: search, install, `tux ... figlet` runs, `update` (upgrades Alpine), `remove`, as root. As guest: message that root is needed.
- Cached package reinstall without download.

## Found and fixed on the way

- `cloke` did not start on ClokeOS at all: Python's `urllib.request` imports `_scproxy`, which does not load there. A stub module is inserted.
- The ClokeOS Python has no `ssl` module: all https goes through `curl` when `ssl` is missing.
- `subprocess` with `preexec_fn` from a worker thread aborts Python 3.13 on ClokeOS (fork in a thread); privilege drop uses `user=` / `group=`.

## Gaps (not tested or not done)

- x86_64 (brew under `/usr/local`, apk x86_64): code is the same, never run here.
- dnf, apt, pacman, flatpak, winget, choco, AppImage on their own systems; the Raven installer path (Raven is x86_64 only) – only parsers and ranking are unit-tested.
- Real `sudo` (does not exist on ClokeOS yet): the `sudo -E python3 cloke` re-exec is untested.
- `--offline` is unit-tested and the cached package path was run in the VM; a real network cut was not.
- Searching apk needs an up-to-date Alpine index (`cloke update` as root refreshes it); Alpine installs are system-wide only.
- Casks (macOS GUI apps from Homebrew) are for real Macs; most will not run on ClokeOS.
- Signatures: only SHA-256 from the index (which comes from GitHub over https).
- No graphical store yet ("Programme/Store"); the optional GUI was left out.
