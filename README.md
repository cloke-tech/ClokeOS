# ClokeOS

ClokeOS is an operating system built on the Darwin kernel (XNU), with a cape as its logo. Its default look is macOS-like, but the goal is one system with three worlds: **Mac** (apps from macOS on Darling's frameworks), **Windows** (a Windows-like interface today, `.exe` programs through Wine, in a tree of their own at `/NT`) and **Linux** (programs in a tree of their own at `/TUX`). What works today and what is still planned is listed under [One system, three worlds](#one-system-three-worlds).

**Download:** the [latest release](https://github.com/cloke-tech/ClokeOS/releases/latest) has USB installers for PCs (offline and online), a ready-made SSD image and installer images for virtual machines (x86_64 and arm64). See [Installing ClokeOS](#installing-clokeos).

![ClokeOS desktop: Fundus (Thunar) and Terminal over the papercut wallpaper, the Dock below, a glass menu bar on top](clokeos-darwin/screenshots/desktop.png)

## One system, three worlds

ClokeOS starts as a macOS-like system and grows towards running software from other systems as well, each in its own tree so that the worlds do not mix:

| World | Works today | Planned |
|---|---|---|
| **Mac** (default interface "ClokeOS") | macOS-like desktop (CloakServer), Mac apps on Darling's frameworks (with many gaps), Fundus, Launchpad, Spotlight, System Settings, recovery and installer | bash and Homebrew, a modern browser |
| **Windows** | the **NT interface**: a Windows-like desktop with taskbar, Start menu, tray and desktop icons (System Settings > Appearance > Interface) | `.exe` programs through Wine 11.0 (x86_64 only): own tree `/NT` as drive C:, `wine` on the PATH, double-click in Fundus, "Wine" app; 64-bit and 32-bit console programs and notepad run in the VM | Windows programs on arm64 (needs the translation layer), more tested programs |
| **Linux** | first version (arm64 and x86_64) | programs in their own tree `/TUX` (Alpine: musl, BusyBox) run through `tux`, a user-space loader that translates Linux system calls; BusyBox `sh`, `ls`, `cat` work on both, and so do threads, sockets, epoll and `/proc`; Debian's glibc programs run on both (see [Linux programs](#linux-programs-tux)) |
| **Other CPU** | images for x86_64 and arm64; **x86_64 programs on arm64** through the translator `ucrun` (see [x86_64 programs on arm64](#x86_64-programs-on-arm64-ucrun)) | a translation layer between arm64 and x86_64 in both directions, like Rosetta 2; see the [feasibility study](clokeos-darwin/docs/translation-study.md) |

![The NT interface: square windows with the buttons on the right, a taskbar with the Start button, the running apps, speaker, network and clock, and icons on the desktop](clokeos-darwin/screenshots/nt-desktop.png)

![The NT Start menu: all apps, System Settings, Sleep, Restart, Shut Down and Log Out](clokeos-darwin/screenshots/nt-start.png)

## What is there

ClokeOS consists of two parts:

- **The real system** in `clokeos-darwin/`: a real Darwin kernel (XNU, Darwin 20.5.0) from [PureDarwin 20.5](https://github.com/PureDarwin/PureDarwin/releases/tag/20.5), with ClokeOS' own window server CloakServer (menu bar, Dock, Launchpad, login, setup assistant, recovery), a global menu bar for GTK and Mac apps, Mac frameworks from Darling, its own installer and recovery, USB installers for real PCs and its own kernel drivers.
- **The UI prototype** `clokeos.html`, which runs in any browser and was the model for CloakServer:

```bash
python -m http.server 8765    # then open http://localhost:8765/clokeos.html
```

The prototype has the boot screen with the cape logo (key **V** shows a verbose XNU boot log), login, menu bar, Dock with magnification, draggable windows, a Liquid Glass look with the "Golden Gate" wallpaper (a hand-drawn suspension bridge in fog, light by day and dark in the evening), apps (Fundus, Search, Terminal, Notes, Calculator, Calendar, Activity Monitor, System Settings), search, Launchpad and a virtual file system with Darwin's layout that is kept in the browser.

## Installing ClokeOS

### On a PC (x86_64, USB stick)

There are two sticks:

- `clokeos-x86_64-usb-offline.img.xz` carries the whole system and installs without internet. The right choice for most PCs.
- `clokeos-x86_64-usb-online.img.xz` is small and downloads the system while installing; this needs a LAN cable and a supported network chip (see below).

1. Download, unpack (`xz -dk`) and write to a USB stick of at least 2 GB, for example with balenaEtcher, or on a Mac: `diskutil list`, `diskutil unmountDisk /dev/diskN`, `sudo dd if=clokeos-x86_64-usb-offline.img of=/dev/rdiskN bs=4m` (N is the stick; everything on it is erased).
2. Plug in a USB keyboard and mouse if the PC has no built-in PS/2 keyboard and touchpad, then choose the stick in the firmware's boot menu (UEFI without CSM/legacy, Secure Boot off, SATA in AHCI mode, Intel VMD/RST off). Loading takes one to two minutes; holding **V** shows the verbose boot.
3. In Recovery choose "Install ClokeOS" and pick the SATA or NVMe SSD. The SSD is erased. If the recovery together with the system does not fit into the memory below 1 GB (depending on the firmware only about 400 MB may be free there in one piece), the offline stick starts a slim variant that reads the system from the stick while installing.
4. Restart and remove the stick. On first start the setup assistant asks for language, region, your account and the appearance.

![Recovery: window "ClokeOS Recovery" with Install ClokeOS, Get Help Online, Disk Utility and Terminal; menu bar with Recovery, Utilities and Window](clokeos-darwin/screenshots/recovery.png)

![Setup assistant after installing: "Select Your Language" with English and Deutsch](clokeos-darwin/screenshots/setup.png)

Requirements: UEFI, a SATA or NVMe SSD. Keyboard and mouse over USB (plugged in at power-on) or a built-in PS/2 keyboard and PS/2 touchpad (ClokeOS' own driver ClokePS2); I²C touchpads, as in most notebooks from about 2015 on, do not work yet. If ClokeOS finds no keyboard or mouse, it shows a diagnostics window after 45 seconds to take a photo of. Network only over LAN with Realtek RTL8111/8168/8169 or Intel 82545EM; Wi-Fi and newer Intel network chips (I219, I225) are not supported. Display through the firmware's graphics (GOP).

### Without a USB stick: ready-made SSD image

`clokeos-x86_64-disk.img.xz` is an installed SSD with system and recovery, no internet needed: connect the PC's SSD to a Mac with a USB-SATA or USB-NVMe adapter, unpack and write it (`sudo dd if=clokeos-x86_64-disk.img of=/dev/rdiskN bs=4m`, or balenaEtcher), put it back into the PC and boot. The setup assistant runs on first start. ClokeOS uses a bit more than 5 GB, the rest of the SSD stays free.

### In a virtual machine (QEMU)

Unpack `clokeos-<arch>-installer.img.xz` (x86_64, or arm64 for Macs with Apple silicon) and start it with an empty second disk, for example with `clokeos-darwin/run.sh` from the source: `CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop ./run.sh --image clokeos-arm64-installer.img --disk disk.img`. Recovery then downloads the system from this repository's releases.

Later, "Restart in Recovery…" in the ClokeOS menu starts the recovery on the disk, on x86_64 also the key **R** at power-on. From there ClokeOS can be reinstalled; the recovery stays.

## The real system on XNU

```text
root@clokeos / # sw_vers
ProductName:		ClokeOS
ProductVersion:		2.3
BuildVersion:		2C1
root@clokeos / # uname -a
Darwin clokeos 20.5.0 Darwin Kernel Version 20.5.0: Sun Aug 23 10:54:37 UTC 2026; nixbld:xnu-build/RELEASE_X86_64 x86_64
```

![ClokeOS booting in QEMU: the cape logo, white on black](clokeos-darwin/bootlogo.png)

The base is the ready-made image of [PureDarwin 20.5](https://github.com/PureDarwin/PureDarwin/releases/tag/20.5) (UEFI, its own loader, XNU, launchd, zsh, ext4 as root file system). ClokeOS changes:

- `SystemVersion.plist`: ClokeOS 2.3 "Presidio", build 2C1 (launchd also sets `kern.osversion` from it)
- `sw_vers` reads the plist as on macOS; PureDarwin's own `sw_vers` has "PureDarwin 11.3" compiled in
- host name `clokeos` via the launchd service `com.clokeos.hostname`
- login text with the cape (`/etc/motd`), prompt `root@clokeos / #`
- `clokefetch` as in the prototype's Terminal, volume name "Cloke HD"
- boot logo: the cape instead of the kernel log (a custom build of PureDarwin's loader, see below); holding V at power-on shows the log
- desktop variant: CloakServer on Xorg, started by launchd

Set up once (Homebrew):

```bash
brew install qemu e2fsprogs mtools mingw-w64
```

Build and start:

```bash
cd clokeos-darwin && ./brand.sh && ./run.sh
```

`brand.sh` downloads the PureDarwin image (275 MB) and the boot loader's sources (300 KB together, all checksums are verified), builds the loader with the boot logo, writes the files from `overlay/` into the ext4 file system with `debugfs` (no Linux and no root needed), puts loader and boot arguments into the EFI partition with `mtools` and produces `~/ClokeOS-work/out/clokeos-2.3-x86_64.img`. `run.sh` opens a QEMU window with the screen and the serial console in the terminal. On a Mac with Apple silicon x86_64 is emulated; reaching the prompt takes about 70 seconds (arm64 with Apple's hypervisor: about 7, see [Architectures](#architectures)). Quit with `poweroff` in the VM.

For tests without a window: `./run.sh --headless`, then `./vmctl.py wait 'Welcome'`, `./vmctl.py send 'clokefetch'`, `./vmctl.py shot screen.png`, `./vmctl.py quit`. Downloads, images and VM state live in `~/ClokeOS-work`, not in the project folder. Versions and names are in `clokeos-darwin/config.sh`.

### Boot logo

The kernel is started by PureDarwin's boot loader [xnu-loader](https://github.com/PureDarwin/xnu-loader) (`EFI/BOOT/BOOTX64.EFI`). ClokeOS builds it itself with a change: without `-v` in the boot arguments it draws the cape (path `LOGO_D` from `clokeos.html`) via GOP, centered on black, 12 % of the screen height, before starting the kernel. Its own log then only goes to the serial port; errors still also go to the screen.

After that the screen belongs to the kernel, and PureDarwin's kernel is built for debugging: given a framebuffer it paints colored progress bars into the top lines and, while the text console is off, every character of the serial output onto the screen, even later over a desktop. Boot arguments cannot turn this off. All these painting routines only draw at 32-bit color depth, though, and XNU's video console does not know 24 bit at all, while PureDarwin's graphics driver `IOGOPFramebuffer` treats 24 bit exactly like 32. Without `-v` the loader therefore hands the kernel the same framebuffer in graphics mode, only marked as 24 bit. The kernel then leaves the logo alone, `/dev/fb0` exists and Xorg can draw. As soon as `IOGOPFramebuffer` runs, XNU draws its progress bar below the logo, just like the boot screen in `clokeos.html`.

The boot arguments are `CLOKE_BOOT_ARGS` in `clokeos-darwin/config.sh`: `debug=0x218 -nogzalloc_mode keepsyms=1 serial=3 gen9_debug=1`, PureDarwin's default without `-v` and `gopconsole=1`. `serial=3` must stay, console, terminal and `vmctl.py` run over it. Holding **V** at power-on gives a single verbose boot: the loader looks twice without waiting (right after reading the boot arguments and again once the kernel is loaded), appends ` -v gopconsole=1`, clears the logo and hands the kernel the framebuffer as text console. A short tap is not enough: the firmware empties the keyboard buffer right before starting the loader, and in QEMU the loader is done after about 0.25 seconds; only key repeat of a held key reliably lands in that window.

The loader is built by `clokeos-darwin/loader/build.sh`, which `brand.sh` calls: it unpacks xnu-loader at `8295cd89` (the same as in PureDarwin 20.5) and gnu-efi 4.0.2, applies `loader/xnu-loader.patch`, adds `splash.c` and turns `LOGO_D` into outline data with `logo.py`. The loader rasterizes the logo at boot in the current resolution, so it is sharp on every screen. It is compiled with mingw-w64, which produces EFI programs directly; the build is reproducible.

Network and disk: `run.sh` gives the x86 VM virtio devices (`virtio-blk-pci`, `-nic user,model=virtio-net-pci`), which PureDarwin has drivers for and which cost less under emulation than an emulated AHCI disk and Intel card. `CLOKEOS_VM_DEVICES=classic` uses AHCI and an Intel 82545EM (`e1000-82545em`), the only card PureDarwin's `PDE1000` recognizes. The mouse is a USB mouse (`usb-mouse`), since PureDarwin's HID driver only knows boot-protocol mice.

Limits: the VM sees one CPU core (`hw.ncpu` = 1), see [Drivers and performance](#drivers-and-performance). Kernel and drivers come from PureDarwin, apart from 15 bytes in `IOGOPFramebuffer` (below) and ClokeOS' own drivers ([Own kernel drivers](#own-kernel-drivers)).

#### Wrong colors in PureDarwin's graphics driver

During PCI configuration PureDarwin's `IOPCIFamily` moves the console and writes its address back with bit 0 set (`0x80000001` instead of `0x80000000`; in XNU that means "physical address"). `IOGOPFramebuffer::start()` takes that address unfiltered as framebuffer base. `/dev/fb0` and Xorg's mapping are then one byte off the screen: blue shows as green, white as yellow, red as black. `clokeos-darwin/kcfix.py` fixes this in the kernel collection (`EFI/BOOT/kernel`): the driver is compiled without optimization, so 15 bytes right after `getConsoleInfo()` can be replaced by equally long instructions that round the base address down to 4 bytes. The script finds the code by its surrounding pattern and only changes it on exactly one match. PureDarwin has fixed this in its development branch `upstream-xnu-12377` (commit `867967e`).

### Desktop: CloakServer

![The same desktop in dark mode: dark window frames on the plum wallpaper, the Dock as lighter violet glass](clokeos-darwin/screenshots/desktop-dark.png)

The desktop variant builds on PureDarwin's preview image `puredarwin.img` (6.1 GiB, from puredarwin.org; Xorg with PureDarwin's own driver `puredarwingop`, Wayland/sway, i3, Xfce, Wine, compilers). Nothing graphical starts there by itself; ClokeOS starts Xorg with CloakServer at boot through the launchd service `com.clokeos.cloakserver` (`/usr/libexec/cloakserver-start` and `cloakserver-session`, with a D-Bus for the session):

```bash
cd clokeos-darwin && CLOKEOS_FLAVOR=desktop ./brand.sh && CLOKEOS_FLAVOR=desktop ./run.sh
```

CloakServer (`clokeos-darwin/cloakserver/`) is ClokeOS' window server, for now an X11 window manager with its own UI, written in C with Xlib, cairo, pango and librsvg:

- Wallpaper: pink papercut layers (`wallpapers/make.swift` builds the light and the plum dark variant from the artwork into `overlay-desktop/Library/ClokeOS/Wallpapers/`, `wall.c` draws it covering the screen); the old "Golden Gate" bridge stays as a fallback if the files are missing. System Settings > Wallpaper offers 18 wallpapers: this default, four drawn ones from the prototype (Velvet, Dusk, Tide, Graphite, each light and dark) and 13 artworks by Pixel Makes Art (see Credits); the artworks are 3840 px wide (4K); the choice is saved as `ClokeWallpaper` in `~/Library/Preferences/.GlobalPreferences.plist`. The HTML prototype still has the bridge
- Menu bar: ClokeOS menu (About ClokeOS, System Settings, Activity Monitor, Applications, Sleep, Restart…, Shut Down…, Restart in Recovery…, Log Out), the active app's name and menus, network with IP address, search (opens Launchpad), appearance (auto, light, dark), clock with date
- Global menu bar as on macOS: the active app's menus sit at the top next to the cape, not in the window, with submenus, check marks and shortcuts written as on macOS (⌃⌥⇧⌘). GTK apps like Fundus (Thunar) and Terminal export their menu bar via the GTK module `appmenu-gtk-module` over D-Bus (from vala-panel-appmenu 25.04, LGPL-3, with a small patch, `menubar/appmenu-clokeos.patch`); the helper `cloakmenu` reads it as the logged-in user and passes it to CloakServer. Mac apps put their menu into the window property `_CLOKE_MENU`; Darling's AppKit does that (`macapps/appkit/clokemenu.m`)
- Language as on macOS: English by default, German in System Settings > Language & Region and in the setup assistant. CloakServer, login, menu bar and app names switch immediately; date and clock follow the language. Strings are English in the code, `L()` translates them (`cloakserver/l10n.c`, translations in `strings-de.h`, also for the installer; `cloakserver/l10n-check.py` reports missing ones). App bundles have English names (Notes.app, Calculator.app …), their German names are in `Contents/Resources/de.lproj/InfoPlist.strings` as on macOS
- Setup assistant as on macOS on first start after an install (`cloakserver/setup.c`): "hello" in changing languages, language, country or region (sets the time zone), your own account with full name, account name and password, appearance; the new account is then logged in. It is triggered by `/var/db/.RunClokeOSSetup`, afterwards there is `/var/db/.ClokeOSSetupDone`, like `.AppleSetupDone` on macOS
- GTK theme ClokeOS (light and dark, `overlay-desktop/share/themes`): Adwaita with CloakServer's colors, violet accent, flat toolbars, rounded buttons and fields; CloakServer announces it via XSETTINGS, and running apps switch along with light and dark mode
- Sharing > Remote Login: SSH on port 22, switchable as on macOS; the page shows the matching `ssh` command with the IP address. Behind it is PureDarwin's sshd, started by `/usr/libexec/clokeos-sshd`, which reads the switch in `/etc/clokeos-remote-login`
- Dock with the icons from `clokeos.html`, running indicators, name tags on hover, a context menu (Open, Keep in Dock / Remove from Dock, Show in Fundus, Quit); left, bottom or right in three sizes; it shrinks by itself if it would not fit. Launching apps bounce
- Compositor (`comp.c`): CloakServer composes the picture itself (Composite, Damage, XFixes, Render): soft shadows, smooth rounded corners, glass for Dock, menus and name tags (blurred background with tint and a light edge, as in the prototype's Liquid Glass style). Only damaged areas are redrawn
- Animations: new windows grow in and fade in, "Minimize" shrinks the window into its Dock icon, a click brings it back the same way
- Windows: frame with title bar and traffic lights (close, minimize, zoom), rounded corners, move, resize at the bottom right corner, double-click on the title zooms, Alt+Tab switches, Alt+F4 closes
- "About ClokeOS" as in the prototype, with version and build from `/etc/clokeos-release` and the machine's values (`sysctl`)
- Login as in the prototype, before the desktop and after logging out

![Global menu bar: Fundus (Thunar) with its menus File, Edit, View, Go, Bookmarks, Window and Help at the top next to the cape; the View menu is open](clokeos-darwin/screenshots/menubar.png)

![Login screen: blurred Golden Gate, date and a large clock, the accounts with avatars and a password field, at the bottom Sleep, Restart and Shut Down](clokeos-darwin/screenshots/login.png)

The login shows date and clock over the blurred wallpaper, the accounts with avatars, the password field under the selected account, and Sleep, Restart and Shut Down. Built images have the local user Guest (account `guest`, UID 501, group staff, home `/Users/guest`), created by `brand.sh` from `CLOKE_USER` in `config.sh`; installed systems get the account from the setup assistant. Next to it is root ("System Administrator") for administration, since PureDarwin has neither su nor sudo. Passwords are checked with `crypt()` against `/etc/master.passwd`; a wrong one shakes the field as in the prototype.

CloakServer itself runs as root, like loginwindow on macOS, and starts apps as the logged-in user, with their rights, home and environment and their own session D-Bus. "Log Out" closes all windows, ends all of the user's processes and returns to the login. Shut down and restart are run by CloakServer as root.

#### Apps as on macOS

![Launchpad: a search field and the apps in a grid over the blurred wallpaper](clokeos-darwin/screenshots/launchpad.png)

![Spotlight: the search for "calc" lists the Calculator and a web search with Search](clokeos-darwin/screenshots/spotlight.png)

![Calendar: a month grid with the day list and a field for new events](clokeos-darwin/screenshots/calendar.png)

Apps in ClokeOS are bundles as on macOS: a folder `Name.app` with `Contents/Info.plist` (`CFBundleName`, `CFBundleIdentifier`, `CFBundleExecutable`, `CFBundleIconFile` …), the executable in `Contents/MacOS/` and the icon in `Contents/Resources/`. CloakServer finds them in `/System/Applications` (utilities in `Utilities/`), `/Applications` and `~/Applications`; Fundus (in a theater, the room where costumes and capes hang) lives in `/System/Library/CoreServices` like the Finder. Installing works as on macOS: drag the bundle into `/Applications` or `~/Applications`, remove it with the Trash. Info.plist may be XML or binary, icons SVG, PNG or `.icns`.

Launchpad ("Applications" in the Dock, the magnifier in the menu bar) shows all apps in a grid; typing searches, Return starts the first, Esc closes. The Dock is stored per user in `~/Library/Preferences/com.clokeos.dock.plist` with the keys of `com.apple.dock`, the appearance in `.GlobalPreferences.plist` (`AppleInterfaceStyle`).

**Interfaces.** System Settings > Appearance > Interface switches between ClokeOS (macOS-like, default) and **NT** (Windows-like), stored per user as `ClokeUI` in `.GlobalPreferences.plist` and applied at the next login. NT has square windows with the title on the left and minimize, maximize and close on the right, a full-width taskbar at the bottom (Start button with a menu of all apps and the power items, the apps with a bar under running ones, speaker and network in the tray, the clock) and icons on the desktop (Fundus, System Settings, Search, Trash; double-click starts them). The top bar with the global menu stays, because the GTK apps export their menus there; a Windows-style tray with real volume and network controls is not built yet. The profile is a runtime switch in `cloakserver/cloakserver.c` (`nt`, `TITLE_H`, `RADIUS`, `paint_frame_nt`, `paint_taskbar`).

The system apps are generated by `cloakserver/bundles.py`; their executables are small scripts in front of programs from the PureDarwin image:

| App | Location | behind it |
|---|---|---|
| Fundus | `/System/Library/CoreServices` | Thunar |
| Applications | `/System/Applications` | Launchpad in CloakServer |
| Search | `/System/Applications` | NetSurf |
| Notes | `/System/Applications` | nano in xfce4-terminal |
| Calculator | `/System/Applications` | xcalc |
| Calendar | `/System/Applications` | `/usr/libexec/clokeos-kalender` in xfce4-terminal |
| System Settings | `/System/Applications` | a window in CloakServer |
| Terminal | `/System/Applications/Utilities` | xfce4-terminal with the prototype's colors and font |
| Activity Monitor | `/System/Applications/Utilities` | `/usr/libexec/clokeos-aktivitaet` in xfce4-terminal |
| Xfce Settings | `/System/Applications/Utilities` | xfce4-settings-manager for everything System Settings lacks |
| Dillo | `/Applications` | the Dillo browser, as an example of an added app |

**Mac apps.** ClokeOS is Darwin 20.5, the foundation of macOS 11, and runs Mach-O programs of its architecture (x86_64, arm64 on arm64). A command line program built on the Mac with Apple's SDK (`clang -target x86_64-apple-macos11.0`) runs unchanged. For apps with windows the desktop image brings Darling's Mac frameworks, built for ClokeOS on PureDarwin's real objc4 and dyld (`clokeos-darwin/macapps/`): CoreFoundation, Foundation, AppKit, Cocoa, CoreGraphics, CoreText, QuartzCore and more under `/Library/ClokeOS/MacFrameworks`. AppKit draws in software and copies each window to CloakServer, which manages it like any other window. The example is `Hallo.app` in `/Applications`, a small Cocoa app built universal with Apple's SDK: it starts from Launchpad and Dock, has a menu (shown in CloakServer's menu bar), a text field and a button, and quits with the red traffic light.

![Hallo, a small Cocoa app built with Apple's SDK, running in ClokeOS; its menu is in the menu bar, its icon in the Dock](clokeos-darwin/screenshots/mac-app-hello.png)

Before an app starts, CloakServer reads its executable: architecture (also in universal binaries) and every library it loads. If a framework is only in `/Library/ClokeOS/MacFrameworks`, CloakServer starts the app with `DYLD_FRAMEWORK_PATH`. If something is missing, a notice appears as on macOS instead of a silent failure, and Launchpad marks the icon. Simple apps with classic AppKit controls work; apps with layers, OpenGL, Metal, SwiftUI or Swift do not.

**System Settings** is a window in CloakServer: Appearance, Desktop & Dock, Language & Region, Sharing and Applications. Everything else, such as keyboard and mouse settings, stays in Xfce Settings.

![System Settings: the panes General, Appearance, Wallpaper, Desktop & Dock, Keyboard, Language & Region, Applications and Sharing; Appearance with Light, Dark and Auto](clokeos-darwin/screenshots/settings.png)

CloakServer is built on the Mac, without Linux and without the VM: `cloakserver/build.sh` pulls headers and libraries straight out of the desktop image with `debugfs` and compiles with Apple's clang for `x86_64-apple-macos11.0`, like PureDarwin's own programs (Mach-O for macOS 11, classic dyld info). `cloakserver/resources.py` runs the icon code from `clokeos.html` in macOS' own JavaScript engine (`osascript -l JavaScript`) and writes logo and icons as SVG.

Limits:

- The blur is an approximation, title bars are opaque.
- Animations stutter under emulation (one frame of the moving area takes 150 to 800 ms there).
- Under emulation apps take 10 to 25 seconds to start.
- Mac apps only run as far as Darling's AppKit goes; apps only for Apple silicon do not start on x86_64.
- There is no user management in System Settings yet, and no su or sudo.

### Desktop on arm64

![Login on arm64 with Apple's hypervisor, 12.6 seconds after QEMU starts](clokeos-darwin/screenshots/login-arm64.png)

```bash
cd clokeos-darwin && CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop ./brand.sh && CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop ./run.sh
```

The base is PureDarwin's preview image `puredarwin-arm64-virt.img` (3.1 GiB). With Apple's hypervisor (HVF) the login is there 12.6 seconds after QEMU starts (shell after 7.1 s; x86_64 emulated: almost 90 s), the desktop one second after logging in. What it took, because PureDarwin's arm64 image falls short in several places:

- **Display via virtio-gpu.** `IOGOPFramebuffer` only attaches to a "framebuffer" node on arm64, which PureDarwin only creates for the Raspberry Pi; `run.sh` gives the desktop `virtio-gpu-pci` (1280 × 800), which Xorg's `puredarwingop` drives through `IOVirtIOGPU`.
- **Kernel panic on the first frame.** `IOVirtIOGPU::setConsoleDrawing` turns off the kernel console on Xorg's first frame and writes to `cons_ops_index`, which is read-only in PureDarwin's arm64 kernel; `kcfix.py` replaces that one call with `nop`.
- **Static Xlib only**, **libXcursor** rebuilt from the archive, **no System V shared memory** (SIGSYS ignored), **D-Bus** via `dbus-run-session`, **network and clock** via `com.clokeos.network` (fixed address, time over HTTP), and **Xorg spinning at full load** because PureDarwin's keyboard and mouse drivers answer `poll` with `POLLNVAL` (`cloakserver/xpoll.c` polls them every 10 ms instead).

Darling's Mac frameworks also run on arm64: `macapps/cfarm64.S` and `cfarm64.m` implement Apple's arm64 calling convention for message forwarding and `NSInvocation`, checked against clang itself with 3200 random signatures (`macapps/arm64abi.py`) and in the VM.

### bash and Homebrew

The desktop image has **GNU bash 5.3** as `/bin/bash` next to PureDarwin's zsh (`tools/bash-build.sh` builds it on the Mac: PureDarwin's libSystem lacks `___chkstk_darwin`, `strchrnul`, `iconv`, `/dev/fd` and FIFOs, so `tools/bash-clokeos.patch` runs `<(cmd)` through a temporary file, and the build script checks every imported symbol against the image's libSystem). zsh stays the default login shell; `CLOKEOS_SHELL=bash ./brand.sh` makes bash the login shell of the guest account instead (`/etc/shells`, `/etc/profile` and `/etc/bashrc` are set up for both; the console login that `launchd` starts is still zsh, run `bash -l`).

**Homebrew** works with Big Sur bottles on arm64 (tested: `brew install figlet`; Homebrew itself needs git, curl and its own Portable Ruby, nothing is compiled). One command as a normal user sets it up:

```bash
clokeos-install-homebrew     # git clone of Homebrew/brew into /opt/homebrew, downloads Portable Ruby
brew install figlet          # after opening a new login shell
```

The prefix is `/opt/homebrew` on arm64 (owned by the guest account, group `staff` may write because there is no sudo) and `/usr/local` on x86_64 (the only prefix Intel bottles are built for; **not tested**, the x86_64 desktop image was not built here). What it took:

- **macOS version.** Homebrew reads `ProductVersion` straight from `SystemVersion.plist`, so that key is now the macOS version Darwin 20.5 corresponds to (`CLOKE_MACOS_COMPAT=11.7`, Big Sur) and ClokeOS's own version lives in `ProductUserVisibleVersion` (`sw_vers -clokeosVersion`, plain `sw_vers` shows both).
- **`libcompat/`.** `libclokeos_compat.dylib` exports what Apple-SDK binaries import from libSystem and PureDarwin lacks: CommonCrypto MD5/SHA-1/SHA-384/SHA-512 and `___chkstk_darwin` (Portable Ruby), `clock_getres`, `lchmod`, `lgamma_r`, `setruid`, fenv and a few more (stubs for ucontext, libunwind and Mach exception ports), and two stubs that make `git clone https://` work (`git-remote-http` binds `SCDynamicStoreCopyProxies` flat). `libcompat/build.sh` adds it as an extra `LC_REEXPORT_DYLIB` to PureDarwin's `/usr/lib/libSystem.B.dylib` (`reexport.py`), so two-level-bound programs find the symbols; the same script wraps PureDarwin's `libffi.a` into `/usr/lib/libffi.dylib` for Ruby's fiddle.
- **Absolute paths Homebrew uses.** `/usr/bin/{curl,tar,sed,stat,...}` link to `/bin`, `/usr/bin/python`, `/private/{tmp,var,etc}`, and small shims `shasum` (openssl), `codesign` (no-op, PureDarwin does not check signatures) and `locale`.
- **Limits.** Only formulae with a Big Sur bottle work (`brew install` of anything else would build from source and stops at "No developer tools"); the sandbox is unavailable; `brew update` is slow in the VM.

### Linux programs (tux)

`tux <program>` (or `tux-run`, which starts a Linux shell without arguments) runs a Linux ELF program of the same architecture. `/TUX` is the root of the Linux process, like a chroot (`/TUX/bin`, `/TUX/lib`, `/TUX/etc`, ...); `/dev`, `/tmp`, `/Users` and `/Volumes` are the Mac's. The image ships Alpine Linux's minirootfs (musl and BusyBox, downloaded by `fetch.sh`) in `/TUX`. There is a Linux entry in the Launchpad and Start menu (a terminal running `tux-run`), and a double click on an ELF file in Fundus opens it in a terminal.

How it works (all in `tux/`): the PureDarwin kernel has no hook for foreign system calls. On arm64 an `svc` with a Linux number is not catchable (the call number lives in x16, which Linux code does not set), and on x86_64 a `syscall` with a Linux number silently does nothing. So `tux` loads the ELF file (and musl's dynamic loader) itself, builds stack and auxiliary vector like a Linux kernel, and rewrites every system call instruction in executable memory to a breakpoint; the signal handler translates the call (`tux/sys.c`: paths, `stat`, `open` flags, `ioctl` for terminals, signals, `fork`/`execve` through `tux` again). The kernel also zeroes `TPIDR_EL0` on every task switch, so the Linux thread pointer lives in a Darwin thread-local slot and the instructions that read or write it are rewritten as well.

State: BusyBox `sh`, `ls`, `cat`, `sed`, `grep`, `find`, pipes, redirects, subshells and background jobs work in the arm64 VM (HVF; about 11 ms to start a program) and in the x86_64 VM. On x86_64 the thread pointer (FS) is set through an LDT entry, which only takes 32-bit bases, so `tux` hands out Linux memory below 4 GiB there; `syscall` instructions are found with a small instruction-length decoder (a plain byte scan hits `0f 05` inside constants: 11 false matches in musl) and the PureDarwin kernel logs every trap to the console, which makes x86_64 slow under emulation.

The Linux system call layer (all checked with small musl test programs in both VMs, and glibc with Debian's coreutils on arm64):

- **Threads**: every Linux thread is a Darwin pthread (`clone` with `CLONE_THREAD`, thread pointer per thread, `set_tid_address`, `tgkill`); `futex` (wait, wake, bitset, requeue, wake-op) runs on wait lists with condition variables, so pthread mutexes, condition variables, joins, thread-local storage and `pthread_kill` work.
- **epoll, eventfd, timerfd, signalfd** on top of kqueue (an eventfd is a pipe whose reads and writes `tux` intercepts to add up the counters).
- **Sockets**: TCP, UDP and `AF_UNIX` (paths inside `/TUX` are translated, abstract names become files under `/private/tmp/.tux-abstract`), socket options, `sendmsg`/`recvmsg` with `SCM_RIGHTS`, and `SOCK_SEQPACKET`, which Darwin lacks: it runs on a stream socket with a four-byte length in front of every message.
- **`memfd_create`** as an unnamed temporary file, and **`/proc`** as a path redirection: `tux` writes `/proc/self/{maps,status,stat,statm,cmdline,environ,comm,limits,mountinfo,fd/,cwd,exe,root}`, `cpuinfo`, `meminfo`, `uptime`, `loadavg`, `version` and a few `sys/` entries into `/private/tmp/.tux-proc-<pid>/` when a path is resolved (stale directories of dead processes are swept away).
- **glibc**: Debian's `ls`, `sort` (also with `--parallel`, i.e. threads), `uname`, `id`, `date`, `seq`, `timeout` and others run through `ld-linux-aarch64.so.1` (arm64) and `ld-linux-x86-64.so.2` (x86_64) with `tux -r <debian root> <program>`. Debian is not part of the image.

Network: DNS, HTTP and HTTPS work from the Linux side (checked with BusyBox `nslookup` and `wget`); `/etc/resolv.conf` is the Mac's (via `tux`) unless the Linux tree has its own file.

Also there: **inotify** (kqueue vnode events; changes in a directory become `IN_CREATE`/`IN_DELETE` with names, a rename shows as delete plus create), **netlink** for queries only (`RTM_GETLINK`, `RTM_GETADDR`, so `getifaddrs` and `if_nameindex` work), `MSG_PEEK` on `SOCK_SEQPACKET`, `/proc/<pid>` of other processes (`stat`, `status`, `cmdline`, `comm`, `exe` from libproc), and the stacks of detached threads are freed. Debian's `sha256sum` and `df` were checked as well.

Not there yet: `io_uring` (programs fall back), netlink that changes things, packet sockets, graphical Linux programs, and on arm64 non-PIE programs (Darwin gives arm64 programs no memory below 4 GiB). Fundus does not recognise ELF files by their header (the file type database is not searched by magic), so `tux.desktop` is registered for `application/octet-stream` too and `tux-open` checks the ELF header itself: other unknown files open in a terminal that says so. Programs of the other architecture need a translator, which is a separate task.

### x86_64 programs on arm64 (ucrun)

The arm64 desktop image can run x86_64 Mac programs (Mach-O) with `ucrun`, an emulator built on [Unicorn](https://www.unicorn-engine.org/) (QEMU's CPU core, GPL-2.0) with its own Darwin system-call layer. It loads PureDarwin's x86_64 `dyld` and `libSystem` from `/Library/ClokeOS/x86` and runs them like the kernel would. A library `libxlat-exec.dylib` hooks `execve`, `posix_spawn` and friends, so `./program` in a terminal, a script, or a double click in Fundus (`xlat-open`, in a terminal window) starts an x86_64 program through `ucrun` automatically; the hook is loaded through `DYLD_INSERT_LIBRARIES` from `/etc/profile`, and interactive zsh restarts itself once at startup to pick it up. Works so far: static and dynamic programs, `fork`/`exec`/`posix_spawn`, pipes, signals, threads, mutexes, condition variables, semaphores. Not yet: `pthread_rwlock` under contention, GUI/Mach services/Objective-C frameworks, bash (only zsh gets the hook at startup), speed (about 20x slower than Rosetta 2; a JIT is the planned next step), arm64 programs on x86_64. Details, measurements and open points: [`docs/translation-study.md`](clokeos-darwin/docs/translation-study.md). It is built into the arm64 desktop image only if `~/ClokeOS-work/download/unicorn/src` (Unicorn 2.1.4 source) and the x86_64 PureDarwin image are present.

### Windows programs (Wine)

The x86_64 desktop image runs Windows programs (`.exe`, `.msi`) with **Wine 11.0**. Nothing had to be built: PureDarwin's preview image `puredarwin.img` already ships Wine as a WoW64 build (64-bit `wine`, 32-bit programs run through WoW64, so no 32-bit libraries are needed) with the X11 driver `winex11` on the guest's own Xorg, libX11, freetype and fontconfig (`/usr/lib/wine`, `/usr/share/wine`; the build logs are in `/usr/share/wine-build`). There is no `winemac` because that needs Cocoa. XNU in the image allows Wine's preloader to reserve the address space (`wine --version`, wineserver, services, explorer and wineboot start). ClokeOS adds the layout around it:

- **`/NT` is drive C:** (`/NT/Windows`, `/NT/Program Files`, `/NT/Users`), one Windows tree for the whole system, group `staff` may write. Wine fills it at the first start.
- **`/usr/bin/wine`** (`overlay-desktop/usr/bin/wine`) is a small script in front of PureDarwin's binary: the prefix is per user in `~/Library/Wine` with `drive_c` and `dosdevices/c:` pointing to `/NT`, `DISPLAY=:0`, no `winemenubuilder`, no Mono/Gecko download. `wineboot`, `winecfg`, `winefile`, ... are links to it.
- **Fundus** opens `.exe` and `.msi` with Wine by double-click. PureDarwin has no shared-mime-info, so `/share/mime/globs2`, `/share/applications/wine*.desktop`, `mimeinfo.cache` and `mimeapps.list` are written by hand (`overlay-desktop/share`).
- **"Wine"** is an app in `/Applications` (Launchpad, NT start menu): it opens Wine's file manager `winefile` on C:.
- **arm64 cannot run Windows programs**: there is no x86 translation layer yet (the arm64 image has neither `wine` nor the script).

Try it in a terminal: `wine program.exe`, `wine cmd`, `wine notepad`, `winecfg`. The first start of a user creates the prefix (`wineboot` imports `wine.inf`), which takes a few minutes in the emulated x86_64 VM and well under a minute on a PC.

### cloke: one package manager for everything

`cloke` (`/usr/bin/cloke`, Python 3, also runs on Linux and Windows) installs software from whatever source has it, on any OS:

```
$ cloke install steam
found five versions:

1. Steam-macOS (brew, cask)
2. Steam-Linux (dnf)  [via linux, not installed]
3. Steam-Linux (flatpak)  [via linux, not installed]
4. Steam-Windows (winget)  [via wine]
5. Steam-Windows (choco)  [via wine]

Choose a Version, 2 recommended (default 2):
```

Backends used when present: **brew** (macOS and ClokeOS), **dnf / apt / pacman / flatpak / AppImage** (Linux; AppImages come from the appimage.github.io feed and the project's GitHub release) and **winget / choco** (Windows). Live searches run on every backend the system has; a built-in catalog of popular programs adds the versions for the other OSes. Versions for another OS run through a compatibility layer: Windows programs through **Wine** (cloke reads the installer URL from the winget-pkgs manifest, downloads it and runs it silently under Wine), Linux programs through the command in `CLOKE_LINUX_LAYER` (no Linux layer ships yet, so those entries say "not installed"). The recommended version is a native command-line package first, then Linux, then a native GUI cask, then Windows.

`cloke search|install|remove|list|update|backends`, `-y` takes the recommendation, `--from brew|dnf|...` and `--os macos|linux|windows` narrow the list, `search --json` is machine-readable. Tests: `clokeos-darwin/tools/test_cloke.py`. Not tested yet: dnf, flatpak, winget, choco and the Wine installer path on real systems (only parsers, ranking and the prompt are covered by tests).

### Architectures

| Target | State | Base |
|---|---|---|
| x86_64 (QEMU q35, UEFI) | built, booted, released | PureDarwin 20.5, `puredarwin-minimal.img` |
| x86_64 desktop | built, booted, released (installer, USB sticks, SSD image), runs Windows programs with Wine | PureDarwin's preview image `puredarwin.img` |
| x86_64 on real PCs | tested on an HP 15s-fq5655ng: boot from USB, recovery | the same |
| arm64 (QEMU virt) | built, booted with Apple's hypervisor and emulated | `puredarwin-arm64-virt-minimal.img` |
| arm64 desktop | built, booted with HVF, released (installer) | `puredarwin-arm64-virt.img` |
| Raspberry Pi 3 | open | PureDarwin boots there to a shell but has no ready image |

The architecture is chosen with `CLOKEOS_ARCH` for all scripts (`CLOKEOS_ARCH=arm64 ./brand.sh && CLOKEOS_ARCH=arm64 ./run.sh`). On arm64 `run.sh` uses Apple's hypervisor (HVF, `-cpu host`) on a Mac with Apple silicon; `--tcg` emulates instead.

### Drivers and performance

Measured on a Mac with Apple silicon (minimal image, time from QEMU start; the loop is `while (( i < 2000000 )); do (( i++ )); done` in the VM's zsh):

| | to the prompt | 2 million loop iterations |
|---|---|---|
| arm64 with HVF | about 7 s | 2–3 s |
| arm64 emulated (TCG) | about 16 s | 18 s |
| x86_64 emulated (TCG) | about 66–71 s | 23 s |

Drivers ClokeOS uses: virtio disk and network (`IOVirtIOBlock`, `IOVirtIONet`), NVMe (`IONVMEFamily`), SATA (`RavynAHCIPort`), xHCI with USB keyboard, mouse and mass storage (`RavynXHCIPort`, `IOUSBHIDDriver`), Realtek RTL8111 and Intel 82545EM LAN, display via GOP. Missing:

- **More than one CPU core**: `kext/build.sh` builds `kernel` with PureDarwin's `PDACPIPlatform` (uACPI interpreter, reads the MADT, starts the other cores) rebuilt from source (`kext/pdacpi/`), and `kernel-up` without it (hold **U** at power-on; the key does not reach the loader on every PC, e.g. the HP). 4 cores in QEMU. On the HP 15s-fq5655ng (Alder Lake, 2 P-cores with 2 threads + 8 E-cores = 12 threads) XNU's `x86_validate_topology()` used to panic invisibly (the screen is already off): MSR 0x35 says 10 cores / 12 threads and CPUID 32 cores / 12 logical IDs per package. The driver now replaces XNU's topology values with a consistent description of the MADT's CPUs (12 CPUs, APIC IDs grouped into 6 two-thread cores) and falls back to one thread per core (10 CPUs) or one core if that does not work out. Boot-args: `cloke_trace=1` paints the progress on the framebuffer, `cloke_pause=N` waits N s, `cloke_smt=0` uses 10 CPUs, `cloke_nofit=1` leaves the MADT alone; `cpus=N` triggers the panic and should not be used.
- **Graphics acceleration.** Drawing is in software.
- **Wi-Fi, I²C touchpads, audio on most hardware.**

#### Own kernel drivers

`clokeos-darwin/kext/` holds ClokeOS' own kernel extensions; `kext/build.sh` builds them and the kernel collection:

- PureDarwin's image contains `Kernel.framework` headers matching its XNU and its own kernel collection builder `kc-builder`. That is an x86_64 program which only needs libSystem, so on a Mac with Apple silicon it runs under Rosetta. Without own drivers the result is byte-identical to the kernel in PureDarwin's EFI partition (after `kcfix.py`).
- Drivers are linked like PureDarwin's own: `MH_BUNDLE` against PureDarwin's libSystem, kernel symbols via `-undefined dynamic_lookup`, pointers as rebase/bind opcodes (`kc-builder` does not apply classic local relocations).
- **ClokePS2** takes over the PS/2 controller (i8042) instead of PureDarwin's ApplePS2Controller, which only passes keys to the text console and has no mouse driver at all. It translates scan codes (set 1) into HID usages, supports PS/2 mice and touchpads in PS/2 mode with wheel, and creates `/dev/clokeos_kbd` and `/dev/clokeos_mouse` in the format of PureDarwin's USB HID devices, which Xorg's `puredarwininput` reads (`overlay-desktop/etc/X11/xorg.conf`). Tested in QEMU with `CLOKEOS_VM_INPUT=ps2` (keys, pointer movement, clicks).
- `inputfix.py` completes the key table in `puredarwininput`: Insert, Home, Delete, End, Page Up/Down, Print, the whole keypad, the ISO key `<>|` and the menu key were missing, for USB keyboards too.

- **ClokeI2CHID** drives I²C HID touchpads on Intel LPSS (DesignWare I²C): it assigns BAR0 itself when the firmware left it empty (HP notebooks), probes the usual addresses, parses the report descriptor and polls every 10 ms; `/dev/clokeos_touchpad`. With the uACPI interpreter in `PDACPIPlatform` the touchpad's real address and GPIO interrupt could be read from the ACPI tables instead; the loader still copies the firmware's ACPI tables onto the stick (`\ClokeOS\ACPI`) for analysis.
- **ClokePayload** serves the install images that the loader staged in reserved memory as `/dev/clokeos_payload`, because PureDarwin's USB mass storage driver fails after a few dozen MB on real hardware.

### Recovery and installation

Like macOS, ClokeOS has a recovery that downloads the current system from the internet and installs it: as bootable installer image `clokeos-<arch>-installer.img` for x86_64 and arm64, as USB sticks for PCs, and on every disk set up with it as its own partition "ClokeOS Recovery".

The recovery is CloakServer in its own mode: dark, without login, Dock or desktop icons (`cloakserver/recovery.c`). A window offers "Install ClokeOS", "Get Help Online" (NetSurf with the releases page), "Disk Utility" (partition disks, see below) and "Terminal" (zsh as root); the same items are in the menu bar under "Utilities". "Install ClokeOS" walks through introduction, disk choice and confirmation, then shows progress and remaining time and restarts at the end.

Installing is done by `clokeos-installer` (`recovery/installer.c`, runs as root):

- It loads the description `clokeos-<arch>.txt` from the latest release of this repository (via `releases/latest/download/`, no login) and then three xz-compressed partition images, about 400 MB together. libcurl downloads, liblzma unpacks, and everything goes straight to the disk without a temporary file. Each image is checked against the SHA-256 in the description. Broken connections continue at the same byte with a range request (up to 10 attempts).
- The disk gets a new GPT with three partitions as on macOS: EFI partition (loader, kernel, boot arguments, 64 MB), "ClokeOS" (the full desktop system, ext4, about 4.4 GB) and "ClokeOS Recovery" (ext4, almost 600 MB). The recovery lacks Wine, compilers, Python and developer files (`recovery/imagetool.py prune`).
- It then writes the new GPT partitions' GUIDs into the boot arguments on the EFI partition: `boot-args-system.txt`, `boot-args-recovery.txt` and `boot-args.txt`, which the loader reads at every boot. `boot-uuid=` there names the GPT partition the kernel mounts as root.
- Started from the recovery of the same disk, GPT and recovery stay and only EFI partition and system are rewritten; the installer then writes to the partition devices, since the whole disk cannot be opened while its third partition is the root.

"Restart in Recovery…" in the ClokeOS menu switches `boot-args.txt` to the recovery for one boot (`clokeos-installer bootnext recovery`); the launchd service `com.clokeos.recovery-boot` switches it back right away. On x86_64 the key **R** at power-on also starts the recovery.

#### USB sticks for real PCs

`recovery/usb.sh offline <folder>` and `recovery/usb.sh online` build the two sticks. Each has a FAT32 partition with loader, kernel and boot arguments; the loader loads the recovery as a RAM disk into memory and the kernel boots from it with `rd=md0` (`clokeos_load_ramdisk` in `loader/xnu-loader.patch`).

Why a RAM disk: PureDarwin's only USB mass storage driver (`RavynXHCIMassStorageDisk` in the xHCI driver) fails with I/O errors after a while as a root disk, and USB 2 controllers have none. The firmware can read the stick, though. PureDarwin had switched this path off on x86: XNU treats loader memory as free after boot and only reaches the RAM disk through `ml_static_ptovirt`, i.e. below `physfree`, and its page tables only cover memory up to 0x3E800000 (just under 1 GB) at boot. ClokeOS' loader therefore searches the firmware's memory map for the lowest free range below that limit that fits the RAM disk (minus room for UEFI runtime services), allocates it as `EfiLoaderData`, moves the runtime window behind it and grows `ksize` over both. How much is free there depends on the firmware, not on the installed RAM: QEMU with 1 GB leaves 672 MB in one piece, an HP 15s-fq5655ng with 8 GB only 381 MB.

The loader takes the first RAM disk that fits and, if none does, shows how much it needed and how large the largest free piece is:

- `\ramdisk.img` (offline only, about 700 MB): recovery plus the images of EFI partition and system under `/Library/ClokeOS/Install`, installs from memory.
- `\ramdisk-lite.img` (about 260 MB): only the recovery, without SSH, build tools, other window managers and X demos. On the offline stick the installer then reads the images under `\ClokeOS\Install` from the stick itself (`clokeos-installer install <disk> stick:`): the whole FAT once, the files in contiguous pieces of up to 1 MB, each read retried, in 64 KB pieces after errors. The recovery detects this with `clokeos-installer stick`. The online stick downloads instead.

Started from the stick, the loader connects all firmware drivers before booting (`clokeos_connect_all`, like `connect -r` in the UEFI shell): `RavynAHCIPort` reads a SATA port's device signature before letting it receive data and sees only 0xffffffff on ports the firmware never touched. It also copies the ACPI tables onto the stick. If no keyboard or mouse is found, `clokeos-diag` shows model, PCI devices with their drivers, USB devices, network and kernel messages in a window after 45 seconds.

`recovery/disk.sh` builds the ready-made SSD image: an installed disk with boot arguments, system and recovery, exactly as the installer writes it.

Build and test:

```bash
cd clokeos-darwin
CLOKEOS_ARCH=arm64 recovery/build.sh     # recovery, images, installer into ~/ClokeOS-work/out/recovery-arm64/
recovery/release.sh                      # both architectures, packed into ~/ClokeOS-work/dist/online-<version>/
CLOKEOS_ARCH=arm64 CLOKEOS_FLAVOR=desktop ./run.sh --image ~/ClokeOS-work/out/recovery-arm64/installer.img --disk disk.img
```

`--disk` adds a second disk (created empty with 16 GB if missing). The offline stick is tested with `CLOKEOS_VM_DISK2=nvme ./run.sh --usb ~/ClokeOS-work/out/recovery-x86_64/usb-offline.img --disk ssd.img`; with `CLOKEOS_VM_MEMORY=1024` the slim RAM disk starts and reads from the stick. `CLOKEOS_VM_DISK2=ahci` attaches the target as SATA SSD, `CLOKEOS_VM_INPUT=ps2` leaves out USB keyboard and mouse like on a notebook, `CLOKEOS_VM_SSH=2222` forwards port 2222 to SSH in the VM. With `CLOKEOS_RELEASES_URL=http://10.0.2.2:8099` before `recovery/release.sh` the recovery downloads from a local web server on the Mac, so everything can be tested without publishing.

### Disk Utility and disk images

**Disk Utility** (`cloakserver/diskutil.c`, in the recovery under "Utilities", in the system under Applications > Utilities; it needs root) looks like its macOS counterpart: the sidebar lists disks with their partitions, the main area shows the layout as a bar and a list, and the toolbar has *Add Partition*, *Erase*, *Resize*, *Remove* and *Refresh*. Add asks for name, size (`2 GB`, `500 MB`) and format; the formats are those ClokeOS can write itself: FAT32 (own `mkfs`), ext4 (`mke2fs` from the e2fsprogs in `/usr/sbin`) or unformatted. Erase on a partition makes a new file system, on the whole disk a new empty GPT. Resize works for ext4 partitions (`e2fsck`, then `resize2fs`, shrinking before the table changes, growing after) and for partitions without a file system; FAT32 can't be resized in place, so erase or remove and add it again. All of it is done by `clokeos-installer layout | add | delete | format | resize` (`recovery/installer.c`), which reads and writes the GPT itself (primary and backup, CRCs, protective MBR), keeps the GUIDs of untouched partitions, aligns to 1 MiB and works on the whole-disk device with offsets, so it doesn't depend on partition nodes. While it works, mounted partitions of that disk are unmounted and `clokeos-automount` pauses (lock file `/var/run/clokeos-diskutil.lock`). A disk the system is running from can't be changed, like the startup volume on a Mac, and neither can the installer medium.

**Disk images:** double-click a `.dmg` in Fundus (or `/usr/libexec/clokeos-dmg mount file.dmg`) and its volume appears under `/Volumes/<name>`. PureDarwin has no disk image driver (no `hdiutil`, no vnode disk) and the kernel mounts HFS+ only from a block device, so `clokeos-dmg` (`dmg/dmg.c`) reads the image itself and **copies the files out, read-only**; Eject (in Disk Utility under "Disk Images", or `clokeos-dmg eject <folder>`) deletes the copy again, the `.dmg` is never changed. It reads UDIF (zlib `UDZO`, bzip2 `UDBZ`, raw/`UDRO`/`UDRW`, also flat images without the `koly` trailer) with its own inflate and bunzip2 code, finds the HFS+/HFSX volume (bare, in an Apple partition map, in a GPT, or inside an HFS wrapper), and reads its catalog, extents overflow and attributes B-trees: folders, files, symlinks, hard links, permissions and times, and files with HFS+ compression (decmpfs zlib and LZVN; LZFSE-compressed files are skipped and counted). Resource forks are not copied. APFS, ISO 9660 or FAT inside an image, LZFSE/ADC-compressed images and encrypted images are refused with a message. It checks the free space before it starts. `dmg/dmg.c` also builds on macOS (`cc dmg/dmg.c`, `CLOKE_VOLUMES` replaces `/Volumes`), which is how it is tested against images made by `hdiutil`. Fundus (Thunar) opens `.dmg` through a desktop entry and a one-line MIME rule in `overlay-desktop/share/` plus a `gio-launch-desktop` stub that PureDarwin's glib lacks.

### Releases

`recovery/release.sh` builds all images fresh, packs them with xz and writes `SHA256SUMS` and `RELEASE-NOTES.md` into `~/ClokeOS-work/dist/online-<version>/`; `--upload` publishes the release `v<version>` in this repository (only a published release counts as "latest" and is found by the recovery). Each release has a source archive of exactly the built commit with the third-party sources built along (for the GPL and LGPL parts). The images contain PureDarwin and with it GPL parts (corecrypto); the release notes point to the exact sources.

### What Apple does not publish

The kernel is the smaller problem, since XNU is open source (Apple Public Source License 2.0, `github.com/apple-oss-distributions/xnu`). Hard is everything Apple does **not** publish:

| Layer | macOS | Open source? | ClokeOS' approach |
|---|---|---|---|
| Kernel | XNU | yes | from PureDarwin, plus own drivers |
| Base userland | Libc, dyld, BSD tools | mostly | from PureDarwin (plus toybox and zsh) |
| Init | launchd | only older versions | PureDarwin's launchd/libxpc |
| Graphics stack | Quartz, WindowServer | no | own window server ("CloakServer") |
| UI | AppKit, Finder, Dock | no | rebuilt natively; AppKit for Mac apps from Darling (darling-cocotron) |
| Drivers | graphics, Wi-Fi, audio | mostly no | VMs with standard hardware first, own drivers for PCs |

### Roadmap

1. ~~**Boot Darwin in a VM.**~~ Done with PureDarwin 20.5 in QEMU.
2. ~~**Branding.**~~ Done: `SystemVersion.plist`, `sw_vers`, host name, login text, volume name and the boot logo.
3. **Window server.** CloakServer runs as an X11 window manager and compositor with menu bar, Dock, login, setup assistant and global menu bar. Next: user management, sudo, and in the long run a Wayland compositor instead of Xorg.
4. **Port the shell.** Menu bar, Dock, Launchpad and System Settings are native in CloakServer, apps are `.app` bundles, Mac apps run through Darling's AppKit. Open: Fundus, Search and the other prototype apps as native programs.
5. ~~**Installer.**~~ Done: installer, recovery, USB sticks and SSD image (see [Recovery and installation](#recovery-and-installation)).
6. **Real hardware.** Started: USB installers, SATA detection, PS/2 keyboard and touchpad (ClokePS2), I²C touchpads, several CPU cores. Next: Wi-Fi (Realtek RTL8821CE in development), graphics.
7. **Windows.** The NT interface and Wine for `.exe` programs on x86_64 exist (Windows tree in `/NT`). Next: a baked prefix so the first start is fast, more programs tested, arm64 through the translation layer.
8. **Linux.** First version done on arm64 and x86_64 (`tux`, see [Linux programs](#linux-programs-tux)); still to do: graphical Linux programs, `inotify`, programs of the other architecture.
9. **Other CPU.** Done: x86_64 programs on arm64 (`ucrun`, Unicorn-based, about 20x slower than Rosetta 2; own block JIT started) and arm64 programs on x86_64 (`a64run`), see [`docs/translation-study.md`](clokeos-darwin/docs/translation-study.md). Next: JIT for SSE/dynamic programs and threads, Windows programs on arm64 through the translated Wine, x86_64 Linux programs through `tux`.
10. **iOS apps.** Run iOS apps (arm64 Mach-O) on ClokeOS, on top of the Mac frameworks and the translation layer, with UIKit shims. Not started.
11. **Android apps.** Run Android apps through a Dalvik/ART layer on top of `tux`. Not started.

Custom images (for example for the Raspberry Pi or with a custom kernel) need PureDarwin's Nix build on Linux (about 60 GB) and Apple's `MacOSX11.3.sdk`, which you have to get yourself because it may not be redistributed.

Important: Apple's trademarks, icons and fonts must not be used. ClokeOS uses its own names, icons and logo.

### USB sticks and disks

`clokeos-automount` (CloakServer starts it at login, `cloakserver/automount.c`) looks at `/dev/disk*` every two seconds, recognizes the file system from the first sectors and mounts what the kernel can handle under `/Volumes/<label>`: FAT12/16/32 read and write (files belong to the logged-in user), HFS+ read-only. It never tries a file system blindly, because the kernel panics when HFS is mounted on a device with another file system. exFAT, NTFS, ext and ISO 9660 are recognized and logged (`/var/log/clokeos-automount.log`) but not mounted, since the kernel has no driver for them. The boot disk is skipped, EFI partitions stay hidden, a removed device is unmounted. Fundus lists `/Volumes` under Places.

## Credits

- Wallpapers: 13 artworks by [Pixel Makes Art](https://www.instagram.com/pixelmakesart) (Instagram), used with permission. Thank you!
- PureDarwin, Darwin and XNU (Apple Public Source License 2.0); Darling for the Mac frameworks; the Linux `rtw88` driver (Realtek, GPL-2.0 or BSD-3-Clause) as the reference for the Wi-Fi driver, firmware from `linux-firmware` under Realtek's redistribution licence.
- Chinese, Japanese and Korean glyphs: [Noto Sans CJK](https://github.com/notofonts/noto-cjk) (SIL Open Font License 1.1), fetched at build time.
