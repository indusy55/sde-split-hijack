## [1.0.0] - 2026-10-23

First working version. Verified on Xiaomi Pad 8 Pro (`piano`, 25091RP04C),
HyperOS OS4.0.4.0, stock GKI 6.6.118.

### Added

- Four kprobes that let a single wide plane drive both layer mixers of the
  3200×2136 dual-DSI panel:
  - `sde_hw_sspp_setup_rects` (post) — rewrite RECT0 to the left half and
    program RECT1 with the right half
  - `sde_hw_sspp_setup_format` (post) — mirror the pixel format into RECT1
  - `sde_hw_sspp_update_multirect` (pre) — widen the final `SOLO` call to
    `RECT_0 | RECT_1`
  - `sde_hw_lm_setup_blendstage` (pre) — give a single-pipe blend stage a
    RECT1 entry
- Parameters `enabled`, `verbose`, `min_split_width`.
- Build and deploy scripts, design notes.

### Notes

- Only planes wider than `min_split_width` (default 2561) are touched, so the
  two-plane path Android uses is unaffected.
- The kernel is built with KCFI; indirect calls go through `__nocfi` wrappers.

### Traps found while getting here

Recorded because each one produced "the screen is wrong" with no error:

1. RECT0 is programmed with the full width and must be rewritten, or the LM
   overflows (2560 limit vs 3200 rect).
2. RECT1 has its own address registers (`SSPP_SRC1_ADDR` / `SSPP_SRC3_ADDR`);
   skipping them makes the whole screen wrong, not half.
3. The RECT1 address must **not** be offset — the position comes from
   `src_rect`, matching upstream's `dpu_plane_sspp_update_pipe()`.
4. `update_multirect()` runs last with `SOLO`, and its implementation sets
   `mode_mask = 0` in that branch, wiping any RECT1 bit set earlier.
