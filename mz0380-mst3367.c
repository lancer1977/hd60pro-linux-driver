// SPDX-License-Identifier: GPL-2.0
/*
 * mz0380-mst3367.c - MStar MST3367 HDMI receiver bring-up + signal detect.
 *
 * The card firmware is a dumb I2C/GPIO proxy: the host drives the MST3367
 * entirely over the mailbox (op 0x1a/0x1b register R/W to dev 0x9c, op 0x15
 * GPIO set). RE_FINDINGS.md M11-M15 documents the whole path; the register
 * values here come from the GPL hdcapm driver (docs/re-2026-07-05/), which
 * RE'd the same Windows driver for a sibling board.
 *
 * The key finding (M14/M15): the MST3367 powers up held in reset by GPIO pin9
 * (active-low), so every I2C access NAKs until the host releases it. This file
 * releases pin9, runs the init sequence, then reads the mode-detect registers
 * and maps the measured geometry to a v4l2_dv_timings.
 */

#include <linux/delay.h>
#include <linux/v4l2-dv-timings.h>
#include <media/v4l2-dv-timings.h>

#include "mz0380.h"
#include "mz0380-reg.h"

/* --- low-level MST3367 register access over the mailbox proxy ------------- */

static int mst_wr(struct mz0380_dev *dev, u8 reg, u8 val)
{
	return mz0380_periph_write(dev, MST3367_I2C_DEV, reg, val);
}

static int mst_rd(struct mz0380_dev *dev, u8 reg, u8 *val)
{
	u32 v = 0;
	int ret = mz0380_periph_read(dev, MST3367_I2C_DEV, reg, &v);

	if (ret)
		return ret;
	*val = v & 0xff;
	return 0;
}

/* Bank select is a normal write to reg 0x00; the driver caches nothing, so
 * callers must select the bank before each banked access batch. */
static int mst_bank(struct mz0380_dev *dev, u8 bank)
{
	return mst_wr(dev, MST3367_REG_BANK_SELECT, bank);
}

/* --- GPIO (op 0x15, single-pin mask+data; see reg.h GPIO pin map) --------- */

static int mz0380_gpio_set(struct mz0380_dev *dev, unsigned int pin, int level)
{
	u32 params[2] = { 1u << pin, (u32)(!!level) << pin };

	return mz0380_send_command(dev, MZ0380_CMD_GPIO_SET, params, 2,
				   NULL, 500);
}

/*
 * Release the MST3367 from reset. The card holds pin9 (active-low reset) low at
 * power-up; Windows pulses pin9 1->0->1 with pin3 (RX enable) high and pin8
 * (companion strap) alongside. Until this runs, the receiver I2C bus is dead.
 */
static void mz0380_mst3367_reset(struct mz0380_dev *dev)
{
	mz0380_gpio_set(dev, MZ0380_GPIO_RX_ENABLE, 1);   /* RX / mux enable  */
	mz0380_gpio_set(dev, MZ0380_GPIO_RX_RESET, 1);    /* released baseline */
	usleep_range(2000, 3000);
	mz0380_gpio_set(dev, MZ0380_GPIO_RX_STRAP, 0);    /* strap low        */
	mz0380_gpio_set(dev, MZ0380_GPIO_RX_RESET, 0);    /* ASSERT reset     */
	usleep_range(5000, 6000);
	mz0380_gpio_set(dev, MZ0380_GPIO_RX_RESET, 1);    /* RELEASE reset    */
	mz0380_gpio_set(dev, MZ0380_GPIO_RX_STRAP, 1);    /* strap high       */
	usleep_range(10000, 12000);                       /* PLL/I2C come-up  */
}

/*
 * MST3367 init_setup (hdcapm mst3367_init_setup, cross-validated by our disasm).
 * Runs after the reset release; configures HDMI RX path, HDCP receive, YUV422
 * 8-bit output, then a HDMI + HDCP block reset.
 */
static void mz0380_mst3367_init_regs(struct mz0380_dev *dev)
{
	mst_bank(dev, MST3367_BANK0);
	mst_wr(dev, 0xb7, 0x02);   /* HPD off during config */
	mst_wr(dev, 0x41, 0x6f);
	mst_wr(dev, 0xb8, 0x00);
	mst_bank(dev, MST3367_BANK1);
	mst_wr(dev, 0x0f, 0x02);
	mst_wr(dev, 0x16, 0x30);
	mst_wr(dev, 0x24, 0x40);   /* HDCP receive */
	mst_bank(dev, MST3367_BANK0);
	mst_wr(dev, 0xb0, 0x14);
	mst_wr(dev, 0xb1, 0xe0);
	mst_bank(dev, MST3367_BANK2);
	mst_wr(dev, 0x01, 0x61);
	mst_wr(dev, 0x02, 0xf5);
	mst_bank(dev, MST3367_BANK0);
	mst_wr(dev, 0x51, 0x89);
	mst_wr(dev, 0xb7, 0x00);   /* HPD + link on */
	usleep_range(2000, 3000);
	mst_wr(dev, 0xb0, 0x20);   /* YUV422 8-bit output */

	/* HDMI reset (BANK2 0x07 f4->04) + HDCP reset (BANK0 0xb8 10->00) */
	mst_bank(dev, MST3367_BANK2);
	mst_wr(dev, 0x07, 0xf4);
	mst_wr(dev, 0x07, 0x04);
	usleep_range(2000, 3000);
	mst_bank(dev, MST3367_BANK0);
	mst_wr(dev, 0xb8, 0x10);
	mst_wr(dev, 0xb8, 0x00);
	usleep_range(2000, 3000);
}

int mz0380_mst3367_bringup(struct mz0380_dev *dev)
{
	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_warn("%s: MST3367 bring-up skipped - firmware not ready\n",
			dev->name);
		return -ENODEV;
	}

	mz0380_mst3367_reset(dev);
	mz0380_mst3367_init_regs(dev);
	dev->mst3367_ready = true;
	pr_info("%s: MST3367 receiver brought up (reset released, init applied)\n",
		dev->name);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_bringup);

/* --- signal detect -------------------------------------------------------- */

/*
 * Map measured geometry to a standard v4l2_dv_timings. We know active width,
 * interlace, and an approximate frame rate; that uniquely picks a CEA preset
 * for the modes this HDMI capture card supports. The preset carries the exact
 * blanking/pixelclock the raw detect registers do not give us directly.
 *
 * fps100 (hundredths of frames/s) is derived from the vperiod counter, which
 * the hdcapm table shows is ~= frame_rate * 10 (e.g. ~600 => 60.00p, ~300 =>
 * 30.00 frames, i.e. 1080i60). Matching uses a +/-150 window on fps100.
 */
struct mst3367_mode {
	u16 hactive;
	bool interlaced;
	u16 fps100_min;
	u16 fps100_max;
	struct v4l2_dv_timings timings;
};

static const struct mst3367_mode mst3367_modes[] = {
	{ 1920, true,  2900, 3100, V4L2_DV_BT_CEA_1920X1080I60 },
	{ 1920, true,  2400, 2600, V4L2_DV_BT_CEA_1920X1080I50 },
	{ 1920, false, 5900, 6100, V4L2_DV_BT_CEA_1920X1080P60 },
	{ 1920, false, 4900, 5100, V4L2_DV_BT_CEA_1920X1080P50 },
	{ 1920, false, 2900, 3100, V4L2_DV_BT_CEA_1920X1080P30 },
	{ 1920, false, 2400, 2600, V4L2_DV_BT_CEA_1920X1080P25 },
	{ 1920, false, 2300, 2500, V4L2_DV_BT_CEA_1920X1080P24 },
	{ 1280, false, 5900, 6100, V4L2_DV_BT_CEA_1280X720P60 },
	{ 1280, false, 4900, 5100, V4L2_DV_BT_CEA_1280X720P50 },
	{ 1280, false, 2900, 3100, V4L2_DV_BT_CEA_1280X720P30 },
	{ 720,  true,  5900, 6100, V4L2_DV_BT_CEA_720X480I59_94 },
	{ 720,  false, 5900, 6100, V4L2_DV_BT_CEA_720X480P59_94 },
	{ 720,  true,  4900, 5100, V4L2_DV_BT_CEA_720X576I50 },
	{ 720,  false, 4900, 5100, V4L2_DV_BT_CEA_720X576P50 },
};

static const struct v4l2_dv_timings *
mst3367_match_mode(u16 hactive, bool interlaced, u16 fps100)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(mst3367_modes); i++) {
		const struct mst3367_mode *m = &mst3367_modes[i];

		if (m->hactive == hactive && m->interlaced == interlaced &&
		    fps100 >= m->fps100_min && fps100 <= m->fps100_max)
			return &m->timings;
	}
	return NULL;
}

/*
 * Read the MST3367 mode-detect block and fill *out. Returns 0 with a valid
 * timing when a signal is locked, -ENOLCK when no signal, or a negative errno
 * on an I2C failure. Runs the bring-up on first use if it hasn't happened yet.
 */
int mz0380_mst3367_read_signal(struct mz0380_dev *dev,
			       struct v4l2_dv_timings *out)
{
	u8 detect, ilo, ihi, hlo, hhi, hp_hi, hp_lo, vp_hi, vp_lo;
	const struct v4l2_dv_timings *match;
	u16 hactive, htotal, vperiod_raw, fps100;
	bool interlaced;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		if (ret)
			return ret;
	}

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	ret = mst_rd(dev, MST3367_B0_DETECT, &detect);
	if (ret)
		return ret;
	if (!(detect & MST3367_B0_DETECT_LOCK_MASK))
		return -ENOLCK;                     /* receiver sees no signal */

	/* geometry (BANK0) */
	if (mst_rd(dev, MST3367_B0_HTOTAL_HI, &hhi) ||
	    mst_rd(dev, MST3367_B0_HTOTAL_LO, &hlo) ||
	    mst_rd(dev, MST3367_B0_VPERIOD_HI, &vp_hi) ||
	    mst_rd(dev, MST3367_B0_VPERIOD_LO, &vp_lo) ||
	    mst_rd(dev, MST3367_B0_HPERIOD_HI, &hp_hi) ||
	    mst_rd(dev, MST3367_B0_HPERIOD_LO, &hp_lo) ||
	    mst_rd(dev, MST3367_B0_INTERLACE, &ilo))
		return -EIO;

	htotal = ((u16)hhi << 8) | hlo;
	interlaced = ilo & MST3367_B0_INTERLACE_BIT;
	vperiod_raw = ((u16)vp_hi << 8) | vp_lo;

	/* active width lives in BANK2 */
	if (mst_bank(dev, MST3367_BANK2) ||
	    mst_rd(dev, MST3367_B2_HACTIVE_HI, &ihi) ||
	    mst_rd(dev, MST3367_B2_HACTIVE_LO, &hlo))
		return -EIO;
	hactive = ((u16)ihi << 8) | hlo;

	/*
	 * The vperiod register is a line-count-per-field counter; the frame rate
	 * is 1250000 / counter, which the hdcapm table shows lands at ~= fps*10
	 * (e.g. counter 4175 -> 299 -> ~30 frame/s = 1080i60, counter 2083 ->
	 * 600 -> 60p). fps100 = that * 10.
	 */
	if (vperiod_raw)
		fps100 = (1250000u / vperiod_raw) * 10;
	else
		fps100 = 0;

	match = mst3367_match_mode(hactive, interlaced, fps100);
	if (!match) {
		pr_info("%s: MST3367 locked but unmatched: hact=%u htot=%u %s fps100~%u [R55=0x%02x hp=%u vp=%u il=0x%02x] - please report\n",
			dev->name, hactive, htotal, interlaced ? "i" : "p",
			fps100, detect,
			((u16)hp_hi << 8) | hp_lo, vperiod_raw, ilo);
		return -ERANGE;
	}

	*out = *match;
	pr_info("%s: MST3367 signal: %ux%u%s (hact=%u htot=%u fps100~%u R55=0x%02x)\n",
		dev->name, out->bt.width, out->bt.height,
		out->bt.interlaced ? "i" : "p", hactive, htotal, fps100, detect);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_read_signal);
