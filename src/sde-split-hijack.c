// SPDX-License-Identifier: GPL-2.0-only
/*
 * sde-split-hijack - let one wide plane drive both layer mixers of a dual-DSI
 * panel, without replacing msm_drm.
 *
 * The problem
 * -----------
 * The panel is 3200 wide; one layer mixer tops out at 2560
 * (qcom,sde-mixer-linewidth), so the driver needs two LMs. Measured on piano:
 *
 *   1 plane 3200 wide                     -> garbage
 *   2 planes 1600 wide, different pipes   -> correct
 *   1 pipe, both multirect rects          -> correct
 *
 * A standard compositor submits one full-width plane, which is the broken
 * case. One SSPP can serve both halves through its two multirect rectangles.
 *
 * Where the code has to run
 * -------------------------
 * This is the part that took the longest to get right, so it is worth stating
 * plainly: the write must happen in the pre handler of
 * sde_crtc_atomic_flush().
 *
 * Rewriting the geometry from a post handler on sde_hw_sspp_setup_rects() does
 * not work. The value reads back correctly inside the handler:
 *
 *     mini: SRC_SIZE 00000000 -> 08580640      (1600 wide)
 *
 * but by the time the CRTC flush starts it is 08580c80 again:
 *
 *     mini: at flush START SRC_SIZE=08580c80
 *
 * Something later in the same commit rewrites it. From the flush pre handler
 * the write survives the whole commit and the next frame:
 *
 *     flush START   08580c80
 *     after write   08580640
 *     flush end     08580640
 *     restore       08580640
 *     next frame    08580640
 *
 * With only RECT0 shrunk the screen shows the left half correctly and the
 * right half grey, which is exactly what "RECT1 never programmed" predicts.
 *
 * Routing is not the CTL's job here
 * ---------------------------------
 * piano's CTL advertises SDE_CTL_NO_LAYER_EXT, so ops->setup_blendstage is left
 * NULL and routing uses CTL_PIPE_ACTIVE / CTL_LAYER_ACTIVE, which are per-SSPP
 * and do not distinguish the two rectangles. Which rectangle a mixer reads is
 * decided solely by the layer mixer's stage register:
 *
 *     LM_BLEND0_FG_SRC_SEL_V1 (stage base + 0x04), one byte per layout:
 *         pipe_type(2) << 6 | rec_id(2) << 4 | pipe_id(4)
 *
 * The stage blocks are not evenly spaced from zero; their offsets come from
 * qcom,sde-mixer-blend-op-off, which on piano reads
 *
 *     64 112 160 208 256 304 352 400 448 496 544
 *
 * i.e. 0x40, 0x70, 0xa0, ... Do not assume 0x04 + (n-1) * 0x30; that lands on
 * LM_OUT_SIZE and silently reports nonsense.
 *
 * The driver already writes the correct pipe_type/pipe_id for the plane into
 * both mixers. So this module never invents those bits: it reads the byte the
 * driver wrote and only flips rec_id, leaving the left mixer on RECT0 and
 * putting the right mixer on RECT1. That keeps it working no matter which SSPP
 * the compositor picks.
 *
 * Android submits two half-width planes on two pipes and needs none of this.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/io.h>
#include <linux/string.h>

/* ---- layouts, copied from the vendor headers ---- */

struct sde_hw_blk_reg_map {
	void __iomem *base_off;
	u32 blk_off;
	u32 length;
	u32 xin_id;
	u32 hw_rev;
	u32 log_mask;
};

/* sde_rect is four u16, not four s32. */
struct sde_rect {
	u16 x;
	u16 y;
	u16 w;
	u16 h;
};

struct sde_hw_fmt_layout {
	const void *format;
	u32 num_planes;
	u32 width;
	u32 height;
	u32 total_size;
	u32 plane_addr[4];
	u32 plane_size[4];
	u32 plane_pitch[4];
};

/* Full sde_hw_pipe_cfg: layout first, then the four rects. */
struct sde_hw_pipe_cfg {
	struct sde_hw_fmt_layout layout;
	struct sde_rect src_rect;
	struct sde_rect dst_rect;
	struct sde_rect src_rect_extn;
	struct sde_rect dst_rect_extn;
	u8 horz_decimation;
	u8 vert_decimation;
};

struct sde_hw_pipe_head {
	struct sde_hw_blk_reg_map hw;
	void *catalog;
	void *mdp;
	u32 idx;
	void *cap;
};

struct sde_hw_mixer_head {
	struct sde_hw_blk_reg_map hw;
	void *cap;
	void *cfg;
	void *ops;
};

/* ---- SSPP registers, from sde_hw_sspp.c ---- */
#define SSPP_SRC_SIZE		0x00
#define SSPP_SRC_XY		0x08
#define SSPP_OUT_SIZE		0x0c
#define SSPP_OUT_XY		0x10
#define SSPP_SRC0_ADDR		0x14
#define SSPP_SRC1_ADDR		0x18
#define SSPP_SRC2_ADDR		0x1c
#define SSPP_SRC3_ADDR		0x20
#define SSPP_SRC_YSTRIDE0	0x24
#define SSPP_SRC_YSTRIDE1	0x28
#define SSPP_SRC_FORMAT		0x30
#define SSPP_SRC_UNPACK		0x34
#define SSPP_SRC_OP_MODE	0x38
#define SSPP_UBWC_STATIC_CTRL	0x44
#define SSPP_CDP_CNTL		0x134
#define SSPP_MULTIRECT_OPMODE	0x170
#define SSPP_R1_OUT_SIZE	0x160
#define SSPP_R1_OUT_XY		0x164
#define SSPP_R1_SRC_XY		0x168
#define SSPP_R1_SRC_SIZE	0x16c
#define SSPP_R1_SRC_FORMAT	0x174
#define SSPP_R1_SRC_UNPACK	0x178
#define SSPP_R1_SRC_OP_MODE	0x17c
#define SSPP_R1_UBWC_STATIC	0x1c0
#define SSPP_R1_CDP_CNTL	0x13c

/* ---- LM registers, from sde_hw_lm.c ---- */
#define LM_OP_MODE		0x00
#define LM_STAGE_1_OFF		0x40
#define LM_BLEND0_FG_SRC_SEL	0x04
#define LM_SRC_SEL_RESET	0xc0
#define LM_OP_MODE_RIGHT	BIT(31)

/* One entry per layer mixer that took part in the commit. */
#define MAX_LMS	8

struct lm_slot {
	struct sde_hw_mixer_head *lm;
	unsigned long lm_id;
};

static struct lm_slot lms[MAX_LMS];
static int n_lms;

/*
 * The split pipe and its geometry, captured from setup_rects and kept until the
 * plane stops being wide. Steady-state frames do not call setup_rects at all
 * (it is gated on SDE_PLANE_DIRTY_RECTS), so the flush handler re-applies this
 * on every commit.
 */
static struct sde_hw_pipe_head *split_pipe;
static struct sde_hw_pipe_cfg split_cfg;
static bool split_active;

static bool enabled = true;
module_param(enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "split wide planes across both layer mixers");

static bool verbose;
module_param(verbose, bool, 0644);
MODULE_PARM_DESC(verbose, "log every split");

static uint min_split_width = 2561;
module_param(min_split_width, uint, 0644);
MODULE_PARM_DESC(min_split_width, "width above which a plane is split");

static unsigned long n_captured;
static unsigned long n_applied;

/*
 * Capture the pipe and its geometry. Nothing is written here: writes from this
 * hook do not survive the commit (see the file header).
 */
static void rects_post(struct kprobe *p, struct pt_regs *regs,
		       unsigned long flags)
{
	struct sde_hw_pipe_head *ph = (void *)regs->regs[0];
	struct sde_hw_pipe_cfg *cfg = (void *)regs->regs[1];
	unsigned long idx = regs->regs[2];

	if (!enabled || !ph || !cfg || !ph->hw.base_off)
		return;

	/* Only the master rectangle describes the whole plane. */
	if (idx != 0)
		return;

	/*
	 * A narrow plane means the compositor went back to the two-plane layout,
	 * or the plane shrank. Either way this pipe is no longer ours to split.
	 */
	if (cfg->src_rect.w < min_split_width) {
		if (split_pipe == ph) {
			split_active = false;
			split_pipe = NULL;
			if (verbose)
				pr_info("sde-split: sspp %u no longer wide, off\n",
					ph->idx);
		}
		return;
	}

	/*
	 * Parallel multirect cannot scale: both rectangles are fetched by the
	 * same SSPP and the scaler is bypassed for RECT1. Refuse anything that
	 * is not 1:1, matching _sde_plane_validate_multirect_v2().
	 */
	if (cfg->src_rect.w != cfg->dst_rect.w)
		return;

	n_captured++;
	split_pipe = ph;
	memcpy(&split_cfg, cfg, sizeof(split_cfg));
	split_active = true;

	if (verbose)
		pr_info("sde-split: captured sspp=%u src %ux%u+%u+%u dst %ux%u\n",
			ph->idx, cfg->src_rect.w, cfg->src_rect.h,
			cfg->src_rect.x, cfg->src_rect.y,
			cfg->dst_rect.w, cfg->dst_rect.h);
}

/* Remember which mixers are in play. Stable across frames. */
static int lm_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct sde_hw_mixer_head *mh = (void *)regs->regs[0];
	int i;

	if (!mh || !mh->hw.base_off)
		return 0;

	for (i = 0; i < n_lms; i++)
		if (lms[i].lm == mh)
			return 0;

	if (n_lms < MAX_LMS) {
		lms[n_lms].lm = mh;
		lms[n_lms].lm_id = regs->regs[1];
		n_lms++;
		if (verbose)
			pr_info("sde-split: lm hook ctx=%px id=%lu (n=%d)\n",
				mh, (unsigned long)regs->regs[1], n_lms);
	}
	return 0;
}

/*
 * Program both rectangles and point the two mixers at different ones.
 *
 * Runs after every plane update of the commit and immediately before the CTL is
 * kicked off, which is the only window where the writes survive.
 */
static int flush_pre(struct kprobe *p, struct pt_regs *regs)
{
	struct sde_hw_pipe_head *ph = split_pipe;
	struct sde_hw_pipe_cfg *cfg = &split_cfg;
	void __iomem *b;
	u16 w, h, sx, sy, dx, dy, half;
	int i;

	if (!enabled || !split_active || !ph)
		return 0;

	b = ph->hw.base_off + ph->hw.blk_off;

	w = cfg->src_rect.w;
	h = cfg->src_rect.h;
	sx = cfg->src_rect.x;
	sy = cfg->src_rect.y;
	dx = cfg->dst_rect.x;
	dy = cfg->dst_rect.y;

	/* Odd widths would leave a column unfetched; refuse rather than guess. */
	if (w < 2 || (w & 1))
		return 0;

	half = w / 2;

	/* ---- RECT0: left half ---- */
	iowrite32((h << 16) | half, b + SSPP_SRC_SIZE);
	iowrite32((sy << 16) | sx, b + SSPP_SRC_XY);
	iowrite32((h << 16) | half, b + SSPP_OUT_SIZE);
	iowrite32((dy << 16) | dx, b + SSPP_OUT_XY);

	/* ---- RECT1: right half ---- */
	iowrite32((h << 16) | half, b + SSPP_R1_SRC_SIZE);
	iowrite32((sy << 16) | (sx + half), b + SSPP_R1_SRC_XY);
	iowrite32((h << 16) | half, b + SSPP_R1_OUT_SIZE);
	iowrite32((dy << 16) | (dx + half), b + SSPP_R1_OUT_XY);

	/*
	 * RECT1 shares RECT0's plane addresses. The horizontal offset comes from
	 * SRC_XY_REC1, not from the address; upstream passes the identical
	 * layout to both pipes too.
	 */
	iowrite32(ioread32(b + SSPP_SRC0_ADDR), b + SSPP_SRC1_ADDR);
	iowrite32(ioread32(b + SSPP_SRC2_ADDR), b + SSPP_SRC3_ADDR);

	/* ---- everything that describes the pixel format ---- */
	iowrite32(ioread32(b + SSPP_SRC_FORMAT), b + SSPP_R1_SRC_FORMAT);
	iowrite32(ioread32(b + SSPP_SRC_UNPACK), b + SSPP_R1_SRC_UNPACK);
	iowrite32(ioread32(b + SSPP_SRC_OP_MODE), b + SSPP_R1_SRC_OP_MODE);
	iowrite32(ioread32(b + SSPP_UBWC_STATIC_CTRL), b + SSPP_R1_UBWC_STATIC);
	iowrite32(ioread32(b + SSPP_CDP_CNTL), b + SSPP_R1_CDP_CNTL);

	/*
	 * YSTRIDE packs two planes: low 16 bits for plane 0, high 16 for plane 2.
	 * RECT1 uses the high half, so its pitch goes up there.
	 */
	{
		u32 y0 = ioread32(b + SSPP_SRC_YSTRIDE0);
		u32 y1 = ioread32(b + SSPP_SRC_YSTRIDE1);

		y0 = (y0 & 0x0000ffff) |
		     ((u32)(cfg->layout.plane_pitch[0] << 16) & 0xffff0000);
		y1 = (y1 & 0x0000ffff) |
		     ((u32)(cfg->layout.plane_pitch[2] << 16) & 0xffff0000);
		iowrite32(y0, b + SSPP_SRC_YSTRIDE0);
		iowrite32(y1, b + SSPP_SRC_YSTRIDE1);
	}

	/* ---- both rectangles enabled, parallel fetch ---- */
	iowrite32(0x3, b + SSPP_MULTIRECT_OPMODE);

	/*
	 * ---- give each mixer a different rectangle ----
	 *
	 * The driver already wrote the right pipe_type / pipe_id for this plane
	 * into every mixer's stage byte. Only rec_id changes: 0 for the left
	 * mixer, 1 for the right one. Deriving "right" from LM_OP_MODE bit 31 is
	 * what sde_hw_lm_setup_out() sets for the right mixer of a pair, so this
	 * does not depend on mixer ordering or on which SSPP was picked.
	 */
	for (i = 0; i < n_lms; i++) {
		void __iomem *lb = lms[i].lm->hw.base_off +
				   lms[i].lm->hw.blk_off;
		u32 op = ioread32(lb + LM_OP_MODE);
		u32 sel = ioread32(lb + LM_STAGE_1_OFF + LM_BLEND0_FG_SRC_SEL);
		u32 old = sel & 0xff;
		u32 byte;

		/* Nothing staged here, leave it alone. */
		if (old == LM_SRC_SEL_RESET)
			continue;

		byte = (op & LM_OP_MODE_RIGHT) ? ((old & ~0x30u) | 0x10)
					       : (old & ~0x30u);
		if (byte == old)
			continue;

		sel = (sel & ~0xffu) | byte;
		iowrite32(sel, lb + LM_STAGE_1_OFF + LM_BLEND0_FG_SRC_SEL);

		if (verbose)
			pr_info("sde-split: lm%lu stage1 %08x -> %08x\n",
				lms[i].lm_id, (u32)old, byte);
	}

	wmb();

	n_applied++;

	if (verbose && (n_applied <= 4 || !(n_applied % 120)))
		pr_info("sde-split: applied #%lu rect0 %ux%u+%u+%u rect1 %ux%u+%u+%u mr=%08x\n",
			n_applied, half, h, sx, sy, half, h, sx + half,
			dy, ioread32(b + SSPP_MULTIRECT_OPMODE));

	return 0;
}

static struct kprobe kp_rects = {
	.symbol_name = "sde_hw_sspp_setup_rects",
	.post_handler = rects_post,
};

static struct kprobe kp_lm = {
	.symbol_name = "sde_hw_lm_setup_blendstage",
	.pre_handler = lm_pre,
};

static struct kprobe kp_flush = {
	.symbol_name = "sde_crtc_atomic_flush",
	.pre_handler = flush_pre,
};

static int __init sde_split_init(void)
{
	int r;

	r = register_kprobe(&kp_rects);
	if (r) {
		pr_err("sde-split: rects hook failed %d\n", r);
		return r;
	}

	r = register_kprobe(&kp_lm);
	if (r)
		pr_warn("sde-split: lm hook failed %d\n", r);

	r = register_kprobe(&kp_flush);
	if (r) {
		pr_err("sde-split: flush hook failed %d\n", r);
		unregister_kprobe(&kp_rects);
		if (kp_lm.addr)
			unregister_kprobe(&kp_lm);
		return r;
	}

	pr_info("sde-split: armed, min_width=%u\n", min_split_width);
	return 0;
}

static void __exit sde_split_exit(void)
{
	unregister_kprobe(&kp_rects);
	if (kp_lm.addr)
		unregister_kprobe(&kp_lm);
	unregister_kprobe(&kp_flush);

	pr_info("sde-split: exit, captured=%lu applied=%lu\n",
		n_captured, n_applied);
}

module_init(sde_split_init);
module_exit(sde_split_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Split wide planes across both layer mixers of a dual-DSI panel");
