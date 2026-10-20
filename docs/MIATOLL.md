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

## miatoll delta / VERIFY list

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

### 5. Touch (new driver patch, style of topaz's 0083)

- Miatoll units ship different touch ICs by variant/revision; downstream
  Xiaomi sm7125 dts shows the concrete candidates (**VERIFY** per unit:
  `dmesg | grep -i -e ts -e touch` under Android or the downstream dts).
  Candidates in the Goodix / Synaptics / focaltech families.
- Plan: keep sweet's input stack (patches 0060/0062/0063: HID service +
  RPMh rail vote + GENI I2C protocol), replace only the IC driver behind
  `sweet_hid_touch()`.

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

## Workflow

As `AGENTS.md`: CI kernels on GitHub macOS runners; fb console and
persistent log for device work; log pulls via recovery; every
destructive action (userdata flash) needs explicit user confirmation;
device dumps, serials and Apple artifacts stay out of git.
