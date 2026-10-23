// SPDX-License-Identifier: GPL-2.0-only
/*
 * sde-split-hijack - let one wide plane drive both layer mixers of a dual-DSI
 * panel, without replacing msm_drm.
 *
 * The problem
 * -----------
 * The panel is 3200 wide; one layer mixer tops out at 2560
 * (qcom,sde-mixer-linewidth), so the driver needs two LMs. Measured on piano
 * (22-dual-dsi-plane-requirement.md):
 *
 *   1 plane 3200 wide                     -> top strip correct, rest garbage
 *   2 planes 1600 wide, different pipes   -> correct
 *   1 pipe, both multirect rects          -> correct
 *
 * The third row is the opening: one SSPP feeds both halves through its two
 * multirect rectangles.
 *
 * What has to be programmed
 * -------------------------
 * Three things, all of them needed or the screen is worse than before:
 *
 *   1. SSPP rectangles. RECT0 fetches the left half of the framebuffer, RECT1
 *      the right half:
 *        sde_hw_sspp_setup_rects(ctx, &cfg_rect1, SDE_SSPP_RECT_1)
 *      writes SSPP_SRC_SIZE_REC1 / SSPP_SRC_XY_REC1 / OUT_SIZE_REC1 /
 *      OUT_XY_REC1.
 *
 *   2. Multirect enable, otherwise the REC1 registers are ignored:
 *        sde_hw_sspp_update_multirect(ctx, true, SDE_SSPP_RECT_1,
 *                                     SDE_SSPP_MULTIRECT_PARALLEL)
 *
 *   3. Blend stage source select. sde_hw_lm_setup_blendstage() writes one
 *      register per stage holding two source-select bytes, one per layout:
 *        layout 0 -> bits [7:0], layout 1 -> bits [15:8]
 *      each encoded as pipe_type(2) | rec_id(2) | pipe_id(4), where rec_id 0
 *      is RECT0/SOLO and 1 is RECT1. A single-plane commit leaves the second
 *      byte at LM_SRC_SEL_RESET_VALUE, so that half never fetches anything.
 *
 * Android submits two half-width planes on two pipes and needs none of this.
 * A standard compositor submits one full-width plane, which is the case here.
 *
 * Hooks (all present in the stock module, resolved via kallsyms)
 * -------------------------------------------------------------
 *   sde_hw_sspp_setup_rects     post: program RECT1 from RECT0's rect
 *   sde_hw_sspp_setup_format    post: mirror the format into RECT1
 *   sde_hw_lm_setup_blendstage  pre:  give a single-pipe stage a RECT1 entry
 *
 * Only planes wider than one mixer are touched, so the normal two-plane path
 * Android uses is left exactly as it is.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/kprobes.h>
#include <linux/ptrace.h>
#include <linux/types.h>
#include <linux/string.h>

/*
 * Layouts copied from sde_hw_mdss.h / sde_hw_catalog.h. Verified with a
 * host-side offsetof check; see 24-hijack-display-module.md.
 *
 * enum sde_stage is SDE_STAGE_BASE = 0, SDE_STAGE_0 .. SDE_STAGE_10,
 * SDE_STAGE_MAX, so SDE_STAGE_MAX == 12. Getting that wrong shifts every
 * field and silently corrupts the table.
 */
#define SDE_STAGE_MAX		12
#define SDE_STAGE_0		1
#define PIPES_PER_STAGE		2
#define SDE_MAX_PLANES		4

enum sde_sspp_multirect_index {
	SDE_SSPP_RECT_SOLO = 0,
	SDE_SSPP_RECT_0,
	SDE_SSPP_RECT_1,
	SDE_SSPP_RECT_MAX,
};

enum sde_sspp_multirect_mode {
	SDE_SSPP_MULTIRECT_NONE = 0,
	SDE_SSPP_MULTIRECT_PARALLEL,
	SDE_SSPP_MULTIRECT_TIME_MX,
};

struct sde_hw_stage_cfg {
	u32 stage[SDE_STAGE_MAX][PIPES_PER_STAGE];
	u32 multirect_index[SDE_STAGE_MAX][PIPES_PER_STAGE];
	u32 layout[SDE_STAGE_MAX][PIPES_PER_STAGE];
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
	u32 plane_addr[SDE_MAX_PLANES];
	u32 plane_size[SDE_MAX_PLANES];
	u32 plane_pitch[SDE_MAX_PLANES];
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

static bool enabled = true;
module_param(enabled, bool, 0644);
MODULE_PARM_DESC(enabled, "split wide planes across both layer mixers");

static bool verbose;
module_param(verbose, bool, 0644);
MODULE_PARM_DESC(verbose, "log every split");

/*
 * Width above which a plane is considered too wide for one mixer. The panel is
 * 3200 and the mixer limit is 2560, so anything over 2560 needs splitting.
 * Configurable in case the DT value ever changes.
 */
static uint min_split_width = 2561;
module_param(min_split_width, uint, 0644);
MODULE_PARM_DESC(min_split_width, "source width above which a plane is split");

static unsigned long setup_rects_addr;
static unsigned long setup_format_addr;
static unsigned long setup_srcaddr_addr;

static struct kprobe kp_rects;
static struct kprobe kp_format;
static struct kprobe kp_lm;
static struct kprobe kp_mr;


static unsigned long rect_splits;
static unsigned long lm_splits;
static bool split_pending;
static unsigned long mr_calls;

typedef void (*setup_rects_fn)(void *ctx, struct sde_hw_pipe_cfg *cfg,
			       enum sde_sspp_multirect_index idx);
typedef void (*setup_format_fn)(void *ctx, const void *fmt, bool const_alpha,
				u32 flags, enum sde_sspp_multirect_index idx);
typedef void (*setup_srcaddr_fn)(void *ctx, struct sde_hw_pipe_cfg *cfg,
				 enum sde_sspp_multirect_index idx);

/* Call through kprobe-resolved pointers without tripping KCFI. */
static void __nocfi do_setup_rects(void *ctx, struct sde_hw_pipe_cfg *cfg,
				   enum sde_sspp_multirect_index idx)
{
	((setup_rects_fn)setup_rects_addr)(ctx, cfg, idx);
}

static void __nocfi do_setup_format(void *ctx, const void *fmt,
				    bool const_alpha, u32 flags,
				    enum sde_sspp_multirect_index idx)
{
	((setup_format_fn)setup_format_addr)(ctx, fmt, const_alpha, flags, idx);
}

static void __nocfi do_setup_srcaddr(void *ctx, struct sde_hw_pipe_cfg *cfg,
				     enum sde_sspp_multirect_index idx)
{
	((setup_srcaddr_fn)setup_srcaddr_addr)(ctx, cfg, idx);
}

/* ------------------------------------------------------------------------- */
/* SSPP side                                                                 */
/* ------------------------------------------------------------------------- */

/*
 * sde_hw_sspp_setup_rects(struct sde_hw_pipe *ctx, struct sde_hw_pipe_cfg *cfg,
 *                         enum sde_sspp_multirect_index rect_index)
 *
 * x0 = ctx, x1 = cfg, x2 = rect_index.
 *
 * Post handler, so RECT0 is already programmed. Only planes wider than one
 * mixer are touched.
 */
static void rects_post_handler(struct kprobe *p, struct pt_regs *regs,
			       unsigned long flags)
{
	struct sde_hw_pipe_cfg *cfg;
	struct sde_hw_pipe_cfg r_cfg;
	unsigned long idx;
	u16 half;
	void *ctx;

	if (!enabled)
		return;

	ctx = (void *)regs->regs[0];
	cfg = (struct sde_hw_pipe_cfg *)regs->regs[1];
	idx = regs->regs[2];

	if (!cfg || (idx != SDE_SSPP_RECT_SOLO && idx != SDE_SSPP_RECT_0))
		return;

	if (cfg->src_rect.w < min_split_width)
		return;

	half = cfg->src_rect.w / 2;
	if (!half)
		return;

	/*
	 * RECT0 keeps the left half; RECT1 gets the right half. Source
	 * coordinates are in framebuffer space, destination in CRTC space, so
	 * both move right by half.
	 */
	memcpy(&r_cfg, cfg, sizeof(r_cfg));
	r_cfg.src_rect.x = cfg->src_rect.x + half;
	r_cfg.src_rect.w = cfg->src_rect.w - half;
	r_cfg.dst_rect.x = cfg->dst_rect.x + half;
	r_cfg.dst_rect.w = cfg->dst_rect.w - half;

	/*
	 * RECT0 has already been written with the FULL width, which is what
	 * makes the left mixer overflow: one LM tops out at 2560 and the rect
	 * is 3200. Shrink RECT0 to the left half and rewrite it, mirroring
	 * upstream where pipe_cfg->src_rect.x2 is moved to the midpoint before
	 * anything is programmed.
	 */
	cfg->src_rect.w = half;
	cfg->dst_rect.w = half;
	do_setup_rects(ctx, cfg, SDE_SSPP_RECT_0);

	do_setup_rects(ctx, &r_cfg, SDE_SSPP_RECT_1);

	/*
	 * RECT1 fetches from its own address registers (SSPP_SRC1_ADDR /
	 * SRC3_ADDR). Without this the SSPP reads whatever was there before,
	 * which is why the screen went completely wrong instead of half right.
	 *
	 * The right half starts half a line further into the buffer, so add
	 * that byte offset to each plane address.
	 */
	/*
	 * RECT1 gets the SAME plane addresses as RECT0. The offset into the
	 * framebuffer comes from src_rect, not from the address: upstream's
	 * dpu_plane_sspp_update_pipe() passes the identical layout to both
	 * pipes and only differs in pipe_cfg.
	 */
	if (setup_srcaddr_addr)
		do_setup_srcaddr(ctx, &r_cfg, SDE_SSPP_RECT_1);

	/*
	 * Do not call update_multirect here: the driver calls it last with
	 * SOLO, which would clear the bits. multirect_pre_handler widens that
	 * final call instead.
	 */
	split_pending = true;

	rect_splits++;

	if (verbose)
		pr_info("sde-split: rect1 src(%u,%u %ux%u) dst(%u,%u %ux%u)\n",
			r_cfg.src_rect.x, r_cfg.src_rect.y,
			r_cfg.src_rect.w, r_cfg.src_rect.h,
			r_cfg.dst_rect.x, r_cfg.dst_rect.y,
			r_cfg.dst_rect.w, r_cfg.dst_rect.h);
}

/* Mirror the pixel format into RECT1 so both halves decode identically. */
static void format_post_handler(struct kprobe *p, struct pt_regs *regs,
				unsigned long flags)
{
	const void *fmt;
	unsigned long idx;
	bool const_alpha;
	u32 fl;
	void *ctx;

	if (!enabled)
		return;

	ctx = (void *)regs->regs[0];
	fmt = (const void *)regs->regs[1];
	const_alpha = (bool)regs->regs[2];
	fl = (u32)regs->regs[3];
	idx = regs->regs[4];

	if (!fmt || (idx != SDE_SSPP_RECT_SOLO && idx != SDE_SSPP_RECT_0))
		return;

	do_setup_format(ctx, fmt, const_alpha, fl, SDE_SSPP_RECT_1);
}

/* ------------------------------------------------------------------------- */
/* Multirect enable                                                          */
/* ------------------------------------------------------------------------- */

/*
 * sde_hw_sspp_update_multirect(struct sde_hw_pipe *ctx, bool enable,
 *                              enum sde_sspp_multirect_index index,
 *                              enum sde_sspp_multirect_mode mode)
 *
 * x0 = ctx, x1 = enable, x2 = index, x3 = mode.
 *
 * This is the hook that actually makes RECT1 take effect, and the order
 * matters: _sde_plane_update_roi_config() calls it LAST, with
 * index = SDE_SSPP_RECT_SOLO, and the implementation does
 *
 *     if (index == SDE_SSPP_RECT_SOLO)
 *             mode_mask = 0;                      // clears everything
 *     ...
 *     SDE_REG_WRITE(ctx->hw, SSPP_MULTIRECT_OPMODE + idx, mode_mask);
 *
 * so a plain SOLO call wipes any RECT1 bit set earlier. Rewriting the index to
 * (RECT_0 | RECT_1) makes the final write enable both rectangles:
 *
 *     mode_mask |= index   ->  mode_mask = 0b11
 */
static int multirect_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	unsigned long idx = regs->regs[2];

	mr_calls++;
	if (verbose && mr_calls <= 20)
		pr_info("sde-split: update_multirect #%lu enable=%lu idx=%lu\n",
			mr_calls, (unsigned long)regs->regs[1], idx);

	if (!enabled)
		return 0;

	if (idx != SDE_SSPP_RECT_SOLO)
		return 0;

	/*
	 * Only widen this to both rectangles when the plane being programmed is
	 * actually a split one. The flag is set by the rects post handler for
	 * the same commit.
	 */
	if (!split_pending)
		return 0;

	regs->regs[2] = SDE_SSPP_RECT_0 | SDE_SSPP_RECT_1;
	split_pending = 0;

	if (verbose)
		pr_info("sde-split: multirect widened to both rects\n");

	return 0;
}

/* ------------------------------------------------------------------------- */
/* LM side                                                                   */
/* ------------------------------------------------------------------------- */

static int lm_pre_handler(struct kprobe *p, struct pt_regs *regs)
{
	struct sde_hw_stage_cfg *cfg;
	int i, done = 0;

	if (!enabled)
		return 0;

	cfg = (struct sde_hw_stage_cfg *)regs->regs[2];
	if (!cfg)
		return 0;

	for (i = SDE_STAGE_0; i < SDE_STAGE_MAX; i++) {
		u32 a = cfg->stage[i][0];
		u32 b = cfg->stage[i][1];

		if (!a && !b)
			continue;
		/* Two pipes: the compositor submitted one plane per half. */
		if (a && b)
			continue;

		if (!a) {
			cfg->stage[i][0] = b;
			cfg->multirect_index[i][0] = cfg->multirect_index[i][1];
			cfg->layout[i][0] = cfg->layout[i][1];
			a = b;
			cfg->stage[i][1] = 0;
		}

		cfg->stage[i][1] = a;
		cfg->multirect_index[i][1] = SDE_SSPP_RECT_1;
		cfg->layout[i][1] = !cfg->layout[i][0];

		if (cfg->multirect_index[i][0] == SDE_SSPP_RECT_SOLO)
			cfg->multirect_index[i][0] = SDE_SSPP_RECT_0;

		done++;
	}

	lm_splits += done;

	if (done && (verbose || lm_splits <= 8))
		pr_info("sde-split: lm=%lu split %d stage%s\n",
			(unsigned long)regs->regs[1], done,
			done == 1 ? "" : "s");

	return 0;
}

/* ------------------------------------------------------------------------- */

static unsigned long resolve_sym(const char *name)
{
	struct kprobe kp = { .symbol_name = name };
	unsigned long addr;

	if (register_kprobe(&kp) < 0)
		return 0;
	addr = (unsigned long)kp.addr;
	unregister_kprobe(&kp);
	return addr;
}

static int hook(const char *sym, struct kprobe *kp,
		kprobe_pre_handler_t pre, kprobe_post_handler_t post)
{
	memset(kp, 0, sizeof(*kp));
	kp->symbol_name = sym;
	kp->pre_handler = pre;
	kp->post_handler = post;

	return register_kprobe(kp);
}

static int __init sde_split_init(void)
{
	int ret;

	pr_info("sde-split: init, enabled=%d min_width=%u\n",
		enabled, min_split_width);

	setup_rects_addr = resolve_sym("sde_hw_sspp_setup_rects");
	setup_format_addr = resolve_sym("sde_hw_sspp_setup_format");
	setup_srcaddr_addr = resolve_sym("sde_hw_sspp_setup_sourceaddress");
	if (!setup_rects_addr || !setup_format_addr) {
		pr_err("sde-split: cannot resolve sspp ops\n");
		return -ENOENT;
	}
	if (!setup_srcaddr_addr)
		pr_warn("sde-split: no sourceaddress, RECT1 will fetch garbage\n");

	ret = hook("sde_hw_sspp_setup_rects", &kp_rects, NULL,
		   rects_post_handler);
	if (ret) {
		pr_err("sde-split: hook setup_rects failed: %d\n", ret);
		return ret;
	}

	ret = hook("sde_hw_sspp_setup_format", &kp_format, NULL,
		   format_post_handler);
	if (ret)
		pr_warn("sde-split: hook setup_format failed: %d\n", ret);

	ret = hook("sde_hw_sspp_update_multirect", &kp_mr,
		   multirect_pre_handler, NULL);
	if (ret) {
		pr_warn("sde-split: cannot resolve update_multirect: %d\n", ret);
		unregister_kprobe(&kp_rects);
		unregister_kprobe(&kp_format);
		return ret;
	}

	ret = hook("sde_hw_lm_setup_blendstage", &kp_lm, lm_pre_handler, NULL);
	if (ret) {
		pr_err("sde-split: hook lm blendstage failed: %d\n", ret);
		unregister_kprobe(&kp_rects);
		unregister_kprobe(&kp_format);
		unregister_kprobe(&kp_mr);
		return ret;
	}

	pr_info("sde-split: armed rects=%px fmt=%px src=%px mr=%px lm=%px\n",
		kp_rects.addr, kp_format.addr, (void *)setup_srcaddr_addr,
		kp_mr.addr, kp_lm.addr);
	return 0;
}

static void __exit sde_split_exit(void)
{
	unregister_kprobe(&kp_rects);
	if (kp_format.addr)
		unregister_kprobe(&kp_format);
	if (kp_mr.addr)
		unregister_kprobe(&kp_mr);
	unregister_kprobe(&kp_lm);
	pr_info("sde-split: exit, rect_splits=%lu lm_splits=%lu\n",
		rect_splits, lm_splits);
}

module_init(sde_split_init);
module_exit(sde_split_exit);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Split wide planes across both layer mixers of a dual-DSI panel");
