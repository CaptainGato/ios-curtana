# GPU (Adreno 618) — port notes

Status: **M1 + M1b + M2 + M3 implemented (`0093-sweet-gpu-probe.patch` + `0094-sweet-iokit-gpu.patch`).**
- `0093-sweet-gpu-probe.patch` ports the full 22-step Adreno 618 (`qcom,chipid = 0x06010800`) kernel bring-up and `hakios_gpu_*` runtime API into `bsd/dev/sweet_touch.c`:
  - **Steps 1–5 (M1 + M1b)**: Maps `kgsl-3d0` (`0x05000000`), `gbif` (`0x0500f000`), `gpucc` (`0x05090000`), `cx_misc` (`0x0509e000`), `gmu` (`0x0506a000`), `rscc` (`0x0508d000`), `pdc` (`0x0b290000`), `pdc_seq` (`0x0b490000`), `smmu` (`0x05040000`, `0x80000`), `qfprom` (`0x00780000`), `gcc` (`0x00100000`), and `cmd-db` (via `0x0c3f000c` / `0x80860000`); reads QFPROM speed/gaming bins; turns on `GPUCC_CX_GDSCR` (`0x106c`), `gmu_clk_src` (200 MHz), and GCC GPU clocks (`0x71004`, `0x7100c`, `0x71018`, `0x4452c`) with `miatoll` HWCG/FSM-aware polling; measures live GPUCC clocks via `GCC_DEBUG_MUX_GPUCC = 0x144`.
  - **Steps 6–22 (M2)**: Configures SMMU Context Bank 1 (stream 5, GMU) and Context Bank 0 (stream 0, 3D core); loads flat 32 KB `a618_gmu.bin` into GMU ITCM/DTCM (`+0x3000`); programs PDC sequencer (`0x0b490000`), PDC TCS1/TCS3 (`0x0b290000`), and RSCC (`0x0508d000`); boots GMU Cortex-M3 (`0xbabeface`); powers GX rail via legacy `BOOT_SLUMBER` OOB and `SPTPRAC`; runs HFI v1 handshake (`INIT`, `FW_VERSION`, 2-dword `PERF_TABLE`, `BW_TABLE`, `TEST`), DCVS OOB frequency vote, and `OOB_GPU_SET` (bit 16 / ack 24); programs Adreno 618 3D core registers (512 KB GMEM `0x100000..0x17ffff`, `highest_bank_bit = 14`, `ubwc_swizzle = 7`, `macrotile_mode = 0`); loads `a630_sqe.fw` and `a615_zap.mdt`/`b02` (`0x93b15000`, PAS ID 13); runs `CP_ME_INIT` + hardware blit smoke test; and exposes `hakios_gpu_*` when booted with `gpu=1` or `adreno_keep=1`.
- `0094-sweet-iokit-gpu.patch` ports `HakiosGPU` / `RedwoodGPU` and `HakiosGPUUserClient` into `iokit/Kernel/arm/SweetFramebuffer.cpp` and `SweetSurface.h`, publishing both `"HakiosGPU"` and `"RedwoodGPU"` IOKit services with MSM DRM selectors (`0x09`, `0x40..0x4c`) plus `kHakiosSurface` (`0x7e`, zero-copy `SweetSurface` import) and `kHakiosShow` (`0x7f`), and synchronizes `AppleCLCD::present` via `SweetSurfaceDrawn(s, 30)`.

## Hardware (curtana, from the live FDT `backup/fdt.dtb`)

| block | address | note |
| --- | --- | --- |
| `qcom,kgsl-3d0@5000000` | `0x5000000` +0x40000 | chipid **`0x06010800`** (a618), GBIF `@0x500f000` |
| `cx_dbgc` | `0x5061000` +0x800 | |
| `cx_misc` | `0x509e000` +0x1000 | |
| `qfprom_memory` | `0x780000` +0x7000 | speed bin `0x41e9`, gaming bin `0x41f5` |
| `qcom,gpucc@5090000` | `0x5090000` +0x9000 | `qcom,atoll-gpucc` (`GCC_DEBUG_MUX_GPUCC = 0x144`) |
| `qcom,gmu@506a000` | `0x506a000` +0x31000 | + RSCC `0x508d000`, PDC `0xb290000`, PDC seq `0xb490000` |
| `qcom,kgsl-iommu@5040000` | `0x5040000` +0x80000 | `qcom,kgsl-smmu-v2`, CB0 (3D core stream 0), CB1 (GMU stream 5) |
| `gpu_region` | `0x93b15000` +0x2000 | reserved ZAP shader region (`PAS_GPU = 13`) |

Firmware (place in `/firm/` or `/vendor/firmware/` on the root filesystem):

```
a618_gmu.bin   32768   GMU firmware (flat 32 KB ITCM+DTCM image)
a630_sqe.fw    32136   SQE microcode
a615_zap.mdt    6860   zap shader header + hashes (b00/b01/b02)
```

## Boot-args for testing on Curtana

- `sweetgpu=5` — Steps 1–5 (MMIO map, fuses, GPUCC + CX GDSC power-on, GCC GPU clocks, clock frequency measurement).
- `sweetgpu=22` — Full 22-step bring-up including GMU HFI v1, ZAP shader, CP_ME_INIT, and hardware 2D blit test.
- `gpu=1` — Runs full bring-up and keeps Adreno 618 active, registering `HakiosGPU` and `RedwoodGPU` IOKit services for userland Mesa Freedreno (`libdrm_freedreno.dylib`).
