# sde-split-hijack

Make a single wide plane drive both layer mixers of a dual-DSI panel — without
replacing `msm_drm`.

Xiaomi Pad 8 Pro (`piano`, 25091RP04C), Qualcomm SM8750, stock GKI 6.6.118.

---

## The problem

The panel is 3200×2136 but one layer mixer tops out at 2560
(`qcom,sde-mixer-linewidth`), so the driver needs two LMs.

The vendor driver expects **userspace** to submit two half-width planes, one
per LM. Android's SurfaceFlinger does exactly that. Standard Linux compositors
(KWin, wlroots, weston) submit **one full-width plane**, and the result is:

| Submitted | Result |
| --- | --- |
| 1 plane 3200 wide | top strip correct, rest garbage |
| 2 planes 1600 wide, different pipes | correct |
| **1 pipe, both multirect rects** | **correct** |

The third row is the opening: one SSPP can feed both halves through its two
multirect rectangles (RECT0/RECT1).

## The constraint this works around

Android init loads `msm_drm.ko` from `/vendor_dlkm` ~2.9 s after boot. Android
and Linux share the same kernel — `pivot_root` only swaps the root filesystem —
so the guest inherits that exact module instance. It has no wide-plane split
logic.

Replacing it means unloading it first, and that is blocked: `msm_drm`'s
refcount never reaches zero because a handful of display drivers set
`suppress_bind_attrs`, and force-unbinding them deadlocks in
`dsi_ctrl_clocks_deinit()`.

So instead of replacing the module, this one **hooks the hardware programming
path**.

## How it works

Four kprobes, all resolved through kallsyms so `msm_drm` need not export
anything:

| Hook | Type | What it does |
| --- | --- | --- |
| `sde_hw_sspp_setup_rects` | post | rewrite RECT0 to the left half, program RECT1 with the right half |
| `sde_hw_sspp_setup_format` | post | mirror the pixel format into RECT1 |
| `sde_hw_sspp_update_multirect` | pre | turn a `SOLO` call into `RECT_0 \| RECT_1` |
| `sde_hw_lm_setup_blendstage` | pre | give a single-pipe blend stage a RECT1 entry |

Only planes wider than `min_split_width` (default 2561) are touched, so the
normal two-plane path Android uses is left byte-for-byte identical.

### Why each hook is needed

**RECT0 must be rewritten.** The driver programs RECT0 with the *full* width.
Leaving it at 3200 on a 2560-wide LM overflows:

```
before: src_size=0x08580c80  (2136 × 3200)
after:  src_size=0x08580640  (2136 × 1600)
```

**RECT1 needs its own source address.** It reads `SSPP_SRC1_ADDR` /
`SSPP_SRC3_ADDR`; without programming them it fetches stale memory and the
whole screen goes wrong.

**Do not add a byte offset to the RECT1 address.** Upstream's
`dpu_plane_sspp_update_pipe()` hands both pipes the *same* layout; the offset
into the framebuffer comes from `src_rect`, not from the address.

**The multirect enable has to be widened, not added.** This is the subtle one.
`_sde_plane_update_roi_config()` calls `update_multirect()` **last**, with
`SDE_SSPP_RECT_SOLO`, and the implementation does:

```c
if (index == SDE_SSPP_RECT_SOLO)
        mode_mask = 0;                                  /* clears everything */
SDE_REG_WRITE(ctx->hw, SSPP_MULTIRECT_OPMODE + idx, mode_mask);
```

so any RECT1 bit set earlier is wiped. Rewriting that final call's index to
`RECT_0 | RECT_1` makes `mode_mask |= index` produce `0b11`.

## The blend stage encoding

`sde_hw_lm_setup_blendstage()` writes one register per stage
(`LM_BLEND0_FG_SRC_SEL_V1 = 0x04`) holding two source-select bytes:

```
layout 0 -> bits [7:0]      layout 1 -> bits [15:8]
```

each encoded by `_set_staged_sspp()` as `pipe_type(2) | rec_id(2) | pipe_id(4)`,
where `rec_id` 0 means RECT0/SOLO and 1 means RECT1.

Measured on a healthy Android commit (two half-width planes, two pipes):

```
stage[1][0] pipe=13 mi=0 layout=1    stage[1][1] pipe=14 mi=0 layout=0
stage[2][0] pipe=11 mi=0 layout=1    stage[2][1] pipe=12 mi=0 layout=0
stage[3][0] pipe=9  mi=0 layout=1    stage[3][1] pipe=10 mi=0 layout=0
stage[4][0] pipe=3  mi=0 layout=1    stage[4][1] pipe=4  mi=0 layout=0
```

A single-plane commit leaves the second byte at `LM_SRC_SEL_RESET_VALUE`
(`0xC0C0`), so that half of the LM never fetches anything. This module
duplicates the one entry into the free layout byte as RECT1.

## Struct layouts matter

Every layout below was verified with a host-side `offsetof` check. Getting one
field wrong writes garbage into registers silently.

| Struct | Trap |
| --- | --- |
| `sde_hw_stage_cfg` | `SDE_STAGE_MAX` is **12**, not 8 (`BASE=0` + `STAGE_0..10` + `MAX`) |
| `sde_rect` | four **`u16`**, not `s32` |
| `sde_hw_pipe_cfg` | `layout` comes **first**, then the rects |

## Build

Needs a kernel tree configured for the target device (for headers, `Module.symvers`
and vermagic). The tree must match the running kernel's module ABI.

```sh
make KDIR=/path/to/kernel/tree
```

The result is `sde-split-hijack.ko`. `make install DESTDIR=...` copies it to
`/usr/local/lib/piano/`.

### vermagic

`scripts/setlocalversion` appends `-g<sha>-dirty` from git, which will not match
the device. Pin it:

```sh
echo '#define UTS_RELEASE "6.6.57-4k-g34b7c5063ce7"' \
  > $KDIR/include/generated/utsrelease.h
```

Then clear the module directory (`*.o`, `.*.cmd`, `Module.symvers`,
`modules.order`) so it is re-read.

## Use

```sh
insmod sde-split-hijack.ko            # verbose=1 min_split_width=2561
```

Load it after `msm_drm` and before the first commit. It is loaded automatically
by `piano-init` on this device.

Check it is working:

```sh
dmesg | grep sde-split
# sde-split: lm=1 split 1 stage
# sde-split: rect1 src(1600,0 1600x2136) dst(1600,0 1600x2136)
# sde-split: multirect widened to both rects
```

Unload restores stock behaviour on the next commit:

```sh
rmmod sde_split_hijack
```

## Parameters

| Name | Default | Meaning |
| --- | --- | --- |
| `enabled` | `1` | master switch |
| `verbose` | `0` | log every split |
| `min_split_width` | `2561` | source width above which a plane is split |

## Verification

`oneplane` draws left-half red, right-half blue, with three full-width lines at
1/4, 1/2 and 3/4 height. Without the module the top strip is right and the rest
is garbage; with it the whole screen is correct.

## Caveats

- Only wide, unscaled, non-YUV, non-rotated, non-virtual planes are split.
- Unloading mid-commit is not safe; `rmmod` between commits.
- Requires `CONFIG_KPROBES` and `CONFIG_KALLSYMS_ALL`.
- The indirect calls are wrapped in `__nocfi` because the kernel is built with
  KCFI and the addresses come from kprobes.

## Layout

```
src/sde-split-hijack.c   the module
scripts/                 build and deploy helpers
docs/DESIGN.md           deeper notes on the hardware and the hook choice
```

## License

GPL-2.0-only. It links against GPL kernel internals.
