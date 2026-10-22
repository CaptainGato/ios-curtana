# GPU (Adreno 618) — port notes

Status: **not started**. The system draws with the software renderer
(`backboardd` + CoreAnimation on the CPU), so the UI is slow, `PosterBoard`
(SIGTRAP, needs Metal) crashes, and widgets stay empty.

The reference is `oosexgang/hackios-xnu-kernel`:
`xnu/files/bsd/dev/redwood_adreno.c` (Adreno 642L / a660, 22 documented
steps), `xnu/files/iokit/Kernel/RedwoodGPU.cpp` (the IOAccelerator the window
server draws with) and, in `hackios-xnu-userland`, Mesa/freedreno + the EAGL
bridge injected into `backboardd`/`SpringBoard`.

## Hardware (curtana, from the live FDT `backup/fdt.dtb`)

| block | address | note |
| --- | --- | --- |
| `qcom,kgsl-3d0@5000000` | `0x5000000` +0x40000 | chipid **`0x06010800`** (a618) |
| `cx_dbgc` | `0x5061000` +0x800 | |
| `cx_misc` | `0x509e000` +0x1000 | |
| `qfprom_memory` | `0x780000` +0x6300 | speed bin `0x1e9`, gaming bin `0x1f5` |
| `qcom,gpucc@5090000` | `0x5090000` +0x9000 | `qcom,atoll-gpucc` |
| `qcom,gmu@506a000` | `0x506a000` +0x31000 | + PDC `0xb290000`, `0xb490000`; IRQ 304/305 |
| `qcom,kgsl-iommu@5040000` | `0x5040000` +0x10000 | `qcom,kgsl-smmu-v2`, micro-mmu `+0x6000`, `gfx3d_user` CB `+0x48000` |
| `gpu_region` | `0x93b15000` +0x2000 | reserved |

Clocks (`clock-names`): `core_clk` (gpucc), `rbbmtimer_clk`, `mem_clk`,
`mem_iface_clk`, `gmu_clk`. Suppliers: `vddcx`, `vdd` (gfx).

Firmware (pulled from the phone's `super` → `vendor.img`, kept out of git):

```
a618_gmu.bin   32768   GMU firmware
a630_sqe.fw    32136   SQE microcode
a615_zap.mdt    6860   zap shader (b00/b01/b02, .elf)
```

## GPUCC register map (a618 == sdmmagpie/sm6150 family)

From `drivers/clk/qcom/gpucc-sdmmagpie.c`; the redwood driver's constants
match it, which is what makes the port realistic.

| | offset |
| --- | --- |
| PLL0 mode / PLL1 mode | `0x0000` / `0x0100` |
| `cxo_aon` CBCR | `0x1004` |
| GX SW reset | `0x1008` |
| **GX GDSCR** (3D core switch) | `0x100c` |
| `gx_cxo` CBCR | `0x1060` |
| `gx_gmu` CBCR | `0x1064` |
| **CX GDSCR** | `0x106c` |
| `ahb` CBCR | `0x1078` |
| `cx_apb` / `cx_snoc_dvm` | `0x1088` / `0x108c` |
| `cx_gmu` CBCR | `0x1098` |
| `cxo` CBCR | `0x109c` |
| `cx_gfx3d` / `cx_gfx3d_slv` | `0x10a4` / `0x10a8` |
| `gx_gfx3d_clk_src` RCGR | `0x101c` |
| `gmu_clk_src` RCGR | `0x1120` |
| `cb` / `hub_aon` | `0x1170` / `0x1178` |
| `hub_clk_src` RCGR | `0x117c` |
| `hub_cx_int` | `0x1204` |
| `cx_hw_ctrl` | `0x1540` |
| `gx_domain_misc` | `0x158c` |
| `hlos1_vote_gpu_smmu` | `0x5000` |

## Port plan

M1 — probe (`sweet_gpu.c`, boot-arg `sweetgpu=N`, default off): read the
QFPROM speed bin, the GCC GPU clock state, the GPUCC block. Answers "may this
kernel touch the GPU at all" (on redwood that was the first unknown: a write
the hypervisor dislikes kills the run, sometimes 17 s later).

M1b — CX on (`cx_hw_ctrl` bit 31), clocks on + measured with the GCC counter,
read GMU and SMMU ID registers.

M2 — GMU: `a618_gmu.bin` into GMU memory, HFI queues, ask for GX power,
`a630_sqe.fw`, first packet to the 3D core.

M3 — kernel service: SMMU contexts + IOAccelerator (port of `RedwoodGPU.cpp`)
so `backboardd` sees a GPU.

M4 — userland: Mesa/freedreno for a618 + EAGL bridge injected into
`backboardd`/`SpringBoard` (CoreAnimation on GLES), then the AIR/Metal path.

## Address diff redwood → curtana

| | redwood (a660) | curtana (a618) |
| --- | --- | --- |
| kgsl-3d0 | `0x3d00000` | `0x5000000` |
| GPUCC | `0x3d90000` | `0x5090000` |
| GMU | `0x3d6a000` | `0x506a000` |
| kgsl SMMU | `kgsl-smmu@3da0000` | `0x5040000` (kgsl-smmu-v2) |
| QFPROM | `0x780000` | `0x780000` |
| chipid | `0x06030500` | `0x06010800` |
| firmware | a660_* | a618_gmu / a630_sqe / a615_zap |

Keep `curtana-product.img` (SpringBoard + touch working) as the fallback image
for every GPU experiment.
