# miatoll bring-up plan (Redmi Note 9S / 9 Pro / Pro Max / Poco M2 Pro)

Adaptation of ios-sweet (SM7150 sweet) to the Xiaomi **miatoll** family:
Redmi Note 9S (`curtana`) / Redmi Note 9 Pro global (`excalibur`) /
Redmi Note 9 Pro Max (`durandal`) / Poco M2 Pro (`gram`) — Qualcomm
**SM7125 (Snapdragon 720G)**, 2x Cortex-A76 + 6x Cortex-A55, Adreno 618.

Everything stated as fact below is verifiable from the two repos
(original ios-sweet and MakrSas/ios-topaz) or common hardware knowledge;
everything **VERIFY** is device-dependent and must be confirmed from the
live DT (`dtb/`), Xiaomi's published sm7125 kernel sources or shim probes
before it is hard-coded. No miatoll device has been tested on this
branch yet (plan written 2026-10-20).

## Why miatoll is the easiest port target

- Same SoC generation as sweet: Kryo 470 = 2x Cortex-A76 + 6x
  Cortex-A55, ARMv8.2, 16K translation granule on BOTH clusters
  (topaz's SM6225 is the odd one: its Kryo 260 Silver A53 cluster
  cannot do 16K, which forced topaz patches 0080-0082). None of those
  workarounds are needed here; the sweet patch series applies as-is in
  that respect.
- No pointer authentication (same as sweet's Kryo 470), same generic
  ARMv8.2 profile the SWEET machine config builds for.
- Same platform generation memory map: GICv3, UFS host, GENI QUP,
  DWC3 USB, SPMI/PMIC are the same families at (nearly) the same
  addresses as sweet.

Expected kernel delta: small. Most device-specific work lives in shim
constants + the ADT generator + the touch/keys drivers; the 4 topaz
CPU-feature/topology patches have no equivalent here.

## Known-good sweet setup (verified on SM7150, from NOTES/README)

| Item | sweet value |
| --- | --- |
| GICv3 | GICD `0x17a00000`, GICR `0x17a60000`, stride `0x20000`, 8 PEs |
| Timer | 19.2 MHz; vtimer INTID 19 (`GIC_PPI 3`), ptimer 18 (Qualcomm PPI numbering) |
| PSCI | 1.1 via SMC; all 7 secondaries `CPU_ON` fine, MPIDR `0x81000n00` |
| Framebuffer | ABL cont splash `0x9c000000`, 1080x2400, stride 4320, a8r8g8b8, DSC video panel |
| Touch | Goodix GT9896 @ I2C `0x5d` on QUP SE7 (`0xa84000`), rail PM6150L LDO11 via RPMh |
| Buttons | PM6150 PON KPDPWR/RESIN (SID0 `0x810`), vol up PM6150L GPIO2 (SID4 `0xc110`) |
| Shim consts | IMEM restart reason `0x146aa65c`, PS_HOLD `0x0c264000` |
| Memory | banks `0x80000000 + 2G`, `0x100000000 + 0xfcc00000`; kernel window `physBase 0xb0000000, memSize 0x4d000000` (above ramdump_fb/cdsp_sec/qseecom, below rmtfs `0xfde01000`) |

## VERIFIED from the live device (2026-10-20, curtana hwversion 1.94.0)

Data pulled over SSH/Termux (root via KernelSU): stock `boot.img` /
`dtbo` / `vbmeta` (sha-verified copies in `backup/`), the booted kernel's
FDT (`dtb/miatoll-live.dtb`, decompiled), `/proc/iomem`, `/proc/cmdline`
and dmesg.

| Item | miatoll live value | vs sweet |
| --- | --- | --- |
| boot header | **v2**, `dtb` present in boot.img (393 KB), cmdline via header | same as sweet — shim `build.sh` works unchanged |
| GICv3 | `interrupt-controller@17a00000`, GICR `0x17a60000`, stride `0x20000`, maintenance PPI 9 (`0x09`) | **identical** |
| CPUs | 8x `arm,armv8`, reg = `0x0,0x100,...,0x700` (Aff1 = n, Aff0 = 0) = **MPIDR n<<8** — exactly what shim's `CPU_ON (i<<8)` sends; capacity: cpu0-5 = A55, cpu6-7 = A76 | same MPIDR shape as sweet |
| Memory node | `<0x80000000 0x3dd00000>`, `<0xc0000000 0xc0000000>` (~4 GB total) | upper bank = clean 3 GB |
| Reserved-memory | all carve-outs <= `0x9f400000` (splash `0x9c000000+0x1700000`, ramoops `0x9d800000+0x400000`, qseecom `0x82200000+0x2200000`, mpss `0x86000000+0x8400000`, etc.) — **nothing above `0xc0000000`** | → kernel window `physBase 0xc0000000, memSize 0xc0000000` is clean |
| `/proc/iomem` System RAM | big run `c0000000-17fffffff`, lower bank fragmented (`80600000`, `80840000`, `84400000-85ffffff`, `93b17000-9dffffff`, `a1200000-bdcfffff`) | confirms the window |
| Framebuffer | ABL splash **`0x9c000000`**, 1080x2400, stride 4320 (1080*4), a8r8g8b8 (mainline dts says a8r8g8b8; `reserved-memory` cont_splash is `0x9c000000+0x1700000`) | **byte-identical to sweet (same address + same splash size!)** |
| IMEM | `qcom,msm-imem@146aa000`, restart_reason @ **`0x146aa65c`** | **identical** |
| PS_HOLD | `restart@c264000` (`pshold-base 0xc264000`) | **identical** |
| UFS host | `ufshc@1d84000` (`androidboot.boot_devices=soc/1d84000.ufshc`) | **identical** |
| serial console | `console=ttyMSM0,115200n8 earlycon=msm_geni_serial,0xa88000` | same |
| ABL state | `androidboot.verifiedbootstate=green`, vbmeta flags 0 — verified boot NOT disabled; `fastboot boot` path is unchanged by that but the first boot is the real canary |
| Touch | **FocalTech FTS over SPI** on `spi@a94000` (QUP1 SE5), compatible `"focaltech,fts"` + `"novatek,NVT-ts-spi"` (dual-IC node, this unit runs FTS per dmesg `[FTS_TS]`), rst/irq GPIO 8/9, `touch_vddio`/`touch_lab`/`touch_ibb` supplies | different from sweet (Goodix I2C) — port FTS-SPI later |
| Panel | `dsi_nt36672c_huaxing_fhd_video_display` (Novatek NT36672C, Huaxing, video mode) per `androidboot.lcmtype`; alternatives in dtbo: `ft8756_huaxing_vid`, `nt36675_tianma` | panel driver not needed while ABL splash is reused |
| Buttons | PM6150/PM6150L (mainline dts: vol-up `pm6150l_gpios 2`, vol-down `pm6150_resin`) — same PMIC pair as sweet | to be probed |
| ROM kernel | 4.14.203 (custom "ArtixV4"), permissive build | irrelevant to XNU, but eases pulls |

**Conclusion:** the sweet shim needs **zero changes** for miatoll (GIC,
IMEM, PS_HOLD, UFS, CPU_ON pattern and framebuffer all match
byte-for-byte). `tools/mkadt-miatoll.py` differs from `mkadt.py`
only in the memory window (0xc0000000+0xc0000000 via `--highmem`),
SM7125 compatible string and synthetic ECID. QEMU smoke with the
miatoll ADT passed (research/qemu-run-miatoll-adt/, no panic, Skywalk
PASSED).

## CANARY RESULTS — XNU boots on miatoll hardware (2026-10-20)

Two `fastboot boot` runs (RAM-only, no partitions were flashed) with
`shim-boot-miatoll.img` / `shim-boot-miatoll2.img`
(payload = CI kernel 39349146597 + miatoll ADT, built on this host):

**Run 1** (`cpus=1`, no ramdisk): full kernel bring-up on the real
board, ended at the root-mount wait (`Waiting on …IOProviderClass…
Apple_HFS`), then boot-picker keep looping (`Failed to write
boot-picker-bringup-reason to NVRAM` — no NVRAM node; harmless).
Confirmed on the panel: Skywalk self-tests PASSED, `SWEET FB:
AppleCLCD 1080x2400 stride 4320 at 0x9c000000, presented as 1125x2436`,
`SWEET CPU: cpu=0 entered initCPU mpidr=0x0 EL1 IRQs=1` → `came alive;
SGI1 OK`, `SWEET CPU: requested=1 selected=1 (limit=8)`,
AMFI/Sandbox/HFS registered, MAC framework, mcache/mbufinit.

**Run 2** (`cpus=1`, ramdisk = static `sweet-init` Mach-O built with
mkinit.py): the same bring-up plus:
- `Added memory device md0/rmd0 (01000000/12000000)` — RAM-disk root
  attached through the ADT "RAMDisk" memory-map entry;
- `BSD root: md0, major 1, minor 0`;
- `Load_init_program: attempting to load /sbin/launchd`;
- `*** SWEET userspace: init running as PID 1 ***` followed by
  `SWEET init: still alive` (console syscalls through devfs work);
- post-init noise `debug_log_init: gPanicBase is still not initialized`
  + `zlib memory 0x8000` — known sweet cosmetic messages.

So on SM7125, Xiaomi-ABL-with-shim → XNU (Darwin 22.6) → exec of a
statically signed arm64 binary as PID 1 all work. Android and userdata
were untouched; the phone returns to Android on a long power press.

**Run 3** (`cpus=6`, same ramdisk): all six Cortex-A55s booted via PSCI
`CPU_ON` (`target=0x100..0x500 entry=0xc148d000`, `rc=0` each), each
reports `came alive; SGI1 OK`; `SWEET CPU: 6/6 CPUs initialized, SGI1
verified`; `mcache: 6 CPU(s)`. The same RAM-disk root + PID 1 path ran
end-to-end.

**Host-side transfer lessons:** the reliable fastboot send path on
OrangePi 3 turned out to be the pair `5311000 (EHCI) + 5311400` with a
freshly started gadget: `fastboot boot … (16808 KB) OKAY [0.530s]`
(high-speed). The phone's fastboot gadget sometimes settles into FS
chirp cycling (OHCI FS descriptor read/64 errors, `-62`); repeated
replugs until the gadget fresh-restarts generally land it on EHCI HS.
The dwc3 "blue" port (5200000.usb) hosts the Android adb gadget at HS
but fails the fastboot gadget (`Cannot enable`).

**Run 4** (`cpus=8`, same ramdisk): the gold pair came up too —
`cpu=6 CPU_ON 0x600 rc=0 → came alive`, `cpu=7 CPU_ON 0x700 rc=0 →
came alive`, `SWEET CPU: 8/8 CPUs initialized, SGI1 verified`,
`mcache: 8 CPU(s)`. Full Kryo 470 (2x A76 + 6x A55) runs under XNU;
this exceeds the sweet milestone (which gated at cpus=6).

**Next:** cpus=8 (add the two A76 golds at 0x600/0x700 — legal on
SM7125, both clusters do 16K+LSE); then UFS / iOS root on userdata
(requires explicit user confirmation — userdata flash destroys data);
touch (FocalTech FTS SPI @a94000) and a battery driver port.

## miatoll delta / VERIFY list (remaining)

### 1. CPUs (shim.c, mkadt.py)

- Topology 2x A76 + 6x A55. Great secondary probe: shim CPU_ON + MPIDR
  report (like topaz commits). Compare against sweet's expected
  Aff1-numbered layout `0x81000n00` (**VERIFY** actual MPIDR pattern).
- mkadt: cpu nodes with real MPIDRs; `clock-frequency` A76 max 2.3 GHz
  (720G) vs sweet's 2.2; A55 1.8 GHz. Both clusters support 16K + LSE,
  so unlike topaz the kernel can theoretically use all of them; but
  sweet's tested path is the A55 set first (`cpus=6`) — start there,
  A76 pair only after that works.
- `cpus=1` remains the safe first-boot fallback.

### 2. Memory (mkadt PHYS_BASE/MEM_SIZE + shim pages)

- Miatoll ships in 4 GB and 6 GB variants (Poco M2 Pro also 4 GB; 9 Pro
  Max up to 8 GB). **VERIFY the memory banks and reserve carve-outs
  from the live DTB**, which the shim already reads — do not copy sweet's
  values: on 4 GB units there may be no bank at `0x100000000` at all and
  the usable kernel window must be picked from the lower bank with its
  own rmtfs/ramdump/qseecom carve-outs below it.
- Approach: derive banks from the FDT /memory node (topaz roadmap item 8
  describes exactly this: have the shim write them into the ADT and boot
  args), then keep `physBase` as a manually tuned window above all
  fixed carve-outs.

### 3. GIC + timers

- Expect sweet's bases (`0x17a00000` / `0x17a60000`) since SM7125 is the
  same platform generation, but **VERIFY** from the live DTB before
  fixing `mkadt.py` / `shim.c` constants. vtimer INTID 19 / ptimer 18 /
  19.2 MHz expected; the shim's `test_gic()` self-test confirms.

### 4. Display (mkadt FB, SweetFramebuffer)

- Panel: 6.67 IPS LCD 1080x2400, simple DSI video mode (no DSC, vs
  sweet's AMOLED DSC). **VERIFY** the ABL `cont_splash_region` base
  address, format and stride on the unit (sweet: `0x9c000000`, 4320).
- Sweet's "video mode, no kick needed" path likely applies; kernel
  console over fb (patch 0003) needs only the FB constants.

### 5. Touch — RESOLVED: FocalTech FTS over SPI (patch 0087)

- Live FDT (`status = "ok"`): `/soc/spi@a94000/touchscreen`,
  `focaltech,fts` (+ novatek alternative), QUP1 SE5 SPI at 8 MHz,
  pads TLMM `gpio53..56` (group `qupv3_se11`, function `qup15`),
  reset `gpio8`, irq `gpio9`, rails pm6150_l18 / lcdb_ldo / lcdb_ncp.
  The `i2c@a84000` candidates (synaptics_tcm@20, atmel_mxt_ts@4a) are
  `status = "disabled"` — the earlier TCM probe on SE7 could never work.
- `xnu/patches/0087-sweet-touch-fts-spi.patch`: bit-banged mode-0 SPI on
  the TLMM pads (no GENI firmware/GCC/DMA), vendor `focaltech_spi.c`
  framing and CRC, app id `0xa3/0x9f`, report register `0x01`, feeding the
  existing `sweet_hid_touch()`. Enabled with boot arg `sweetspi=1`
  (2..4 = alternative pad orders; 1 = MOSI/MISO/CLK/CS on gpio 53..56).
- `shim/out/touchprobe-curtana.img` (built with `EXTRA_CFLAGS=-DTOUCH_PROBE=1`)
  prints pad levels, IRQ-after-reset latency and the result of all four
  pad orders on the panel before resetting to Android: one boot tells us
  pin order and whether the rail is already on.
- If the IC never answers with the rail off, the follow-up is an RPMh vote
  for `ldoa18` (generalise `sweet_rpmh.c`, which resolves names in cmd-db).

### 6. Buttons / PMIC (sweet_keys.c constants)

- Likely a PM6125-class PMIC instead of PM6150 (+L): **VERIFY** PON
  block address, SPMI SIDs and the volume-up GPIO from the downstream
  dts; adjust the two constants and the RPMh rail names if any.

### 7. Shim / boot image packing

- Sweet packs the shim as Android boot **v2**. topaz had to move to
  **v4** because the newer Xiaomi ABL there drops the v2-style ramdisk;
  unknown for the miatoll ABL (Android 10 era): canary test = boot the
  untouched sweet shim image with `fastboot boot`. If the payload is
  never reached, port topaz's v4 packing (`shim/build.sh`,
  `shim/loader.c`) into this branch.
- **VERIFY** IMEM restart reason and PS_HOLD addresses for sm7125 from
  the downstream dts / board files (topaz: `0x0c12565c`, `0x0440b000`;
  sweet: `0x146aa65c`, `0x0c264000`).

### 8. Rootfs / userland

- Unchanged from sweet: iPhone X (iPhone10,3 / D22AP) iOS 16.7.16
  userland, `/product` from `DeviceTree.d22ap.im4p`; `tools/mkiosroot.sh`
  and `sweet-root-tweaks.py` as-is. Same dark caveats (no GPU, no audio,
  security model off).

## Canary test sequence (Android intact, all `fastboot boot`, nothing flashed)

1. Boot the UNMODIFIED sweet shim image (`cpus=1, sweet ADT`) on the
   miatoll. Outcomes:
   - shim banner prints (screen red, MMFR0/MIDR lines) -> ABL hands the
     ramdisk through; record ID_AA64MMFR0 (16K granule) + secondary
     MPIDRs from the self-test; go to step 2.
   - no shim output -> bootloader-packing problem; port topaz's v4
     flow first.
2. miatoll ADT (`dram-base`/`dram-size` from the live FDT measured in
   step 1, FB address from `cont_splash_region`, GIC from live DTB) +
   `cpus=1`: first XNU fbcon output on the panel.
3. `cpus=6` (A55 set, exactly like the sweet milestone boots).
4. UFS/HFS+ root (`rd=ufsroot0` with the sweet image; miatoll UFS is
   the same host generation; 9S/9 Pro global and Poco M2 Pro are UFS
   2.1; **VERIFY** per-unit eMMC/UFS possibility).

## Roadmap

1. Canary boot (step 1) + shim self-test values into this doc.
2. miatoll `mkadt.py` variant + fbcon up with `cpus=1`.
3. UFS -> launchd on the sweet root image.
4. Display -> IOSurface/IOMFB -> SpringBoard.
5. Touch + buttons (the two new device patches).
6. Battery, USB net (0058-0077 are already upstream in this series),
   then consider the modem/WLAN path — the topaz branch has a working
   modem firmware load + WLAN patch chain for the topaz WCN3988 (see
   MakrSas/ios-topaz patches 0084+), and the miatoll modem/WLAN is the
   same WCN39xx class — strong precedent for an IONetworkingFamily
   route.

## SAFETY — flash contract (HARD RULES, never violate)

The ONLY partition that may ever be flashed on this miatoll is
`userdata` (`/dev/block/sda18`, 48441929728 bytes ≈ 45.1 GiB). That
flash destroys the Android system and user data on that partition
(photos/videos are backed up to the PC out-of-repo first), and it is
the one deliberate, user-confirmed step of the UFS bring-up.

NEVER flash ANY of these, in fastboot, recovery or dd, no exceptions:

- `modemst1`, `modemst2`, `fsg` — IMEI / modem NV, bricking-level data.
- `persist`, `persistbak`, `dsp` — sensors calibration, DSP firmware.
- `abl`, `xbl`, `xbl_config`, `abl_b/xbl_b` — bootloader chain.
- `tz`, `hyp`, `rpm`, `pmic`, `cmnlib*`, `keymaster*`, `qupfw`,
  `devcfg`, `imagefv`, `uefisecapp`, `multiimg*` — firmware chain.
- `boot`, `boot_b`, `dtbo`, `vbmeta`, `misc`, `devinfo`, `ach`
  — Android boot/AVB state (kept stock so recovery/Android return
  stays possible).
- logical partitions (`system`, `vendor`, `product`, `odm`, `oem_*`
  super-block layout) — untouched.

Safe-by-design operations:

- `fastboot boot <img>` is RAM-only: it executes a kernel+payload
  from memory and writes NOTHING to flash. All canary/development
  boots use only this. Android is restored by a long power press
  (hardware reboot) afterwards.
- Early-boot XNU on this port does not write storage until launchd
  mounts the root; the ramdisk/PID-1 canary runs are therefore read
  only w.r.t. UFS.

Recovery path back to Android (documented, requires doing things to
`userdata` ONLY): OrangeFox recovery → Format Data (wipes userdata,
recreates the F2FS/ext4 layout) → reflash stock `boot/dtbo/vbmeta`
images from `backup/` (sha-verified copies kept on the PC) if their
validation is ever in doubt; nothing else ever needs flashing.

Current phase progress: iOS rootfs build from the iPhone10,3
iOS 16.7.16 (20H392) IPSW via `tools/mkiosroot.sh`; the resulting
HFS+ image (16G logical, sparse on the host) is converted with
`img2simg` and will be flashed with the single command
`fastboot flash userdata ios16-root.simg` after explicit user
confirmation. The same `fastboot boot` path then launches XNU
with `rd=ufsroot0` against the real root filesystem.

## Workflow

As `AGENTS.md`: CI kernels on GitHub macOS runners; fb console and
persistent log for device work; log pulls via recovery; every
destructive action (userdata flash) needs explicit user confirmation;
device dumps, serials and Apple artifacts stay out of git.
