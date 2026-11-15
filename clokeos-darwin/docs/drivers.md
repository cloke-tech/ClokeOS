# Drivers on ClokeOS

How ClokeOS talks to hardware today, how Linux, macOS and Windows reach the same hardware, the plan for
real graphics on the HP 15s-fq5655ng (Alder Lake, Iris Xe), and a reusable way to bring Linux driver
knowledge into IOKit kexts.

Status: November 2026, branch `feature/drivers`.

---

## 1. Inventory: how ClokeOS reaches hardware today

ClokeOS runs on PureDarwin 20.5: XNU 7195 (`RELEASE_X86_64` / arm64 virt), IOKit, kexts linked into
one kernel collection (KC). There is no `kextd`/`kmutil` at runtime; every driver is in the KC that
the loader reads from `\EFI\BOOT\kernel`.

### Boot path

| Step | What happens | Where |
|---|---|---|
| UEFI firmware | Initialises the iGPU itself (Intel GOP driver in the BIOS), sets the panel's native mode, gives a linear framebuffer | firmware |
| Loader | PureDarwin's xnu-loader with ClokeOS patches: boot logo, keys V/R/D/U, RAM disk for the stick, ACPI dump, **new: GOP report and `cloke_gop=`** | `loader/` |
| `boot_args` | Framebuffer handed to XNU as `Video` (64-bit base, width, height, row bytes, depth 24 without `-v`) | `loader/xnu-loader.patch` |
| Kernel | `PE_state.video` → console, `IOGOPFramebuffer` | KC |
| Xorg | `puredarwingop` driver maps the framebuffer through `IOGOPFramebuffer` (one fixed mode, shadow buffer + damage blits) | PD userland |

### Kernel collection

`kext/build.sh` takes PureDarwin's kexts and `kc-builder` out of the desktop image (debugfs, no boot),
compiles ClokeOS' own kexts and rebuilds the KC. Rules learnt the hard way (see comments in the script):
MH_BUNDLE linked against PD's libSystem with `-undefined dynamic_lookup`, `-no_fixup_chains`, no XML
comments in `Info.plist`, no stack frames ≥ 4 KB (`___chkstk_darwin` does not exist in the kernel).
`kcfix.py` patches PureDarwin's IOGOPFramebuffer console bug (base with bit 0 set) in the result.

Drivers in the x86_64 KC:

| Area | Driver | Origin | Notes |
|---|---|---|---|
| Display | `IOGOPFramebuffer` | PureDarwin | Matches IOPCIDevice named `display` (score 2000) or a `framebuffer` platform nub; one mode, from `boot_args`; publishes `/dev/fb0` |
| Display | `IOIntelFramebuffer` | PureDarwin, based on [lil](https://github.com/managarm/lil) (BSD-2) | Real modesetting for Ivy Bridge, Gemini Lake, Kaby/Coffee Lake only (IOPCIPrimaryMatch list, score 6000). **Does not match Alder Lake.** Boot-arg `gen9_debug` |
| Display | `IOVirtIOGPU` | PureDarwin | QEMU |
| Display | **`ClokeIntelGfx`** | **ClokeOS (new)** | Intel Gen11–13, next to IOGOPFramebuffer (own `IOMatchCategory`), see section 3 |
| ACPI / SMP | `PDACPIPlatform` (uACPI) | PureDarwin, rebuilt with ClokeOS patch | `kext/pdacpi`: hybrid-CPU topology fix, `ClokeTrace` |
| Input | `ClokePS2`, `ClokeI2CHID` | ClokeOS | PS/2 keyboard; I2C-HID touchpad on Intel LPSS (DesignWare), polled |
| Wi-Fi | `ClokeRtw88` | ClokeOS, after Linux rtw88 | RTL8821CE/8822CE: power-on, firmware, H2C; no data path yet |
| Audio | `RavynHDAudio` | ravynOS, rebuilt with amp patch | HDA by class code |
| Network | `PDRealtek8111`, `PDE1000`, `IOVirtIONet` | PureDarwin (8111 patched) | |
| Storage | `IONVMEFamily`, `RavynAHCIPort`, `RavynXHCIPort` | PureDarwin / ravynOS | |
| Install | `ClokePayload` | ClokeOS | serves loader-placed images as `/dev/clokeos_payload` |

### PureDarwin quirks every driver hits

* **No IODeviceMemory for BARs**: `mapDeviceMemoryWithIndex` returns 0. Drivers read the BAR from config
  space and map the physical range themselves (`IOMemoryDescriptor::withPhysicalAddress`).
* **No IOPCIFamily headers** in `Kernel.framework`: kexts declare the few non-virtual
  `extendedConfigRead/Write*` methods themselves; kc-builder binds them.
* **Firmware may leave BARs unassigned** (HP: LPSS I2C BAR0) – the driver must place them
  (`ClokeI2CHID`), below 4 GB hangs the HP.
* **DMA memory**: physically contiguous below 4 GB only one page (4 KB) at a time.
* **No ACPI interpreter in the default single-core KC**; the SMP kernel has uACPI (PDACPIPlatform) but it
  does not export methods to other drivers yet → GPIO interrupts, `_BCM`/`_DSM` unreachable; drivers poll.
* **Interrupts**: no ClokeOS driver uses MSI/INTx yet (all poll). IOPCIFamily lists
  `IOPCIMessagedInterruptController`, so `IOInterruptEventSource` on index 0 should work – unverified.
* Everything a driver prints goes to `dmesg`; `clokeos-diag` (stick boot) copies it to
  `\ClokeOS\diag.txt` on the stick every 30 s – the main way to get results from real hardware.

---

## 2. Comparison: Linux, macOS, Windows

The hardware is the same; the operating systems differ in who finds it, who owns the resources and
how the driver is split.

| | Linux | macOS (IOKit) | Windows (WDDM / KMDF) | ClokeOS today |
|---|---|---|---|---|
| Discovery | PCI core enumerates, `pci_driver.id_table` | IOPCIFamily publishes `IOPCIDevice` nubs, matching by `IOPCIMatch`/class/name in `Info.plist` | PnP manager, INF hardware IDs | as macOS (PD IOPCIFamily) |
| BARs / MMIO | `pci_resource_start`, `pci_iomap`, `ioremap` | `IOPCIDevice::mapDeviceMemoryWithRegister` / `getDeviceMemoryWithIndex` | `CM_RESOURCE_LIST` in `EvtDevicePrepareHardware`, `MmMapIoSpaceEx` | read BAR from config space, map physical range by hand |
| Interrupts | `pci_alloc_irq_vectors` (MSI/MSI-X/INTx), `request_irq`, threaded IRQs | `IOInterruptEventSource` / `IOFilterInterruptEventSource` on a provider index, work loop | `WdfInterruptCreate`, ISR + DPC; MSI via INF | polling timers; `lkpi` wraps `IOInterruptEventSource` (unverified) |
| ACPI | full AML interpreter (ACPICA); `_DSM`, `_BCM`, GPIO via `pinctrl` | AppleACPIPlatform, ACPI nubs, `evaluateObject` | ACPI.sys; ACPI-enumerated devices | uACPI only in the SMP kernel; drivers read the tables the loader dumped |
| DMA / IOMMU | DMA API (`dma_alloc_coherent`, `dma_map_sg`), IOMMU (VT-d) transparent | `IODMACommand`, `IOMapper` (DART/VT-d) | DMA adapter, `AllocateCommonBuffer`, IOMMU via DMA remapping | single contiguous pages below 4 GB, no IOMMU (VT-d left off by the firmware) |
| Firmware blobs | `request_firmware` from `/lib/firmware` (linux-firmware) | inside the kext bundle / embedded | in the driver package (INF `CopyFiles`) | compiled into the kext as a C array (`kext/build.sh`, rtw88) |
| Power | runtime PM, `pci_set_power_state` | IOPM power states on the service | PoFx | D0 by hand in `start` |

### Graphics specifically (Intel iGPU)

| | Linux `i915` / `xe` | macOS (Apple Intel kexts) | Windows (Intel DCH) |
|---|---|---|---|
| Split | One kernel module: display (KMS: CRTC/plane/encoder/connector), GT (engines, GuC/HuC firmware, memory), userspace via DRM ioctls; Mesa `iris` (GL) and `anv` (Vulkan) build command buffers | `AppleIntelFramebuffer<gen>` (display, IOFramebuffer subclass) + `AppleIntel<gen>Graphics` (IOAccelerator, GT) + user-space GL/Metal drivers | WDDM: `igdkmd64.sys` (KMD: display + scheduling via DxgKrnl) + UMDs for D3D/GL/Vulkan |
| Takeover from firmware | `fastboot`: reads out the GOP mode (pipe, plane, DPLL, ports) and keeps it if it fits, else full modeset | Framebuffer kext takes over early, re-programs from its own connector tables | Disables VGA, full modeset via VBT |
| Mode data | EDID via DDC/AUX, panel data + backlight data from the VBT (in the Option ROM / ACPI OpRegion) | built-in platform tables + EDID | VBT + EDID |
| Backlight | PWM in the PCH (`BXT_BLC_PWM_*` on CNP and newer) or eDP AUX (DPCD 0x720…), ACPI `_BCM` as fallback | AppleBacklight + framebuffer PWM | DxgKrnl brightness DDI, PWM or AUX |
| Firmware blobs | DMC (display power states), GuC, HuC from linux-firmware | in the kexts | in the driver package |
| Licence | i915 and xe: **MIT** | proprietary (only the binaries, not reusable) | proprietary |

Consequences for ClokeOS:

* **i915/xe are MIT-licensed**, so ClokeOS may port code, not only knowledge, as long as the copyright
  notices stay. The Intel PRMs (public) describe the registers.
* Apple's kexts cannot run on ClokeOS (they need IOAccelerator, Metal and WindowServer interfaces PureDarwin
  does not have) and are not ported; macOS is the model only for *how the driver plugs into IOKit*
  (IOFramebuffer subclass for display, IOAccelerator for GT).
* Windows drivers are not usable (WDDM is a different kernel model); the INF files are a useful list of
  device IDs only.

---

## 3. Graphics on the HP 15s-fq5655ng

Hardware: Intel Core i5-1235U (Alder Lake-P), Iris Xe (8086:46a8, display version 13, 80 EU),
15.6" 1920×1080 eDP panel, PCH backlight.

The goal is a **real graphics driver**, not a better framebuffer: ClokeOS programs the display engine
itself (modes, planes, backlight, outputs) and later uses the GPU's engines. GOP stays the fallback that
keeps the screen working while the driver grows.

### Stage 1 – stable framebuffer in native resolution (GOP) – done

* The firmware's GOP already sets the panel's native mode; the loader keeps it.
* New boot-arg **`cloke_gop=max`** (largest linear mode) or **`cloke_gop=WxH`** (exact mode) in
  `\EFI\BOOT\boot-args.txt`, set before the boot logo is drawn (`loader/gop.c`).
* On a stick boot the loader writes **`\ClokeOS\gop.txt`** (all GOP modes, current mode, framebuffer
  address and size) and **`\ClokeOS\edid.bin`** (panel EDID from `EFI_EDID_ACTIVE/DISCOVERED`, with the
  native mode decoded from the first detailed timing).
* Tested in QEMU: `cloke_gop=max` → `SetMode(27) -> 2048x2048`, report written.

### Stage 2 – own display driver `ClokeIntelGfx` – in progress

`kext/ClokeIntelGfx` matches every display controller by PCI class (non-Intel ones are refused in `start`) and attaches next to IOGOPFramebuffer (separate
`IOMatchCategory`, so the GOP framebuffer keeps working). Built on `kext/include/cloke_lkpi.h`.

What it does now:

1. **State readout** like i915's fastboot (`igfx_regs.h`, registers from the Intel PRMs and the i915
   headers): per pipe transcoder timings, measured refresh rate (`PIPE_FRMCOUNT`), pixel clock, source
   size, pipe scaler, plane 1 (size, stride, tiling, GGTT surface), DDI port, link mode and bpc; DDI
   buffers; panel power (`PP_STATUS`/`PP_CONTROL`); PWM backlight controllers.
2. **Brightness**: PCH PWM (`BLC_PWM_CTL/FREQ/DUTY`, Cannon Point and newer), or eDP AUX brightness
   (DPCD `0x721–0x723`) when no PWM controller is enabled. 5–100 %.
3. **AUX channel** (native DPCD read/write over `DP_AUX_CH_CTL` A) – the base for EDID, link training and
   PSR later.
4. **Own scanout** (`scanout-test`, boot-arg `cloke_gfx=2`): allocates its own framebuffer in system
   memory (4 KB pages), maps it into a free range at the end of the GGTT, points plane 1 at it, verifies
   with `PLANE_SURFLIVE` that the display engine really reads it, shows colour bars for 3 s, then restores
   GOP's surface and the GGTT entries. This is the first step that no framebuffer driver does.
5. `/dev/clokeos_gfx`: read = report; write = `brightness N|+N|-N`, `dpcd`, `rescan`, `scanout-test`.
   `clokeos-brightness` is the user-facing command. IORegistry properties `ClokeGfx Mode`,
   `ClokeGfx Backlight`, `ClokeGfx Platform`.
6. `clokeos-diag` prints the report (and the panel's eDP DPCD capabilities once per boot), so it lands in
   `\ClokeOS\diag.txt` on the stick.

Boot-arg `cloke_gfx=0` turns the driver off, `1` (default) = readout + brightness, `2` = also the
scanout test at start.

Host test: `kext/ClokeIntelGfx/test/igfx_test.cpp` checks decoding against a register set as an Alder
Lake laptop's firmware leaves it (`c++ -std=c++17 -I.. igfx_test.cpp`).

### Roadmap to a real driver

| Step | Content | Model |
|---|---|---|
| 2a ✔ (tested on the HP) | Readout, backlight, AUX, own scanout via GGTT | i915 `intel_display` readout, `intel_backlight.c` (cnp), `intel_dp_aux.c` |
| 2b | **ClokeIntelGfx becomes the IOFramebuffer** (subclass, higher score than IOGOPFramebuffer, falls back to it on failure): framebuffer in own GGTT memory, hardware cursor plane (cursor A `CUR_CTL`), page flips on vblank, `/dev/fb0` compatible so `puredarwingop` keeps working | Apple's IOFramebuffer model, PD `IOGOPFramebuffer` |
| 2c | **Mode list and resolution change at runtime**: EDID over AUX-I2C, VBT from the ACPI OpRegion (`ASLS` in PCI config 0xFC), scaled modes through the pipe scaler (`PS_CTRL`, no link retraining needed on eDP), Xorg driver that reads modes from the kext (RandR) | i915 `intel_panel.c`, `skl_scaler.c`; lil |
| 2d | **Full modeset**: disable pipe/transcoder/DDI, combo-PHY DPLL programming, DP link training, panel power sequencing; external outputs (HDMI/DP over Type-C needs TC-PHY + PMC handshake) | i915 `intel_ddi.c`, `intel_dpll_mgr.c`, `intel_dp_link_training.c`; lil's Kaby Lake code (`IOIntelFramebuffer`) for the IOKit shape |
| 2e | Display power: DMC firmware (`adlp_dmc.bin`, linux-firmware), DC states, PSR – battery life | i915 `intel_dmc.c` |
| 3a | **GT bring-up**: forcewake, GT power wells, GuC/HuC firmware load (`adlp_guc_70.bin`, `tgl_huc.bin`), engines (render, blitter, video) with execlists/GuC submission; first blitter fill/copy → 2D acceleration for CloakServer (damage blits, scrolling) | i915 `gt/` (MIT) |
| 3b | GPU memory: per-process GTT (PPGTT), buffer objects, user client (IOUserClient) API close to the DRM ioctls (`GEM_CREATE`, `EXECBUFFER2`, `WAIT`) | i915 `gem/` |
| 3c | 3D: port Mesa's `iris`/`anv` on top of that API (Mesa is MIT; it only needs the ioctl layer and a winsys) → OpenGL/Vulkan, then `glamor` in Xorg | Mesa |
| 3d | Media engine (HuC/VDBOX) for video decode | Mesa/intel-media |

Each step can be tested on the HP from the stick with the result in `diag.txt`, without SSH.

### What actually ran on the laptop

Two stick tests on 2026-11-15 (2.8 live stick with the new loader, kernel and ramdisk tools; results read from
`\ClokeOS\gop.txt`, `edid.bin` and `diag.txt`):

| What | Result on the HP 15s-fq5655ng |
|---|---|
| GOP | 5 modes (1920×1080, 640×480, 800×600, 1024×768, 1280×1024), all BGRX; firmware leaves mode 0 = **1920×1080 native**; framebuffer at **0x4000000000** (above 4 GB, BAR2 aperture), 0x7e9000 bytes |
| EDID | panel vendor `CMN` (0x0dae), product 0x153b; native 1920×1080, 152.82 MHz (60 Hz); a second detailed timing at 101.89 MHz (40 Hz, low refresh) |
| Driver match | `ClokeIntelGfx` attaches next to `IOGOPFramebuffer` on `display@2` 8086:46a8, `/dev/clokeos_gfx` present |
| Readout | pipe A 1920×1080, total 2250×1132, eDP on DDI A in DP mode at **6 bpc**, plane 1 linear, stride 7680, GGTT surface 0; pipes B–D off; PSR and PSR2 off; panel power and backlight enable on |
| Refresh | measured 57.7 Hz on both boots (pixel clock ~147 MHz), EDID says 60 Hz – open: either GOP programs a lower clock or PureDarwin's time base is ~4 % off; to be checked against the DPLL registers |
| Backlight | PCH PWM controller 0, frequency value 96000; **`clokeos-brightness 30` / `100` works** |
| eDP DPCD | DPCD 1.1, 2.7 Gbit/s, 2 lanes, enhanced framing; eDP caps `0x701 = 0` → no AUX brightness, PWM is the right path |
| GGTT | GMCH_CTRL 0xfec1 → 4 GB GGTT; unused entries are firmware leftovers (0x3fd23fd23fd23fd2), not a scratch page – the first test refused that range, the second one does not |
| **Own scanout** | **works**: 8 MB of system memory (first page at 0x2a2050000) mapped at GGTT 0xfc000000, plane 1 switched, `PLANE_SURFLIVE` = 0xfc000000, colour bars visible for 3 s, then back to the GOP surface |
| Keyboard | arrow keys did not work: CloakServer loaded the XKB keymap with `xfree86` key codes while `puredarwininput` sends evdev codes; fixed (`evdev+aliases(qwerty)`, `inet(evdev)`), arrows work on the HP |

So on this laptop ClokeOS now drives the display engine itself for scanout and backlight. Next is step 2b:
`ClokeIntelGfx` becomes the IOFramebuffer with its own framebuffer in the GGTT.

---

## 4. A reusable way: Linux driver knowledge on IOKit

### `kext/include/cloke_lkpi.h`

A header-only Linux kernel API subset for ClokeOS kexts, in the spirit of FreeBSD's LinuxKPI but much
smaller: only what a driver needs to start. Everything is `static inline`, so every kext links only what it
uses and no two kexts share symbols.

| Linux | cloke_lkpi | Backed by |
|---|---|---|
| `u8…u64`, `BIT`, `GENMASK`, `FIELD_GET/PREP`, `DIV_ROUND_*` | same names | – |
| `dev_info/warn/err/dbg`, `WARN_ON` | same | `IOLog` |
| `udelay`, `msleep`, `usleep_range`, `ktime_get_ns`, i915's `wait_for(COND, ms)` | same | `IODelay`, `IOSleep`, `mach_absolute_time` |
| `readl/writel/readq/…`, `posting_read` | same | volatile access + compiler barrier |
| `struct pci_dev`, `pci_read/write_config_*`, `pci_find_capability`, `pci_resource_start`, `pci_enable_device_mem`, `pci_set_master` | same, `lkpi_pci_init` fills it from config space | `IOPCIDevice::extendedConfig*` |
| `pci_iomap_range`, `ioremap` | `pci_iomap_range(..., &map)`, `lkpi_ioremap` | `IOMemoryDescriptor::withPhysicalAddress` + `map(kIOMapInhibitCache)` (PD has no IODeviceMemory) |
| `kzalloc/kmalloc/kcalloc/kfree` | same | `IOMallocZero` with a size header |
| `dma_alloc_coherent` | same (+ owner struct), refuses > 4 KB | `IOBufferMemoryDescriptor::inTaskWithPhysicalMask` below 4 GB |
| `request_firmware` | `request_firmware_from(table)` | firmware compiled into the kext |
| `request_irq`, `free_irq` | same (+ owner struct); `lkpi_poll_start` as polling fallback | `IOInterruptEventSource` / `IOTimerEventSource` on a work loop |
| `mutex`, `spinlock_t` | same | `IOLock`, `IOSimpleLock` |
| module params | `lkpi_bootarg("cloke_<name>", default)` | `PE_parse_boot_argn` |

### Recipe for a new driver

1. **Find the Linux driver and its licence.** MIT/BSD/dual-licensed (i915, xe, amdgpu, rtw88, iwlwifi,
   ath, SOF parts): code may be ported with its notices. GPL-only: use it as documentation, write own code.
   Prefer the vendor's public manuals (Intel PRMs, Realtek is not public → rtw88 is the reference).
2. **Collect facts from the real machine first**: PCI IDs and BARs (`clokeos-diag`), ACPI tables
   (`\ClokeOS\ACPI` from the loader), firmware state. Start every driver with a read-only stage that logs
   what the firmware left behind.
3. **Create `kext/<Name>/`** with `Info.plist` (no XML comments; `IOPCIMatch`/`IOPCIClassMatch`;
   own `IOMatchCategory` if another driver must stay attached) and `.cpp` files; include `cloke_lkpi.h`.
   `kext/build.sh` picks it up automatically; firmware blobs go in like `rtw8821c_fw.h`.
4. **Keep hardware knowledge in plain headers** (registers, decoders) without kernel dependencies and test
   them on the Mac (`test/`), as `igfx_regs.h` does.
5. **Gate every stage with a boot-arg** (`cloke_<name>=0/1/2/…`) so a hang on real hardware can be bisected
   by editing `boot-args.txt` on the stick, without a new build.
6. **Report through `dmesg` and a `/dev/clokeos_<name>` node**; add a line to `clokeos-diag` so the result
   lands in `\ClokeOS\diag.txt`.
7. **Test on the stick**: `kext/build.sh`, put `out/kernel` on a copy of the live stick (as below), boot,
   read `diag.txt` on the Mac.

### How the other drivers map onto it

| Driver | Linux reference (licence) | What the layer gives | What is still missing |
|---|---|---|---|
| Wi-Fi RTL8821CE/8822CE (`ClokeRtw88`) | rtw88 (dual BSD/GPL) | PCI, MMIO, DMA pages, firmware table | migrate `ClokeRtw88` to `cloke_lkpi.h`; RX/TX rings, 802.11 stack (mac80211 equivalent) and WPA supplicant in userland; IONetworkingFamily glue |
| Intel Wi-Fi (AX201/AX211 in other laptops) | iwlwifi (dual BSD/GPL) | same | large firmware, multi-page DMA → needs scatter-gather (`IODMACommand`) in the layer |
| Audio (HDA codecs; Alder Lake laptops often use SOF DSP with DMIC) | snd-hda (GPL), SOF (BSD/GPL) | PCI, IRQ, DMA | HDA works via RavynHDAudio; SOF needs DSP firmware + topology files and ACPI NHLT (the HP has an NHLT table) |
| Touchpad I2C-HID (`ClokeI2CHID`) | i2c-designware, i2c-hid (GPL) | PCI, MMIO | ACPI GPIO interrupt through uACPI instead of polling |
| NVMe | nvme (GPL) / PD's IONVMEFamily | – | works; layer only for fixes |
| GPU (this document) | i915/xe (MIT) | everything above | see roadmap |

Layer extensions that the next drivers will need: scatter-gather DMA (`dma_map_sg` over
`IODMACommand`), MSI-X vector selection, a `uACPI` bridge so drivers can evaluate `_DSM`/`_BCM`/`_CRS`
(GPIO interrupts), and a work queue (`schedule_work`, `delayed_work`) on `IOWorkLoop`.

---

## 5. Testing on the HP

* Base: the 2.8 live stick (`~/Desktop/ClokeOS-live-x86_64.img`), copied, with the new loader, the new
  kernel (`kext/build.sh`) and the ramdisk's `clokeos-diag` / `clokeos-brightness` replaced via debugfs.
* Boot it, wait about two minutes on the desktop, shut down, plug the stick into the Mac and read
  `ClokeOS/gop.txt`, `ClokeOS/edid.bin`, `ClokeOS/diag.txt`.
* Never write an image to a drive without asking; the user writes the stick (balenaEtcher or `dd`).
