# Contributing

## What this is

A single-purpose kernel module for one device: Xiaomi Pad 8 Pro (`piano`).
It makes a single full-width plane drive both layer mixers of the 3200-wide
dual-DSI panel, because standard compositors submit one plane where the vendor
driver expects two.

It is not a general DRM fix and does not try to be.

## Before you change anything

Read `docs/DESIGN.md`. It documents the four register-level traps that took the
longest to find:

1. RECT0 is programmed full width and must be rewritten.
2. RECT1 needs its own source address registers.
3. Do **not** add a byte offset to the RECT1 address.
4. `update_multirect()` is called last with `SOLO` and wipes the RECT1 bit.

Any patch that touches the SSPP path should account for all four.

## Struct layouts

The module duplicates a few `sde_hw_*` structs instead of including the vendor
headers, because `sde_hw_mdss.h` pulls in `msm_drv.h` and from there most of the
display driver.

That means the layouts are copied by hand and must match. If you touch one,
verify it:

```c
/* host-side, no kernel tree needed */
printf("%zu\n", offsetof(struct sde_hw_pipe_cfg, src_rect));
```

Known traps:

- `SDE_STAGE_MAX` is 12, not 8.
- `sde_rect` is four `u16`, not `s32`.
- In `sde_hw_pipe_cfg`, `layout` comes before the rects.

## Testing

There is no test suite; this needs real hardware. The procedure:

1. Build and load the module.
2. Run a single-plane full-width test (`oneplane` on piano).
3. The expected frame is left-half red, right-half blue, with three
   full-width lines at 1/4, 1/2 and 3/4 height.
4. Without the module the top strip is right and the rest is garbage.

Register-level check, if you have a debug build:

```
RECT0 src_size should be h<<16 | 1600   (not 3200)
RECT1 src_size should match
RECT1 src_xy   should be 1600
```

## Style

- Kernel coding style, tabs, 80 columns.
- Comments explain *why*, not *what*. The four traps above each get a paragraph
  because they are not obvious from reading the code.
- No `printk` on the hot path unless `verbose` is set.

## License

GPL-2.0-only, matching the kernel. Contributions are accepted under the same
terms.
