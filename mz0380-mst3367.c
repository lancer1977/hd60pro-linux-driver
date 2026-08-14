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
#include "mz0380-edid.h"

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

/*
 * Load the EDID into the receiver's DDC EEPROM (I2C 0xA0) with the bulk-write
 * opcode, 32 bytes per command (M10: PARAM1 = (len<<16)|(block<<8)|dev8, then
 * the bytes little-endian). This is the half of "look like a sink" that the
 * driver has been missing: an HDMI source that cannot read an EDID keeps its
 * transmitter off, so the receiver never locks no matter what else we do.
 */
static int mz0380_mst3367_load_edid(struct mz0380_dev *dev)
{
	unsigned int off;
	int ret;

	for (off = 0; off < MZ0380_EDID_SIZE; off += MZ0380_EDID_CHUNK) {
		/* payload = offset byte + the chunk, padded to whole words */
		u8 buf[1 + MZ0380_EDID_CHUNK] = { 0 };
		u32 params[1 + (sizeof(buf) + 3) / 4] = { 0 };
		unsigned int i;

		buf[0] = off;                            /* EEPROM byte offset */
		memcpy(&buf[1], &mz0380_edid_default[off], MZ0380_EDID_CHUNK);

		params[0] = MZ0380_EDID_I2C_DEV |
			    ((u32)MZ0380_I2C_COMBO_WRITE << 8) |
			    ((u32)sizeof(buf) << 16);
		for (i = 0; i < sizeof(buf); i++)
			params[1 + i / 4] |= (u32)buf[i] << (8 * (i % 4));

		/*
		 * M47: default timeout 0 = fire-and-forget. The card posts no
		 * completion for this opcode (it returned -110 while the very
		 * next command worked), exactly like START_STREAMING.
		 */
		ret = mz0380_send_command(dev, mz0380_edid_opcode, params,
					  ARRAY_SIZE(params), NULL,
					  mz0380_edid_timeout_ms);
		if (ret && mz0380_edid_timeout_ms) {
			pr_warn("%s: EDID write failed at offset %u (%d)\n",
				dev->name, off, ret);
			return ret;
		}
		msleep(20);   /* bit-banged I2C + EEPROM page-write cycle */
	}

	pr_info("%s: EDID pushed (%u bytes, opcode 0x%02x, wait %ums) - the source is the oracle: a camera switches to HDMI out once it can read one\n",
		dev->name, MZ0380_EDID_SIZE, mz0380_edid_opcode,
		mz0380_edid_timeout_ms);
	return 0;
}

/*
 * Assert hot-plug detect. Two levers, both needed: the board-level HPD pin
 * (GPIO pin1) that the source physically senses, and the receiver's own HPD
 * enable (BANK0 0xb7 bit1, cleared to 0 by the init sequence above). Order
 * matters - HPD must rise only AFTER the EDID is readable, or the source will
 * read a garbage/absent EDID once and not retry.
 */
static void mz0380_mst3367_hpd(struct mz0380_dev *dev, bool on)
{
	mst_bank(dev, MST3367_BANK0);
	mst_wr(dev, MST3367_B0_HPD, on ? MST3367_B0_HPD_ON : MST3367_B0_HPD_OFF);
	mz0380_gpio_set(dev, MZ0380_GPIO_HPD, on);
	pr_info("%s: HPD %s\n", dev->name, on ? "asserted" : "deasserted");
}

/*
 * M46. Declare the input BEFORE touching the receiver. The Windows bring-up
 * order is AUTO.INPUT (input select) -> EDID -> HPD -> detect, and our
 * RE notes flag input-select as the prime suspect for the "receiver domain is
 * gated until Windows does something first" behaviour. Everything we have
 * done so far skipped straight to the receiver, so the card was never told
 * which front-end to route - which fits a receiver that answers I2C happily
 * but never sees a signal, and a detect block frozen at its idle values.
 */
static void mz0380_mst3367_select_input(struct mz0380_dev *dev)
{
	u32 params[8] = { 0 };
	int ret;

	/* cmd[6] = input code (HDMI), cmd[5] = fps, cmd[8..11] = geometry */
	params[0] = ((u32)MZ0380_INPUT_CODE_HDMI << 16) | (60u << 8);
	params[1] = (1080u << 16) | 1920u;

	ret = mz0380_send_command(dev, MZ0380_CMD_SET_VIC_PARAMS, params,
				  ARRAY_SIZE(params), NULL, 500);
	pr_info("%s: input select: SET_VIC(input=HDMI 1920x1080@60) ret=%d\n",
		dev->name, ret);
}

/*
 * M47: re-push the EDID and re-pulse HPD without reloading the module, so a
 * sweep over edid_opcode / edid_timeout_ms can be watched against the source
 * live. HPD has to fall and rise again: a source only re-reads the EDID on a
 * hotplug edge.
 */
/*
 * M48. Isolate HPD. Every test so far changed several things at once (input
 * select, receiver init, EDID push, HPD), and the source reacted exactly once
 * - which is as consistent with a coincidence (camera idle-timeout) as with
 * our hotplug edge. A source's response to HPD is the one thing we can test
 * with a single variable: pulse it, change nothing else, and see whether the
 * camera reacts every time.
 *
 * If it does, the board-level hotplug line really is ours to drive and the
 * EDID is being served by something we do not control (the factory-programmed
 * EDID MCU the RE docs mention) - which would make the remaining fault the
 * receiver's own configuration. If it never reacts, our HPD is not reaching
 * the connector at all and no amount of EDID work will ever matter.
 */
/*
 * M49. HPD is now PROVEN to reach the connector: the camera reacts to the
 * hotplug edge, then falls back to its LCD instead of staying in HDMI output
 * - which is exactly what a source does when it sees a sink appear, tries to
 * read the EDID over DDC, and gets nothing. So the EDID is the single
 * remaining fault, and it must be served out of the receiver (nothing else
 * exists on the bus to serve it).
 *
 * Find the EDID RAM by writability. A config register mostly has reserved or
 * hardwired bits and will not return an arbitrary byte; a RAM cell returns
 * exactly what was written. Sweep every bank, read the original, write a test
 * value, read back, and put the original back immediately. A contiguous run
 * of registers that hold arbitrary values is the EDID window.
 *
 * Original values are restored register-by-register, so the chip is left as
 * it was found; the receiver is re-initialised on every bring-up anyway.
 */
int mz0380_mst3367_wscan(struct mz0380_dev *dev)
{
	static const u8 test1 = 0x5a, test2 = 0xa5;
	unsigned int bank;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;

	pr_info("%s: writability scan (finding RAM windows; '#' = holds an arbitrary byte)\n",
		dev->name);

	for (bank = 0; bank <= 3; bank++) {
		bool ram[256] = { false };
		unsigned int reg, run_start = 0;
		unsigned int hits = 0, best = 0, best_start = 0;

		if (mst_bank(dev, bank))
			continue;

		/*
		 * Scan the WHOLE file, 0x01..0xff. The first pass stopped at
		 * 0x80 and bank3's writable run was still going at the edge -
		 * if it continues to 0xff that is a 128-byte window, exactly
		 * one EDID block. Reg 0x00 is the bank select: never written.
		 */
		for (reg = 1; reg <= 0xff; reg++) {
			u8 orig = 0, rb1 = 0, rb2 = 0;

			if (mst_rd(dev, reg, &orig))
				continue;
			if (mst_wr(dev, reg, test1) ||
			    mst_rd(dev, reg, &rb1) ||
			    mst_wr(dev, reg, test2) ||
			    mst_rd(dev, reg, &rb2)) {
				mst_wr(dev, reg, orig);
				continue;
			}
			mst_wr(dev, reg, orig);      /* restore immediately */

			if (rb1 == test1 && rb2 == test2) {
				ram[reg] = true;
				hits++;
			}
		}

		/* report contiguous runs - a RAM window is one long run */
		for (reg = 1; reg <= 0x100; reg++) {
			bool is_ram = (reg <= 0xff) && ram[reg];

			if (is_ram && !run_start) {
				run_start = reg;
			} else if (!is_ram && run_start) {
				unsigned int len = reg - run_start;

				if (len >= 8)
					pr_info("%s: bank%u run 0x%02x-0x%02x (%u bytes)\n",
						dev->name, bank, run_start,
						reg - 1, len);
				if (len > best) {
					best = len;
					best_start = run_start;
				}
				run_start = 0;
			}
		}
		pr_info("%s: bank%u: %u writable, longest run %u bytes at 0x%02x%s\n",
			dev->name, bank, hits, best, best_start,
			best >= 128 ? "  <- EDID-SIZED WINDOW" : "");
	}

	mst_bank(dev, MST3367_BANK0);
	pr_info("%s: writability scan done - a long run of '#' is the EDID RAM candidate\n",
		dev->name);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_wscan);

int mz0380_mst3367_hpd_pulse(struct mz0380_dev *dev, unsigned int count,
			     unsigned int gap_ms)
{
	unsigned int i;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;

	for (i = 0; i < count; i++) {
		pr_info("%s: HPD pulse %u/%u: deassert\n",
			dev->name, i + 1, count);
		mz0380_mst3367_hpd(dev, false);
		msleep(gap_ms);
		pr_info("%s: HPD pulse %u/%u: assert - watch the source now\n",
			dev->name, i + 1, count);
		mz0380_mst3367_hpd(dev, true);
		msleep(gap_ms);
	}
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_hpd_pulse);

int mz0380_mst3367_reload_edid(struct mz0380_dev *dev)
{
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		return ret;
	}

	mz0380_mst3367_hpd(dev, false);
	msleep(200);
	ret = mz0380_mst3367_load_edid(dev);
	msleep(100);
	mz0380_mst3367_hpd(dev, true);
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_reload_edid);

int mz0380_mst3367_bringup(struct mz0380_dev *dev)
{
	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_warn("%s: MST3367 bring-up skipped - firmware not ready\n",
			dev->name);
		return -ENODEV;
	}

	mz0380_mst3367_select_input(dev);
	mz0380_mst3367_reset(dev);
	mz0380_mst3367_init_regs(dev);

	/*
	 * Present the card as a sink: EDID first, then HPD. Toggle HPD low
	 * across the load so a source that was already attached re-reads the
	 * EDID instead of keeping whatever it saw before.
	 */
	mz0380_mst3367_hpd(dev, false);
	if (mz0380_mst3367_load_edid(dev) == 0) {
		msleep(100);            /* let the source settle before the edge */
		mz0380_mst3367_hpd(dev, true);
	} else {
		pr_warn("%s: continuing without EDID - the source will stay dark\n",
			dev->name);
	}

	dev->mst3367_ready = true;
	pr_info("%s: MST3367 receiver brought up (reset released, init applied)\n",
		dev->name);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_bringup);

/* --- sink-chain diagnostics ----------------------------------------------- */

/*
 * M43. "EDID loaded" only means the mailbox accepted the command: the card's
 * firmware forces the I2C result byte to 0 on a NAK, so a write that never
 * reached a chip is indistinguishable from a good one at the command layer.
 * Everything in the sink chain therefore has to be read BACK.
 *
 * The EDID read-back is the sharpest probe available: bytes 1..6 of a valid
 * EDID are 0xff, a value the firmware cannot manufacture on failure (it
 * forces 0x00). So "header reads ff" proves the DDC EEPROM really holds our
 * blob; "all zero" proves the writes went nowhere.
 */
static int mz0380_gpio_get(struct mz0380_dev *dev, unsigned int pin, u32 *out)
{
	u32 params[1] = { 1u << pin };
	int ret = mz0380_send_command(dev, MZ0380_CMD_GPIO_READ, params, 1,
				      NULL, 500);

	if (ret)
		return ret;
	/* op 0x14 returns the sampled bitmap in PARAM2 (BAR0+0x0c) */
	*out = !!(mz_mmio_read(dev, MZ0380_MB_PARAM(2)) & (1u << pin));
	return 0;
}

/*
 * M44. Is BANK3 writable RAM (i.e. the EDID store) or just an empty register
 * shadow? The bank sweep showed banks 0/1/2 full of live config while bank 3
 * reads all zeros except reg 0x00, which is the bank-select echo - exactly
 * what an EDID RAM that nobody has loaded would look like.
 *
 * Write a walking pattern into a few bank-3 registers, read it back, then put
 * the zeros back. Registers that return the pattern are RAM; registers that
 * read back 0 are unimplemented. Reg 0x00 is never touched - it is the bank
 * select on every bank.
 */
int mz0380_mst3367_ramtest(struct mz0380_dev *dev)
{
	static const u8 pattern[] = { 0xa5, 0x5a, 0x00, 0xff, 0x12, 0x34,
				      0x56, 0x78 };
	unsigned int i;
	unsigned int hits = 0;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;

	ret = mst_bank(dev, MST3367_BANK3);
	if (ret)
		return ret;

	for (i = 0; i < ARRAY_SIZE(pattern); i++)
		mst_wr(dev, 0x10 + i, pattern[i]);

	pr_info("%s: BANK3 RAM test, wrote a5 5a 00 ff 12 34 56 78 at 0x10:\n",
		dev->name);
	for (i = 0; i < ARRAY_SIZE(pattern); i++) {
		u8 v = 0;

		if (mst_rd(dev, 0x10 + i, &v)) {
			pr_info("%s:   reg 0x%02x: read failed\n",
				dev->name, 0x10 + i);
			continue;
		}
		pr_info("%s:   reg 0x%02x: wrote %02x read %02x%s\n",
			dev->name, 0x10 + i, pattern[i], v,
			v == pattern[i] ? "  <- RAM" : "");
		if (v == pattern[i] && pattern[i])
			hits++;
	}

	/* restore */
	for (i = 0; i < ARRAY_SIZE(pattern); i++)
		mst_wr(dev, 0x10 + i, 0x00);
	mst_bank(dev, MST3367_BANK0);

	pr_info("%s: BANK3 RAM test: %u/%u registers held their value - %s\n",
		dev->name, hits, (unsigned int)ARRAY_SIZE(pattern) - 1,
		hits ? "writable, this is a RAM window (EDID candidate)"
		     : "not writable, bank3 is not the EDID store");
	return hits ? 0 : -ENODEV;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_ramtest);

/*
 * M45. The connected camera visibly reacts to running the bring-up (its live
 * preview stops), which is what a camera does when it detects an HDMI sink
 * and switches its output over. So the source IS responding to our HPD - the
 * question is whether it then transmits, and if it does, why the receiver
 * never reports lock.
 *
 * Watch the detect block live so a plug/unplug or camera power-cycle can be
 * correlated with what the chip sees. reg 0x55 is the status byte: bit2..5
 * (mask 0x3c) are the lock bits the GPL driver gates on, and we consistently
 * read 0x03 - bits the driver does not use, most plausibly 5V/clock presence.
 * If those low bits track the cable while the timing counters stay idle, the
 * source is attached but not transmitting; if the counters come alive, the
 * receiver is seeing TMDS and the fault is in its configuration.
 */
int mz0380_mst3367_watch(struct mz0380_dev *dev, unsigned int secs)
{
	unsigned long end;
	u8 last[8];
	bool first = true;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;
	if (!dev->mst3367_ready)
		mz0380_mst3367_bringup(dev);

	pr_info("%s: watching MST3367 detect for %us - plug/unplug or power-cycle the source now\n",
		dev->name, secs);
	end = jiffies + secs * HZ;

	while (time_before(jiffies, end)) {
		static const u8 regs[] = { 0x55, 0x5f, 0x57, 0x58, 0x59,
					   0x5a, 0x6a, 0x6b };
		u8 now[ARRAY_SIZE(regs)];
		unsigned int i;
		bool changed = false;

		if (mst_bank(dev, MST3367_BANK0))
			break;
		for (i = 0; i < ARRAY_SIZE(regs); i++) {
			now[i] = 0;
			mst_rd(dev, regs[i], &now[i]);
			if (first || now[i] != last[i])
				changed = true;
		}

		if (changed) {
			pr_info("%s: detect 55=%02x %s | 5f=%02x hper=%02x%02x vper=%02x%02x htot=%02x%02x\n",
				dev->name, now[0],
				(now[0] & MST3367_B0_DETECT_LOCK_MASK) ?
					"LOCKED" : "no-lock",
				now[1], now[2], now[3], now[4], now[5],
				now[6], now[7]);
			memcpy(last, now, sizeof(last));
			first = false;
		}
		msleep(250);
	}

	pr_info("%s: watch done\n", dev->name);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_watch);

void mz0380_mst3367_diag(struct mz0380_dev *dev, struct seq_file *m)
{
	static const struct { const char *name; unsigned int pin; } pins[] = {
		{ "hpd      ", MZ0380_GPIO_HPD },
		{ "rx_enable", MZ0380_GPIO_RX_ENABLE },
		{ "rx_strap ", MZ0380_GPIO_RX_STRAP },
		{ "rx_reset ", MZ0380_GPIO_RX_RESET },
	};
	unsigned int i;
	u8 v;

	seq_printf(m, "%s: fw %s, receiver %s\n", dev->name,
		   mz0380_fw_state_name(dev->fw_state),
		   dev->mst3367_ready ? "brought up" : "NOT brought up");

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		seq_puts(m, "  (firmware not ready - nothing else can be probed)\n");
		return;
	}

	seq_puts(m, "  GPIO read-back (op 0x14):\n");
	for (i = 0; i < ARRAY_SIZE(pins); i++) {
		u32 level = 0;
		int ret = mz0380_gpio_get(dev, pins[i].pin, &level);

		if (ret)
			seq_printf(m, "    %s pin%-2u : read failed (%d)\n",
				   pins[i].name, pins[i].pin, ret);
		else
			seq_printf(m, "    %s pin%-2u : %u\n",
				   pins[i].name, pins[i].pin, level);
	}

	seq_puts(m, "  EDID read-back from DDC 0xA0 (bytes 1..6 must be ff):\n    ");
	for (i = 0; i < 8; i++) {
		u32 b = 0;

		if (mz0380_periph_read(dev, MZ0380_EDID_I2C_DEV, i, &b))
			seq_puts(m, "?? ");
		else
			seq_printf(m, "%02x ", b & 0xff);
	}
	seq_puts(m, "\n");

	/*
	 * M43: the receiver answers at 0x9c but 0xA0 read back all zeros, so
	 * either nothing lives at the EDID address on this bus or the EEPROM
	 * is gated behind the receiver's DDC. Probe the addresses the Windows
	 * driver knows about: a non-zero byte proves a device is there,
	 * because the firmware forces 0x00 when nobody ACKs.
	 */
	/*
	 * Reading a single register is not enough to call an address dead:
	 * reg 0x00 on the main bank is the bank-select and legitimately reads
	 * back 0. Sample a few registers per address and treat "any non-zero"
	 * as present (the firmware forces 0x00 when nobody ACKs).
	 */
	seq_puts(m, "  I2C presence scan (regs 00/01/02/7f; any non-zero = present):\n");
	{
		/*
		 * 0xaa: the board also carries a small Nuvoton "EDID MCU" -
		 * the doc calls it "the EEPROM the HDMI source reads" - and
		 * our own RE noted an MCU passthrough slave at 0xaa. If the
		 * EDID lives there, that is what has to be programmed.
		 */
		static const u8 addrs[] = { 0x9c, 0xa0, 0xa2, 0xa4, 0xa6,
					    0xa8, 0xaa, 0x88, 0x98, 0x60,
					    0x90, 0x94, 0x74, 0xb0 };
		static const u8 probe[] = { 0x00, 0x01, 0x02, 0x7f };

		for (i = 0; i < ARRAY_SIZE(addrs); i++) {
			unsigned int j;
			bool any = false;

			seq_printf(m, "    %02x:", addrs[i]);
			for (j = 0; j < ARRAY_SIZE(probe); j++) {
				u32 b = 0;

				if (mz0380_periph_read(dev, addrs[i], probe[j],
						       &b)) {
					seq_puts(m, " ??");
					continue;
				}
				seq_printf(m, " %02x", b & 0xff);
				any |= !!(b & 0xff);
			}
			seq_puts(m, any ? "   <- present\n" : "\n");
		}
	}

	/*
	 * Hunt for the EDID. It is not at the DDC EEPROM address on this bus,
	 * so it is most likely served out of the receiver's internal EDID RAM.
	 * Dump the first 8 bytes from every address that answered: whichever
	 * window shows the EDID signature 00 ff ff ff ff ff ff 00 is the one
	 * to write the blob into.
	 */
	seq_puts(m, "  EDID signature hunt (looking for 00 ff ff ff ff ff ff 00):\n");
	{
		static const u8 addrs[] = { 0x98, 0xaa, 0xa8, 0xa2, 0x88,
					    0x90, 0x94, 0x60 };
		unsigned int j;

		for (i = 0; i < ARRAY_SIZE(addrs); i++) {
			seq_printf(m, "    %02x:", addrs[i]);
			for (j = 0; j < 8; j++) {
				u32 b = 0;

				if (mz0380_periph_read(dev, addrs[i], j, &b))
					seq_puts(m, " ??");
				else
					seq_printf(m, " %02x", b & 0xff);
			}
			seq_puts(m, "\n");
		}
	}

	/*
	 * The bus holds only 0x9c and 0x98 - no EEPROM, no MCU - so the EDID
	 * must live inside the receiver. The GPL reference says the chip has
	 * FOUR banks (0..3) with a 256-byte shadow each, and we have only ever
	 * used 0/1/2: bank 3 is unexplored and is the natural home for EDID
	 * RAM. Dump the head of every bank on both slaves and look for the
	 * signature.
	 */
	seq_puts(m, "  bank sweep, first 16 bytes of each bank (EDID RAM hunt):\n");
	{
		static const u8 slaves[] = { MST3367_I2C_DEV, 0x98 };
		unsigned int s, bank, j;

		for (s = 0; s < ARRAY_SIZE(slaves); s++) {
			for (bank = 0; bank <= 3; bank++) {
				if (mz0380_periph_write(dev, slaves[s],
							MST3367_REG_BANK_SELECT,
							bank))
					continue;
				seq_printf(m, "    %02x bank%u:", slaves[s],
					   bank);
				for (j = 0; j < 16; j++) {
					u32 b = 0;

					if (mz0380_periph_read(dev, slaves[s],
							       j, &b))
						seq_puts(m, " ??");
					else
						seq_printf(m, " %02x",
							   b & 0xff);
				}
				seq_puts(m, "\n");
			}
		}
		mst_bank(dev, MST3367_BANK0);   /* leave the chip on bank 0 */
	}

	seq_puts(m, "  MST3367 BANK0 regs (all-zero column = bus NAK, not real values):\n");
	if (mst_bank(dev, MST3367_BANK0)) {
		seq_puts(m, "    bank select failed - receiver I2C is dead\n");
		return;
	}
	{
		static const u8 regs[] = { 0x55, 0x5f, 0x6a, 0x6b, 0x59, 0x5a,
					   0xb0, 0xb7, 0xb8, 0x51 };

		seq_puts(m, "    ");
		for (i = 0; i < ARRAY_SIZE(regs); i++) {
			if (mst_rd(dev, regs[i], &v))
				seq_printf(m, "%02x=?? ", regs[i]);
			else
				seq_printf(m, "%02x=%02x ", regs[i], v);
		}
		seq_puts(m, "\n");
	}
	seq_puts(m, "  reg 0x55 & 0x3c == 0x3c means locked; 0x00 across the row means\n"
		    "  the receiver is not answering at all (check reset/power, not EDID).\n");
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_diag);

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

/* --- M51: bit-banged I2C on the card's spare GPIO pins -------------------- */

/*
 * The GPL hdcapm driver (same MST3367, sibling Elgato product) revealed the
 * EDID architecture these cards use: a plain I2C EEPROM wired to the HDMI
 * connector's DDC pins, sitting on a SECOND I2C bus ("bus#1, it has the
 * eeprom on it", dev 0xa2/0xa0) - NOT on the receiver's bus. The source
 * reads the EDID straight out of that chip; no software EDID serving at all.
 *
 * Our mailbox I2C proxy (op 0x1a/0x1b) only reaches the receiver bus (M43:
 * 0x9c + 0x98, nothing else), which would explain every negative EDID sweep:
 * wrong bus. But the card also has host-controllable GPIO (op 0x14/0x15/0x17
 * = read/set/direction), and NEXT_SESSION notes GPIO12/13 as the internal
 * I2C pair. So: bit-bang I2C on those pins from the host, one mailbox
 * command per line transition. Slow (~2 ms per command) but an ACK scan is
 * seconds and a full 256-byte EDID burn is minutes - and the EEPROM keeps
 * the data across power cycles, so the burn is one-time.
 *
 * Open-drain emulation: a line is released high by switching the pin to
 * INPUT (external pull-ups float it up) and driven low by data=0 + OUTPUT.
 * GPIO_DIR (op 0x17) is assumed {mask, data} with data bit 1 = output -
 * unverified on hardware; if a scan finds EVERY address ACKing or the bus
 * reads stuck low, the polarity or pin numbers are wrong, and the scan says
 * so instead of reporting garbage.
 */

static int mz0380_bb_dir(struct mz0380_dev *dev, u8 pin, bool output)
{
	u32 params[2] = { 1u << pin, output ? (1u << pin) : 0 };

	return mz0380_send_command(dev, MZ0380_CMD_GPIO_DIR, params, 2,
				   NULL, 500);
}

/* release = input, pull-up floats the line high */
static int mz0380_bb_release(struct mz0380_dev *dev, u8 pin)
{
	return mz0380_bb_dir(dev, pin, false);
}

static int mz0380_bb_drive_low(struct mz0380_dev *dev, u8 pin)
{
	u32 params[2] = { 1u << pin, 0 };
	int ret = mz0380_send_command(dev, MZ0380_CMD_GPIO_SET, params, 2,
				      NULL, 500);

	if (ret)
		return ret;
	return mz0380_bb_dir(dev, pin, true);
}

static int mz0380_bb_read(struct mz0380_dev *dev, u8 pin, u8 *val)
{
	u32 params[1] = { 1u << pin };
	int ret = mz0380_send_command(dev, MZ0380_CMD_GPIO_READ, params, 1,
				      NULL, 500);

	if (ret)
		return ret;
	*val = !!(mz_mmio_read(dev, MZ0380_MB_PARAM(2)) & (1u << pin));
	return 0;
}

struct mz0380_bb_bus {
	struct mz0380_dev *dev;
	u8 sda;
	u8 scl;
};

static int mz0380_bb_start(struct mz0380_bb_bus *b)
{
	int ret;

	/* both released -> SDA low -> SCL low */
	ret = mz0380_bb_release(b->dev, b->sda);
	ret = ret ?: mz0380_bb_release(b->dev, b->scl);
	ret = ret ?: mz0380_bb_drive_low(b->dev, b->sda);
	ret = ret ?: mz0380_bb_drive_low(b->dev, b->scl);
	return ret;
}

static int mz0380_bb_stop(struct mz0380_bb_bus *b)
{
	int ret;

	ret = mz0380_bb_drive_low(b->dev, b->sda);
	ret = ret ?: mz0380_bb_release(b->dev, b->scl);
	ret = ret ?: mz0380_bb_release(b->dev, b->sda);
	return ret;
}

static int mz0380_bb_write_bit(struct mz0380_bb_bus *b, bool bit)
{
	int ret;

	ret = bit ? mz0380_bb_release(b->dev, b->sda)
		  : mz0380_bb_drive_low(b->dev, b->sda);
	ret = ret ?: mz0380_bb_release(b->dev, b->scl);   /* clock high */
	ret = ret ?: mz0380_bb_drive_low(b->dev, b->scl); /* clock low  */
	return ret;
}

static int mz0380_bb_read_bit(struct mz0380_bb_bus *b, u8 *bit)
{
	int ret;

	ret = mz0380_bb_release(b->dev, b->sda);
	ret = ret ?: mz0380_bb_release(b->dev, b->scl);
	ret = ret ?: mz0380_bb_read(b->dev, b->sda, bit);
	ret = ret ?: mz0380_bb_drive_low(b->dev, b->scl);
	return ret;
}

/* returns 0 on ACK, 1 on NAK, negative on mailbox error */
static int mz0380_bb_write_byte(struct mz0380_bb_bus *b, u8 byte)
{
	u8 ack;
	int i, ret;

	for (i = 7; i >= 0; i--) {
		ret = mz0380_bb_write_bit(b, (byte >> i) & 1);
		if (ret)
			return ret;
	}
	ret = mz0380_bb_read_bit(b, &ack);
	if (ret)
		return ret;
	return ack;   /* SDA low during 9th clock = ACK(0) */
}

static int mz0380_bb_read_byte(struct mz0380_bb_bus *b, u8 *byte, bool ack)
{
	u8 bit;
	int i, ret;

	*byte = 0;
	for (i = 7; i >= 0; i--) {
		ret = mz0380_bb_read_bit(b, &bit);
		if (ret)
			return ret;
		*byte |= (u8)bit << i;
	}
	return mz0380_bb_write_bit(b, !ack);   /* ACK = drive low */
}

int mz0380_i2cbb_scan(struct mz0380_dev *dev, u8 sda, u8 scl)
{
	struct mz0380_bb_bus b = { .dev = dev, .sda = sda, .scl = scl };
	unsigned int addr, acks = 0;
	u8 v;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;

	/* idle-state sanity: both lines must float high or there is no bus */
	ret = mz0380_bb_release(dev, sda);
	ret = ret ?: mz0380_bb_release(dev, scl);
	ret = ret ?: mz0380_bb_read(dev, sda, &v);
	if (ret) {
		pr_info("%s: i2cbb scan sda=%u scl=%u: mailbox error %d\n",
			dev->name, sda, scl, ret);
		return ret;
	}
	if (!v) {
		pr_info("%s: i2cbb scan sda=%u scl=%u: SDA reads LOW when released - wrong pin, wrong DIR polarity, or no pull-up; aborting\n",
			dev->name, sda, scl);
		return -EIO;
	}

	pr_info("%s: i2cbb scan start (sda=%u scl=%u)\n", dev->name, sda, scl);
	for (addr = 0x08; addr < 0x78; addr++) {
		ret = mz0380_bb_start(&b);
		ret = ret < 0 ? ret : mz0380_bb_write_byte(&b, addr << 1);
		if (ret >= 0)
			mz0380_bb_stop(&b);
		if (ret < 0) {
			pr_info("%s: i2cbb scan aborted at 0x%02x (%d)\n",
				dev->name, addr, ret);
			return ret;
		}
		if (ret == 0) {
			acks++;
			pr_info("%s: i2cbb ACK at 0x%02x (8-bit 0x%02x)\n",
				dev->name, addr, addr << 1);
		}
	}

	if (acks > 16)
		pr_info("%s: i2cbb scan: %u ACKs - that is a stuck bus, not real devices (check pins/polarity)\n",
			dev->name, acks);
	else
		pr_info("%s: i2cbb scan done: %u device(s)\n", dev->name, acks);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_i2cbb_scan);

/*
 * Burn the validated EDID into the DDC EEPROM found by the scan, 8-byte
 * pages (safe for 24C02..24C16 parts), ack-poll between pages, then read
 * everything back and compare. One-time operation: the EEPROM is
 * non-volatile, so a verified burn permanently un-blocks the source's EDID
 * read - no Windows trace needed.
 */
int mz0380_i2cbb_edid_burn(struct mz0380_dev *dev, u8 sda, u8 scl, u8 addr7)
{
	struct mz0380_bb_bus b = { .dev = dev, .sda = sda, .scl = scl };
	unsigned int off, i, poll;
	u8 rd;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;

	pr_info("%s: i2cbb EDID burn -> dev 0x%02x (sda=%u scl=%u), %u bytes\n",
		dev->name, addr7, sda, scl, MZ0380_EDID_SIZE);

	for (off = 0; off < MZ0380_EDID_SIZE; off += 8) {
		ret = mz0380_bb_start(&b);
		ret = ret < 0 ? ret : mz0380_bb_write_byte(&b, addr7 << 1);
		if (ret > 0) {
			pr_info("%s: i2cbb burn: NAK on address at off %u\n",
				dev->name, off);
			mz0380_bb_stop(&b);
			return -ENXIO;
		}
		ret = ret < 0 ? ret : mz0380_bb_write_byte(&b, off);
		for (i = 0; !ret && i < 8; i++)
			ret = mz0380_bb_write_byte(&b,
						   mz0380_edid_default[off + i]);
		if (ret >= 0)
			mz0380_bb_stop(&b);
		if (ret) {
			pr_info("%s: i2cbb burn failed at off %u (%d)\n",
				dev->name, off, ret);
			return ret < 0 ? ret : -EIO;
		}

		/* ack-poll until the internal page write completes */
		for (poll = 0; poll < 20; poll++) {
			ret = mz0380_bb_start(&b);
			ret = ret < 0 ? ret : mz0380_bb_write_byte(&b, addr7 << 1);
			if (ret >= 0)
				mz0380_bb_stop(&b);
			if (ret <= 0)
				break;
		}
		if (ret)
			pr_info("%s: i2cbb burn: ack-poll never ACKed after page %u\n",
				dev->name, off / 8);
	}

	/* verify: sequential read of the whole array */
	ret = mz0380_bb_start(&b);
	ret = ret < 0 ? ret : mz0380_bb_write_byte(&b, addr7 << 1);
	ret = ret < 0 ? ret : mz0380_bb_write_byte(&b, 0);
	ret = ret < 0 ? ret : mz0380_bb_start(&b);   /* repeated start */
	ret = ret < 0 ? ret : mz0380_bb_write_byte(&b, (addr7 << 1) | 1);
	if (ret) {
		pr_info("%s: i2cbb verify setup failed (%d)\n", dev->name, ret);
		return ret < 0 ? ret : -EIO;
	}
	for (off = 0; off < MZ0380_EDID_SIZE; off++) {
		ret = mz0380_bb_read_byte(&b, &rd,
					  off != MZ0380_EDID_SIZE - 1);
		if (ret < 0)
			return ret;
		if (rd != mz0380_edid_default[off]) {
			pr_info("%s: i2cbb VERIFY MISMATCH at %u: wrote 0x%02x read 0x%02x\n",
				dev->name, off, mz0380_edid_default[off], rd);
			mz0380_bb_stop(&b);
			return -EIO;
		}
	}
	mz0380_bb_stop(&b);

	pr_info("%s: i2cbb EDID burn VERIFIED - %u bytes match; pulse HPD and the source should now read a valid EDID\n",
		dev->name, MZ0380_EDID_SIZE);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_i2cbb_edid_burn);
