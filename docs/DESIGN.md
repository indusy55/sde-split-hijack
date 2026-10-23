# Design notes

Deeper background on why the module exists and why it hooks what it hooks.
For build and usage, see the README.

---

## 1. The panel and the layer mixers

Xiaomi Pad 8 Pro (`piano`) drives a 3200×2136 panel over two DSI links, each
carrying 1600×2136. The SDE display controller's layer mixer has a hard limit:

```
qcom,sde-mixer-linewidth = <2560>      /* sun-sde-common.dtsi */
```

3200 > 2560, so the CRTC must be composed from **two layer mixers**, each
covering 1600 pixels.

The vendor driver does not split a plane automatically. It expects userspace to
submit one plane per LM. This is a real difference from upstream `dpu`, which
has `dpu_plane_split()`-style handling in `dpu_plane_atomic_check()`.

Measured behaviour on this device:

| Submitted | Result |
| --- | --- |
| 1 plane, 3200 wide | top strip correct, rest garbage |
| 2 planes, 1600 wide, different SSPPs | correct |
| 1 SSPP, both multirect rects | correct |

The third row is what this module exploits: one source pipe can feed both LMs
through RECT0 and RECT1 of its multirect engine.

---

## 2. Why not replace msm_drm

The obvious fix is to patch `msm_drm` and deploy it. That path was built and
verified — the patched module compiles, and all 732 shared symbols match the
device's CRC table — but it cannot be deployed:

- Android init loads `msm_drm` from `/vendor_dlkm` ~2.9 s after boot.
- Android and Linux share one kernel; `pivot_root` only changes the root
  filesystem. The guest inherits that loaded instance.
- The kernel refuses to load a second module with the same name.
- Unloading requires `refcnt == 0`, which never happens:

| Stage | `msm_drm` refcnt |
| --- | --- |
| Android running | 41 (composer HAL holds 17 fds on `/dev/dri/card0`) |
| Android userspace dead | 6 |
| `nt36532_touch` unloaded | 5 |
| two `dsi_phy` unbound | 3 |
| remaining | 3 |

Those last three come from bound platform devices whose drivers set
`suppress_bind_attrs = true`, which removes the sysfs `unbind` file:

```
drm_dsi_ctrl    (×2)
msm-dp-display
sde_cesta
```

`device_release_driver()` ignores that flag, but calling it deadlocks:
`dsi_ctrl_dev_remove()` takes `dsi_ctrl->ctrl_lock` and deinits clocks and
supplies, which blocks once the PHY below it is gone.

Hooking avoids all of it.

---

## 3. The hardware path, in order

For one plane commit, `_sde_plane_update_roi_config()` does roughly:

```
setup_sourceaddress(pipe_hw, pipe_cfg, pstate->multirect_index)
setup_rects(pipe_hw, pipe_cfg, pstate->multirect_index)
setup_format(pipe_hw, fmt, ..., pstate->multirect_index)
...
update_multirect(pipe_hw, true, pstate->multirect_index, pstate->multirect_mode)
```

with `pstate->multirect_index == SDE_SSPP_RECT_SOLO` throughout.

Then, per LM, `sde_crtc_atomic_begin()` calls
`sde_hw_lm_setup_blendstage()` for each mixer, and finally
`sde_hw_ctl_setup_blendstage()` for the CTL path.

### 3.1 RECT0 is programmed full width

`setup_rects()` writes `cfg->src_rect.w` straight into `SSPP_SRC_SIZE`. On a
single-plane commit that is 3200 — wider than the mixer. Left alone, the LM
overflows regardless of what the blend stage says.

The module rewrites RECT0 with half the width before programming RECT1.

### 3.2 RECT1 needs its own registers

RECT1 lives in a separate register bank:

```
SSPP_SRC_SIZE_REC1   0x16C
SSPP_SRC_XY_REC1     0x168
SSPP_OUT_SIZE_REC1   0x160
SSPP_OUT_XY_REC1     0x164
SSPP_SRC1_ADDR       0x18    (luma/chroma 0)
SSPP_SRC3_ADDR                (chroma 1/2)
```

`setup_rects(ctx, cfg, SDE_SSPP_RECT_1)` handles the first four;
`setup_sourceaddress(ctx, cfg, SDE_SSPP_RECT_1)` handles the addresses.

Skip the address step and RECT1 reads whatever was there before — which shows
up as the *entire* screen being wrong, not just half. That was the first wrong
turn during development.

### 3.3 Do not offset the RECT1 address

The intuitive move is "the right half starts half a line further in, so add
half a line to each plane address". That is wrong.

Upstream's `dpu_plane_sspp_update_pipe()` passes the **same** layout struct to
both pipes:

```c
if (layout && pipe->sspp->ops.setup_sourceaddress)
        pipe->sspp->ops.setup_sourceaddress(pipe, layout);
```

The position within the framebuffer comes from `src_rect`, not from the
address. Both rectangles point at the same base; `SSPP_SRC_XY_REC1` says where
to start reading.

### 3.4 update_multirect is called last, with SOLO

This is the subtle one, and it cost the most time.

```c
static void sde_hw_sspp_update_multirect(struct sde_hw_pipe *ctx,
                bool enable,
                enum sde_sspp_multirect_index index,
                enum sde_sspp_multirect_mode mode)
{
        if (index == SDE_SSPP_RECT_SOLO) {
                mode_mask = 0;          /* clears everything */
        } else {
                mode_mask = SDE_REG_READ(&ctx->hw, SSPP_MULTIRECT_OPMODE + idx);
                if (enable)
                        mode_mask |= index;
                ...
        }
        SDE_REG_WRITE(&ctx->hw, SSPP_MULTIRECT_OPMODE + idx, mode_mask);
}
```

Because `_sde_plane_update_roi_config()` calls it **last** with `SOLO`, any
RECT1 bit set earlier is wiped to zero. Programming RECT1 from a
`setup_rects` post-handler and expecting it to survive does not work.

The module therefore intercepts that final call and rewrites its `index`
argument to `SDE_SSPP_RECT_0 | SDE_SSPP_RECT_1` (value 3). Then
`mode_mask |= index` yields `0b11` and both rectangles are live.

---

## 4. Blend stage encoding

`sde_hw_lm_setup_blendstage()` writes one 32-bit register per stage:

```
LM_BLEND0_FG_SRC_SEL_V1 = 0x04,  + stage offset
LM_SRC_SEL_RESET_VALUE  = 0xC0C0
```

The register holds two source-select bytes, one per layout:

```
layout 0 -> bits [7:0]      layout 1 -> bits [15:8]
```

Each byte, from `_set_staged_sspp()`:

```
pipe_type(2) | rec_id(2) | pipe_id(4)
```

- `pipe_type`: 0 = DMA, 1 = VIG
- `rec_id`: 0 = RECT0/SOLO, 1 = RECT1
- `pipe_id`: index within the pipe type

A healthy Android commit, dumped from `sde_hw_lm_setup_blendstage`:

```
stage[1][0] pipe=13 mi=0 layout=1    stage[1][1] pipe=14 mi=0 layout=0
stage[2][0] pipe=11 mi=0 layout=1    stage[2][1] pipe=12 mi=0 layout=0
stage[3][0] pipe=9  mi=0 layout=1    stage[3][1] pipe=10 mi=0 layout=0
stage[4][0] pipe=3  mi=0 layout=1    stage[4][1] pipe=4  mi=0 layout=0
```

Two different pipes per stage, both `rec_id = 0`, one in each layout byte.

A single-plane commit has one entry per stage. The second byte stays at
`0xC0C0`, meaning "no source" — that half of the LM fetches nothing.

The module duplicates the single entry into the free layout byte with
`rec_id = 1`:

```c
cfg->stage[i][1]          = cfg->stage[i][0];
cfg->multirect_index[i][1] = SDE_SSPP_RECT_1;
cfg->layout[i][1]         = !cfg->layout[i][0];
```

Note that both LMs of a CRTC share **one** `stage_cfg`: in
`sde_crtc_atomic_begin()`, `lm_layout = i / MAX_MIXERS_PER_LAYOUT` and
`MAX_MIXERS_PER_LAYOUT == 2`, so mixers 0 and 1 both index `stage_cfg[0]`. The
rewrite therefore happens once and both LMs see it. A second call for the same
`stage_cfg` finds two entries per stage and is skipped, which is correct.

---

## 5. Struct layouts

Every layout was verified with a host-side `offsetof` program rather than
assumed. This matters: a wrong field silently writes garbage into display
registers, and the symptom is just "the screen is wrong", with no error.

| Struct | Layout | Trap |
| --- | --- | --- |
| `sde_hw_stage_cfg` | `stage[12][2]`, `multirect_index[12][2]`, `layout[12][2]` of `u32` | `SDE_STAGE_MAX` is 12, not 8 |
| `sde_rect` | `u16 x, y, w, h` | 8 bytes, not 16 |
| `sde_hw_fmt_layout` | ptr + 4×u32 + 3×`u32[4]` = 72 bytes | — |
| `sde_hw_pipe_cfg` | `layout`, `src_rect`, `dst_rect`, `src_rect_extn`, `dst_rect_extn`, 2×u8 = 112 bytes | `layout` is first |

`SDE_STAGE_MAX` is the dangerous one:

```c
enum sde_stage {
        SDE_STAGE_BASE = 0,
        SDE_STAGE_0, ... SDE_STAGE_10,
        SDE_STAGE_MAX,          /* == 12 */
};
```

Using 8 shifts `multirect_index` and `layout` by 32 bytes each, so every stage
index past the first few reads and writes the wrong slot.

---

## 6. KCFI

The kernel is built with KCFI. Calling a function through an address obtained
from a kprobe trips:

```
CFI failure at init_module+0x1a8 (target: bus_find_device_by_name+0x0/0x130;
expected type: 0x0ab29704)
```

The indirect calls here are wrapped in `__nocfi` functions:

```c
static void __nocfi do_setup_rects(void *ctx, struct sde_hw_pipe_cfg *cfg,
                                   enum sde_sspp_multirect_index idx)
{
        ((setup_rects_fn)setup_rects_addr)(ctx, cfg, idx);
}
```

The probes themselves are fine: kprobe `pre_handler`/`post_handler` are called
by the kernel directly, not through a function pointer derived from a symbol
address.

---

## 7. Why kallsyms and not exported symbols

None of the four functions is exported. They are `static` inside `msm_drm`:

```
$ grep sde_hw_lm_setup_blendstage /proc/kallsyms
0000000000000000 t sde_hw_lm_setup_blendstage   [msm_drm]
```

`t` = local text symbol. They are visible because the kernel has
`CONFIG_KALLSYMS_ALL=y`, and reachable because kprobes accepts a symbol name
and resolves it internally — the module never needs the address at link time.

This also means the module has **no build-time dependency on `msm_drm`'s
symbol table** beyond the usual `Module.symvers` for the handful of core kernel
symbols it does use (`register_kprobe`, `_printk`, `memcpy`, ...).

---

## 8. Verification method

`oneplane` is the decisive test. It sets one full-width plane with:

- left half red, right half blue
- three full-width lines at 1/4, 1/2, 3/4 height (green, white, yellow)

Without the module: the top strip is correct and everything below is garbage.
With it: the whole frame is correct.

A useful intermediate check is the register dump:

```
RECT0 src_size=0x08580640   -> 2136 × 1600   (was 0x08580c80 = 2136 × 3200)
RECT1 src_size=0x08580640   -> 2136 × 1600
RECT1 src_xy  =0x00000640   -> x = 1600
```

---

## 9. Known gaps

- **Colour**: the expected green line reads as yellow on this panel. Likely
  colour-space (the plane reports `ITU-R BT.601 YCbCr`), not a split bug.
- **Compositor**: only `oneplane` has been verified. KWin/Plasma has not been
  run against it yet.
- **Bandwidth**: RECT1 doubles the fetch. No underrun observed at 120 Hz in the
  test pattern, but a real compositor with overlapping planes may differ.
- **Timing**: `setup_sourceaddress` for RECT1 is called from the `setup_rects`
  post-handler. If a future driver revision writes SRC1 after that point, the
  call needs to move.
