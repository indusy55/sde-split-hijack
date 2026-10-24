## [1.1.0] - 2026-10-24

Move the geometry write into the CRTC flush path. This is the version that
actually holds a stable picture; 1.0.0 built and loaded but the panel kept
showing a full-width RECT0.

### Changed

- **The RECT0/RECT1 write now happens in the pre handler of
  `sde_crtc_atomic_flush()`.** Rewriting the geometry from a post handler on
  `sde_hw_sspp_setup_rects()` reads back correctly inside the handler but is
  overwritten before the commit reaches the hardware:

  ```
  mini: SRC_SIZE 00000000 -> 08580640      (1600 wide, inside the handler)
  mini: at flush START SRC_SIZE=08580c80    (back to 3200 wide)
  ```

  From the flush pre handler the value survives the whole commit and the next
  frame. This is the only change that made the module work.

- Dropped `sde_hw_sspp_setup_format` and `sde_hw_sspp_update_multirect` hooks.
  Neither is needed once the write happens late: the driver's own multirect
  programming is correct for the rectangles it sets up, and the format mirrors
  through RECT0's layout.

- Blend stage handling now reads the byte the driver wrote instead of
  composing one. The driver already puts the right `pipe_type` / `pipe_id` into
  both mixers, so the hook only flips `rec_id`, leaving the left mixer on
  RECT0 and moving the right mixer to RECT1. That keeps it correct whichever
  SSPP the compositor picks.

### Added

- `LM_BLEND0_FG_SRC_SEL_V1` stage offsets are taken from
  `qcom,sde-mixer-blend-op-off` (`0x40, 0x70, 0xa0, …` on piano). Assuming
  `0x04 + (n-1) * 0x30` lands on `LM_OUT_SIZE` and silently reports nonsense.

### Verified

- Static test pattern: left half red, right half blue, three horizontal lines
  — correct.
- KWin/sddm compositing at 3200×2136@120 Hz — stable, continuous refresh.

---

## [1.0.0] - 2026-10-23

First version that built and loaded. Superseded by 1.1.0: the four-kprobe
design below did not hold the geometry through a commit.

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
