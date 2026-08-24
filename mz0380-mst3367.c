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
#include <linux/mutex.h>
#include <linux/v4l2-dv-timings.h>
#include <media/v4l2-dv-timings.h>

#include "mz0380.h"
#include "mz0380-reg.h"
#include "mz0380-edid.h"

/*
 * The MST3367 bank selector is global chip state, while each mailbox command
 * is only serialized individually.  Keep bank-select + register batches
 * atomic with respect to every receiver operation in this file.  This is a
 * global lock because the device structure predates the receiver support;
 * HD60 Pro systems normally contain one such card.
 */
static DEFINE_MUTEX(mst3367_lock);

static bool mst3367_status_locked(u8 status)
{
	/*
	 * M67: gate on the core bits (0x1c), not the full 0x3c. Bit 0x20 is
	 * not required for the timing block to be valid - see the evidence
	 * table at MST3367_B0_DETECT_LOCK_CORE - and it toggles fast enough to
	 * be set when the poll loop reads R55 and clear ~5ms later when
	 * measure_once() re-reads it, which is exactly how three runs died
	 * with "no full lock at start of pass" while R55 read 0x5f.
	 *
	 * Sample quality is not this predicate's job anyway: the snapshot
	 * re-read, the vtotal delta and the line-count cross-check decide
	 * whether a measurement is trustworthy.
	 */
	return (status & MST3367_B0_DETECT_LOCK_CORE) ==
		MST3367_B0_DETECT_LOCK_CORE;
}

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

/*
 * Is the receiver answering at all? The firmware turns an I2C NAK into a
 * 0x00 result byte, so "every register reads zero" is indistinguishable
 * from a chip that is powered down, held in reset, or absent - and quite
 * distinguishable from a live one, which always has some non-zero config.
 * Sampled across bank0 so one zeroed register cannot decide it.
 */
static int mst_bus_alive(struct mz0380_dev *dev)
{
	unsigned int reg;
	int ret;

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	for (reg = 0x01; reg <= 0xf1; reg += 0x10) {
		u8 v = 0;

		ret = mst_rd(dev, reg, &v);
		if (ret)
			return ret;
		if (v)
			return 1;
	}
	return 0;
}

/* --- GPIO (op 0x15, single-pin mask+data; see reg.h GPIO pin map) --------- */

static int mz0380_gpio_set(struct mz0380_dev *dev, unsigned int pin, int level)
{
	u32 params[2] = { 1u << pin, (u32)(!!level) << pin };

	return mz0380_send_command(dev, MZ0380_CMD_GPIO_SET, params, 2,
				   NULL, 500);
}

/*
 * Force a pin back to OUTPUT (op 0x17). The card powers up with these pins
 * already outputs, so bring-up historically only ever wrote DATA - but the
 * M51 bit-bang probe deliberately switched pins to input to emulate
 * open-drain, and left them that way. A GPIO_SET on an input pin changes
 * nothing, so the reset line (pin9) could no longer be driven and the
 * receiver stayed in reset with a dead I2C bus, surviving every rmmod.
 *
 * @invert selects the data-bit sense, which is still unproven; bring-up
 * tries both and keeps whichever revives the bus.
 */
static int mz0380_gpio_force_output(struct mz0380_dev *dev, unsigned int pin,
				    bool invert)
{
	u32 params[2] = { 1u << pin, invert ? 0 : (1u << pin) };

	return mz0380_send_command(dev, MZ0380_CMD_GPIO_DIR, params, 2,
				   NULL, 500);
}

/*
 * Release the MST3367 from reset.  The HD60 Pro Windows routine
 * FUN_14024dc28 drives pin9 1->0->1 with 50 ms at every phase, then drives
 * pin8 low and waits another 50 ms.  The old sibling-board sequence used
 * 2/5/10 ms delays and put pin8 back high; that is not the sequence shipped
 * for this PCI ID and can leave the receiver's acquisition state marginal.
 * Keep pin3 enabled around the device-specific reset because it is the board
 * RX/mux gate discovered during the original hardware bring-up.
 */
static int mz0380_mst3367_reset(struct mz0380_dev *dev)
{
	static const u8 driven[] = {
		MZ0380_GPIO_HPD, MZ0380_GPIO_RX_ENABLE,
		MZ0380_GPIO_RX_STRAP, MZ0380_GPIO_RX_RESET,
	};
	unsigned int i;
	int ret;

	/* every pin we are about to drive must actually be an output */
	for (i = 0; i < ARRAY_SIZE(driven); i++) {
		ret = mz0380_gpio_force_output(dev, driven[i],
						mz0380_gpio_dir_invert);
		if (ret)
			return ret;
	}

	ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_ENABLE, 1);
	if (ret)
		return ret;
	ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_RESET, 1);
	if (ret)
		return ret;
	msleep(50);
	ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_RESET, 0);
	if (ret)
		return ret;
	msleep(50);
	ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_RESET, 1);
	if (ret)
		return ret;
	msleep(50);
	/*
	 * M109: pin 8 is the companion reset/power strap and this line has
	 * driven it LOW since the original bring-up, never varied. Pin 9 next
	 * door is active-low and we release it high; if pin 8 shares that
	 * convention, LOW holds the companion in reset all session - which
	 * would explain the dead HDMI passthrough output.
	 */
	if (mz0380_rx_strap != MZ0380_RX_STRAP_LEAVE) {
		ret = mz0380_gpio_set(dev, MZ0380_GPIO_RX_STRAP,
				      mz0380_rx_strap ? 1 : 0);
		if (ret)
			return ret;
		msleep(50);
	}
	return 0;
}

/*
 * MST3367 init_setup (hdcapm mst3367_init_setup, cross-validated by our disasm).
 * Runs after the reset release; configures HDMI RX path, HDCP receive, YUV422
 * 8-bit output, then a HDMI + HDCP block reset.
 */
/* read-modify-write helpers, matching hdcapm's mst3367_set / mst3367_clr */
static int mst_set(struct mz0380_dev *dev, u8 reg, u8 mask)
{
	u8 v = 0;
	int ret;

	ret = mst_rd(dev, reg, &v);
	if (ret)
		return ret;
	return mst_wr(dev, reg, v | mask);
}

static int mst_clr(struct mz0380_dev *dev, u8 reg, u8 mask)
{
	u8 v = 0;
	int ret;

	ret = mst_rd(dev, reg, &v);
	if (ret)
		return ret;
	return mst_wr(dev, reg, v & ~mask);
}

static int mst3367_hdcp_reset(struct mz0380_dev *dev)
{
	int ret;

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	ret = mst_wr(dev, 0xb8, 0x10);
	if (ret)
		return ret;
	ret = mst_wr(dev, 0xb8, 0x00);
	if (ret)
		return ret;
	msleep(20);
	return 0;
}

static int mst3367_hdmi_reset(struct mz0380_dev *dev)
{
	int ret;

	ret = mst_bank(dev, MST3367_BANK2);
	if (ret)
		return ret;
	ret = mst_wr(dev, 0x07, 0xf4);
	if (ret)
		return ret;
	ret = mst_wr(dev, 0x07, 0x04);
	if (ret)
		return ret;
	msleep(20);
	return 0;
}

/*
 * MST3367 init - a faithful port of hdcapm's mst3367_init_setup() plus the
 * RxHdmiInit block it ends with (GPL, same receiver, RE'd from the Windows
 * driver for a sibling board).
 *
 * M56: our previous version was a small subset of this and, on hardware,
 * the receiver would see a source's clock (detect bit 0x80, horizontal
 * period measured) but never reach frame lock (0x55 & 0x3c stayed clear,
 * the vertical-period counter sat saturated at 0x1fff). The blocks missing
 * here are exactly the ones that would explain that: RxTmdsInit programs
 * the TMDS equaliser/PLL and RxVideoInit the filter and sync handling.
 * Ordering is hdcapm's: HPD stays off across the whole sequence and only
 * rises at the end, after the HDCP and HDMI blocks are reset. Auto-position
 * remains enabled for acquisition and is disabled only after mode recognition.
 */
#define MST3367_TRY(_op) do { \
		ret = (_op); \
		if (ret) \
			return ret; \
	} while (0)

static int mz0380_mst3367_init_regs(struct mz0380_dev *dev)
{
	/* CSC coefficients, written as a block at BANK0 0x92.. (hdcapm) */
	static const u8 csctbl[] = {
		0x40,
		0x08, 0x02, 0x03, 0x65, 0x7E, 0x28, /* M11, M12, M13 */
		0x78, 0xB9, 0x0B, 0x65, 0x79, 0xD6, /* M21, M22, M23 */
		0x7F, 0x45, 0x01, 0x27, 0x08, 0x02, /* M31, M32, M33 */
		0x20, 0x00, 0x02, 0x81, 0x20, 0x01, /*  A1,  A2,  A3 */
		0x15, 0x95, 0x05, 0x20, 0xC0, 0x08
	};
	unsigned int i;
	int ret;

	/* HPD off for the whole configuration */
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_wr(dev, MST3367_B0_HPD, MST3367_B0_HPD_OFF));

	/*
	 * HD60 Pro FUN_14024dc28 preamble.  These fixed writes precede the
	 * receiver's TMDS setup in the shipping driver; the older sibling-board
	 * sequence omitted them even though 0x64..0x67 sit in the clock/sync
	 * acquisition block.
	 */
	MST3367_TRY(mst_wr(dev, 0x13, 0x08));
	/*
	 * FUN_14024eeb8(0): park HPD DEASSERTED during configuration. M69: the
	 * pin is active-low (pin1 = ~(arg>>4)&1 in the Windows driver), so
	 * deasserted = pin HIGH. We wrote 0 here for the entire project, which
	 * ASSERTED hot-plug while the receiver was half-configured and while
	 * the EDID write was in flight - inviting the source to master the
	 * DDC bus under our EEPROM writes.
	 */
	MST3367_TRY(mz0380_gpio_set(dev, MZ0380_GPIO_HPD, 1));

	/* RxGeneralInit */
	MST3367_TRY(mst_wr(dev, 0x41, 0x6f));
	MST3367_TRY(mst_wr(dev, 0xb8, 0x00));
	MST3367_TRY(mst_wr(dev, 0x64, 0x02));
	MST3367_TRY(mst_wr(dev, 0x65, 0xff));
	MST3367_TRY(mst_wr(dev, 0x66, 0x00));
	MST3367_TRY(mst_wr(dev, 0x67, 0x02));

	/* RxTmdsInit - equaliser / PLL; absent from our old sequence */
	MST3367_TRY(mst_bank(dev, MST3367_BANK1));
	MST3367_TRY(mst_wr(dev, 0x0f, 0x02));
	MST3367_TRY(mst_wr(dev, 0x16, 0x30));
	MST3367_TRY(mst_wr(dev, 0x17, 0x00));
	MST3367_TRY(mst_wr(dev, 0x18, 0x00));
	MST3367_TRY(mst_wr(dev, 0x19, 0x00));
	MST3367_TRY(mst_wr(dev, 0x1a, 0x50));
	MST3367_TRY(mst_clr(dev, 0x2a, 0x07));
	MST3367_TRY(mst_set(dev, 0x2a, 0x07));
	MST3367_TRY(mst_bank(dev, MST3367_BANK2));
	MST3367_TRY(mst_wr(dev, 0x08, 0x03));

	/* RxHdcpInit - receive HDCP */
	MST3367_TRY(mst_bank(dev, MST3367_BANK1));
	MST3367_TRY(mst_wr(dev, 0x24, 0x40));
	MST3367_TRY(mst_wr(dev, 0x30, 0x80));
	MST3367_TRY(mst_wr(dev, 0x31, 0x00));
	MST3367_TRY(mst_wr(dev, 0x32, 0x00));

	/* RxVideoInit */
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_wr(dev, 0xb0, 0x14));
	/*
	 * M101: this set does NOT stick. Every output diag since the readback
	 * was added reads 0xae = 0x20, bit2 clear, on runs where this line has
	 * executed - while 0xad and 0xb4, written by the same helper two lines
	 * below, both read back exactly as written. So the bus is fine and
	 * something specific to 0xae bit2 is going on: either it is write-only
	 * or self-clearing (a strobe), or the card's own firmware rewrites the
	 * register. Read it straight back here to tell those apart - if it
	 * reads 0x24 now and 0x20 later, something clears it; if it reads 0x20
	 * now, the write is simply rejected.
	 *
	 * Consequence either way: M99 scored "0xae |= 0x04 is neutral" on a
	 * write that was never in the register. That register is UNTESTED, not
	 * neutral.
	 */
	MST3367_TRY(mst_set(dev, 0xae, 0x04));
	{
		u8 ae_rb = 0;

		if (!mst_rd(dev, 0xae, &ae_rb))
			pr_info("%s: RxVideoInit: 0xae |= 0x04 -> reads back %02x%s\n",
				dev->name, ae_rb,
				(ae_rb & 0x04) ? "" : " (BIT2 DID NOT STICK)");
	}
	MST3367_TRY(mst_wr(dev, 0xad, 0x05)); /* enable low-pass filter */
	/* Exact HD60 Pro FUN_14024dc28 values (the sibling uses e0/08). */
	MST3367_TRY(mst_wr(dev, 0xb1, mz0380_mst_b1 & 0xff));
	MST3367_TRY(mst_wr(dev, 0xb2, mz0380_mst_b2 & 0xff));
	MST3367_TRY(mst_wr(dev, 0xb3, 0x00));
	MST3367_TRY(mst_wr(dev, 0xb4, 0x55));

	/* RxAudioInit */
	MST3367_TRY(mst_clr(dev, 0xb4, 0x03));
	MST3367_TRY(mst_bank(dev, MST3367_BANK2));
	MST3367_TRY(mst_wr(dev, 0x01, 0x61));
	MST3367_TRY(mst_wr(dev, 0x02, 0xf5));
	MST3367_TRY(mst_set(dev, 0x03, 0x02));
	MST3367_TRY(mst_wr(dev, 0x04, 0x01));
	MST3367_TRY(mst_wr(dev, 0x05, 0x00));
	MST3367_TRY(mst_wr(dev, 0x06, 0x08));
	MST3367_TRY(mst_wr(dev, 0x1c, 0x1a));
	MST3367_TRY(mst_wr(dev, 0x1d, 0x00));
	MST3367_TRY(mst_wr(dev, 0x1e, 0x00));
	MST3367_TRY(mst_wr(dev, 0x1f, 0x00));
	MST3367_TRY(mst_clr(dev, 0x25, 0xa2));
	MST3367_TRY(mst_set(dev, 0x25, 0xa2));

	MST3367_TRY(mst_set(dev, 0x02, 0x80));
	MST3367_TRY(mst_set(dev, 0x07, 0x04));
	MST3367_TRY(mst_wr(dev, 0x17, 0xc0));
	MST3367_TRY(mst_wr(dev, 0x19, 0xff));
	MST3367_TRY(mst_wr(dev, 0x1a, 0xff));
	MST3367_TRY(mst_wr(dev, 0x1b, 0xfc));
	MST3367_TRY(mst_wr(dev, 0x20, 0x00));
	MST3367_TRY(mst_clr(dev, 0x21, 0x03));
	MST3367_TRY(mst_wr(dev, 0x22, 0x26));
	MST3367_TRY(mst_wr(dev, 0x27, 0x00));
	MST3367_TRY(mst_set(dev, 0x2e, 0xa1));

	/* colour range */
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_wr(dev, 0xab, 0x15));
	MST3367_TRY(mst_clr(dev, 0xac, 0x3f));
	MST3367_TRY(mst_set(dev, 0xac, 0x15));

	/* RxSwitchSource - HDMI */
	MST3367_TRY(mst_wr(dev, MST3367_B0_HPD, MST3367_B0_HPD_OFF));
	MST3367_TRY(mst3367_hdcp_reset(dev));
	MST3367_TRY(mst3367_hdmi_reset(dev));
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_wr(dev, 0x51, 0x89));
	MST3367_TRY(mst_wr(dev, MST3367_B0_HPD, MST3367_B0_HPD_ON));

	/* Keep auto-position enabled until a coherent mode has been recognized. */
	MST3367_TRY(mst_wr(dev, MST3367_B0_AUTO_POSITION,
			       MST3367_B0_AUTO_POSITION_ON));
	MST3367_TRY(mst_wr(dev, 0x1e, 0x11));
	MST3367_TRY(mst_wr(dev, 0x1f, 0x01));
	MST3367_TRY(mst_wr(dev, 0x73, 0x90));
	MST3367_TRY(mst_wr(dev, 0xb5, mz0380_mst_b5 & 0xff));

	/* CSC */
	MST3367_TRY(mst_wr(dev, 0x90, 0x15));
	MST3367_TRY(mst_wr(dev, 0x91, 0x15));
	for (i = 0; i < ARRAY_SIZE(csctbl); i++) {
		/*
		 * M130: csctbl[0] lands on 0x92, the CSC control byte - the
		 * only entry that is not a coefficient. Overridable so a
		 * YUV444 source can try bypassing the conversion entirely.
		 * See mz0380_mst_csc_ctl in mz0380-core.c.
		 */
		u8 val = csctbl[i];

		if (!i && mz0380_mst_csc_ctl != MZ0380_MST_CSC_CTL_AUTO)
			val = mz0380_mst_csc_ctl & 0xff;

		MST3367_TRY(mst_wr(dev, 0x92 + i, val));
	}
	/*
	 * AUTO leaves hdcapm's byte here on purpose: this runs before HPD, so
	 * nothing is transmitting and the input colour space is not yet
	 * knowable. mz0380_mst3367_apply_csc_mode() re-applies it at stream
	 * start, once the receiver has locked.
	 */
	if (mz0380_mst_csc_ctl != MZ0380_MST_CSC_CTL_AUTO &&
	    (mz0380_mst_csc_ctl & 0xff) != csctbl[0])
		pr_info("%s: MST3367 CSC control 0x92 forced to 0x%02x (hdcapm default 0x%02x)\n",
			dev->name, mz0380_mst_csc_ctl & 0xff, csctbl[0]);

	/* YUV422, 8-bit, external sync */
	MST3367_TRY(mst_wr(dev, 0xb0, 0x20));

	/* RxHdmiInit */
	MST3367_TRY(mst_bank(dev, MST3367_BANK2));
	MST3367_TRY(mst_clr(dev, 0x01, 0xf0));
	MST3367_TRY(mst_set(dev, 0x01, 0x40 | 0x20));
	MST3367_TRY(mst_set(dev, 0x04, 0x01));
	MST3367_TRY(mst_wr(dev, 0x06, 0x08));
	MST3367_TRY(mst_set(dev, 0x09, 0x20));
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_clr(dev, 0x54, 0x10));
	MST3367_TRY(mst_set(dev, 0xac, 0x80));
	MST3367_TRY(mst_set(dev, 0x00, 0x80));
	MST3367_TRY(mst_set(dev, 0xce, 0x80));
	MST3367_TRY(mst_clr(dev, 0xcf, 0x07));
	MST3367_TRY(mst_set(dev, 0xcf, 0x02));
	MST3367_TRY(mst_clr(dev, 0x00, 0x80));

	/*
	 * Complete FUN_14024d2ec's HD60 Pro output-pin mapping.  Registry
	 * defaults are output-map index 1 and automatic mapping enabled, which
	 * select table value zero: BANK80:D0[1:0]=0 and CF[7]=0.
	 */
	MST3367_TRY(mst_bank(dev, 0x80));
	MST3367_TRY(mst_clr(dev, 0xd0, 0x03));
	MST3367_TRY(mst_clr(dev, 0xcf, 0x80));
	MST3367_TRY(mst_bank(dev, MST3367_BANK0));

	msleep(20);
	return 0;
}

/*
 * Load the EDID into the receiver's DDC EEPROM (I2C 0xA0) using the exact
 * Windows write_s packet: PARAM1=(len<<16)|(offset<<8)|dev8 followed by 32
 * data bytes in PARAM2..PARAM9.  This deliberately leaves PARAM10/STATUS free
 * so each chunk can complete synchronously before the mailbox is reused.
 */
/*
 * M69: read the EDID store back through opcode 0x1e - the block-read twin of
 * the 0x1f write. Unlike 0x1a, whose result the firmware forces to 0 on a
 * NAK, 0x1e leaves the host's payload bytes UNTOUCHED on failure - so poison
 * the buffer first and a NAK is unambiguous. Returns 0 with 32 bytes in *buf.
 */
static int mz0380_mst3367_read_edid_chunk(struct mz0380_dev *dev,
					  unsigned int off, u8 *buf)
{
	u32 params[1 + MZ0380_EDID_CHUNK / sizeof(u32)];
	unsigned int i;
	int ret;

	memset(params, 0xa5, sizeof(params));    /* poison: NAK leaves it */
	params[0] = MZ0380_EDID_I2C_DEV |
		    ((u32)off << 8) |
		    ((u32)MZ0380_EDID_CHUNK << 16);

	ret = mz0380_send_command(dev, MZ0380_CMD_I2C_READ_S, params,
				  ARRAY_SIZE(params), NULL, 500);
	if (ret)
		return ret;
	/* the reply payload lands back in the PARAM slots */
	for (i = 0; i < MZ0380_EDID_CHUNK / sizeof(u32); i++) {
		u32 v = mz_mmio_read(dev, MZ0380_MB_PARAM(1 + i));

		memcpy(buf + i * sizeof(u32), &v, sizeof(v));
	}
	return 0;
}

static int mz0380_mst3367_load_edid(struct mz0380_dev *dev)
{
	u8 verify[MZ0380_EDID_CHUNK];
	unsigned int off;
	int ret;

	/*
	 * M69: GPIO pin 2 is the DDC path mux, driven ONLY by the Windows
	 * EDID handler: 1 = the local EDID store is on the SoC's i2c-0
	 * (where our opcode 0x1f writes go), 0 = the passthrough side. Every
	 * previous EDID attempt ran with pin 2 in its power-on state - if
	 * that state is 0, our writes went to a bus with nothing on it,
	 * which is exactly the observed NAK-and-read-back-zero.
	 */
	ret = mz0380_gpio_force_output(dev, MZ0380_GPIO_EDID_MUX,
				       mz0380_gpio_dir_invert);
	if (!ret)
		ret = mz0380_gpio_set(dev, MZ0380_GPIO_EDID_MUX, 1);
	if (ret)
		pr_warn("%s: EDID mux (pin2) drive failed (%d) - writing with it as-is\n",
			dev->name, ret);

	for (off = 0; off < MZ0380_EDID_SIZE; off += MZ0380_EDID_CHUNK) {
		u32 params[1 + MZ0380_EDID_CHUNK / sizeof(u32)] = { 0 };

		params[0] = MZ0380_EDID_I2C_DEV |
			    ((u32)off << 8) |
			    ((u32)MZ0380_EDID_CHUNK << 16);
		memcpy(&params[1], &mz0380_edid_default[off],
		       MZ0380_EDID_CHUNK);

		if (!mz0380_edid_timeout_ms) {
			pr_warn("%s: refusing unsafe asynchronous EDID write at offset %u\n",
				dev->name, off);
			return -EINVAL;
		}
		ret = mz0380_send_command(dev, mz0380_edid_opcode, params,
					  ARRAY_SIZE(params), NULL,
					  mz0380_edid_timeout_ms);
		if (ret) {
			pr_warn("%s: EDID write failed at offset %u (%d)\n",
				dev->name, off, ret);
			/* leave the mux on the connector side on the way out */
			mz0380_gpio_set(dev, MZ0380_GPIO_EDID_MUX, 0);
			return ret;
		}
		msleep(20);   /* conservative EEPROM page-write cycle */
	}

	/*
	 * M69: verify with the clean NAK detector while the mux still points
	 * at the local store. Header bytes 1..6 of a valid EDID are 0xff - a
	 * value the poison (0xa5) cannot fake.
	 */
	memset(verify, 0, sizeof(verify));
	ret = mz0380_mst3367_read_edid_chunk(dev, 0, verify);
	if (!ret && verify[1] == 0xff && verify[2] == 0xff && verify[6] == 0xff)
		pr_info("%s: EDID READ-BACK OK (%02x %02x %02x %02x %02x %02x %02x %02x) - a real store holds our EDID\n",
			dev->name, verify[0], verify[1], verify[2], verify[3],
			verify[4], verify[5], verify[6], verify[7]);
	else
		pr_warn("%s: EDID read-back failed (ret=%d, first bytes %02x %02x %02x %02x) - nothing is holding the EDID at 0xa0\n",
			dev->name, ret, verify[0], verify[1], verify[2],
			verify[3]);

	/* hand the store back to the connector side, as Windows does */
	mz0380_gpio_set(dev, MZ0380_GPIO_EDID_MUX, 0);

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
static int mz0380_gpio_get(struct mz0380_dev *dev, unsigned int pin, u32 *out);

static int mz0380_mst3367_hpd(struct mz0380_dev *dev, bool on)
{
	u32 level = 0;
	u8 b7 = 0;
	bool pin_ok, receiver_ok;
	int ret;

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	ret = mst_wr(dev, MST3367_B0_HPD,
		     on ? MST3367_B0_HPD_ON : MST3367_B0_HPD_OFF);
	if (ret)
		return ret;

	/*
	 * Drive the board pin - and make sure it IS driven. A GPIO_SET on a
	 * pin left as an input silently does nothing (the M51 probe did
	 * exactly that to pin1), so the driver would claim HPD while the
	 * source saw no edge at all. Force the direction, then read the pin
	 * and the receiver's own HPD bit back and report what actually stuck.
	 */
	ret = mz0380_gpio_force_output(dev, MZ0380_GPIO_HPD,
				       mz0380_gpio_dir_invert);
	if (ret)
		return ret;
	/*
	 * M69: HPD is ACTIVE-LOW on this board (win64 FUN_14024eeb8:
	 * pin1 = ~(arg>>4)&1 - HPD_ON drives the pin LOW). Every earlier run
	 * asserted when it meant to release and vice versa, which is exactly
	 * the recorded behaviour: the camera reacted to our edge, then fell
	 * back to its LCD because our "asserted" steady state was, physically,
	 * no sink present.
	 */
	ret = mz0380_gpio_set(dev, MZ0380_GPIO_HPD, !on);
	if (ret)
		return ret;

	ret = mz0380_gpio_get(dev, MZ0380_GPIO_HPD, &level);
	if (ret)
		return ret;
	ret = mst_rd(dev, MST3367_B0_HPD, &b7);
	if (ret)
		return ret;

	pin_ok = !!level == !on;    /* active-low: asserted reads 0 */
	receiver_ok = (!(b7 & MST3367_B0_HPD_OFF)) == on;
	pr_info("%s: HPD %s -> pin%u reads %u, BANK0[0x%02x]=0x%02x%s%s\n",
		dev->name, on ? "asserted" : "deasserted", MZ0380_GPIO_HPD,
		level, MST3367_B0_HPD, b7,
		pin_ok ? "" : "  <- board HPD pin did not follow",
		receiver_ok ? "" : "  <- receiver HPD bit did not follow");

	/*
	 * M61: this read-back is DIAGNOSTIC, not a gate. Every command above
	 * already returned its own error; what is left is a sampled pin, and
	 * on this SoC a GPIO read returns the input register, which need not
	 * follow a pin we are driving as an output. Asserting HPD failed the
	 * check while deasserting passed it - a one-sided result that fits a
	 * read-back artefact, not a dead line. What actually proved HPD works
	 * is the source reacting to the edge (M48/M49), and failing bring-up
	 * here throws away the detect path over a sample we cannot trust.
	 */
	if (!pin_ok || !receiver_ok)
		pr_warn("%s: HPD read-back did not follow (pin_ok=%u receiver_ok=%u) - continuing; the edge is verified by the source's reaction, not by this sample\n",
			dev->name, pin_ok, receiver_ok);
	return 0;
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

	mutex_lock(&mst3367_lock);
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
	mutex_unlock(&mst3367_lock);
	pr_info("%s: writability scan done - a long run of '#' is the EDID RAM candidate\n",
		dev->name);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_wscan);

int mz0380_mst3367_hpd_pulse(struct mz0380_dev *dev, unsigned int count,
			     unsigned int gap_ms)
{
	unsigned int i;
	int ret = 0;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;

	mutex_lock(&mst3367_lock);
	for (i = 0; i < count; i++) {
		pr_info("%s: HPD pulse %u/%u: deassert\n",
			dev->name, i + 1, count);
		ret = mz0380_mst3367_hpd(dev, false);
		if (ret)
			break;
		msleep(gap_ms);
		pr_info("%s: HPD pulse %u/%u: assert - watch the source now\n",
			dev->name, i + 1, count);
		ret = mz0380_mst3367_hpd(dev, true);
		if (ret)
			break;
		msleep(gap_ms);
	}
	mutex_unlock(&mst3367_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_hpd_pulse);

int mz0380_mst3367_reload_edid(struct mz0380_dev *dev)
{
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		if (ret)
			return ret;
	}

	mutex_lock(&mst3367_lock);
	ret = mz0380_mst3367_hpd(dev, false);
	if (ret)
		goto out;
	msleep(200);
	ret = mz0380_mst3367_load_edid(dev);
	if (ret)
		goto out;
	msleep(100);
	ret = mz0380_mst3367_hpd(dev, true);
out:
	mutex_unlock(&mst3367_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_reload_edid);

int mz0380_mst3367_bringup(struct mz0380_dev *dev)
{
	int alive;
	int ret = 0;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_warn("%s: MST3367 bring-up skipped - firmware not ready\n",
			dev->name);
		return -ENODEV;
	}

	mutex_lock(&mst3367_lock);
	if (dev->mst3367_ready)
		goto out;
	dev->mst3367_ready = false;
	/* A physical reset invalidates the cached mode and requires the latch pulse. */
	dev->signal_locked = false;

	ret = mz0380_mst3367_reset(dev);
	if (ret)
		goto failed;

	/*
	 * The reset above forces the driven pins to outputs, but the DIR data
	 * polarity is a guess. If it was the wrong one we have just made them
	 * inputs, reset stays asserted and the bus is dead - which is exactly
	 * the state the M51 bit-bang probe left the card in. Detect that and
	 * retry with the opposite sense, then remember which one worked.
	 */
	alive = mst_bus_alive(dev);
	if (alive < 0) {
		ret = alive;
		goto failed;
	}
	if (!alive) {
		pr_info("%s: MST3367 silent after reset (dir_invert=%u) - retrying with the opposite GPIO_DIR sense\n",
			dev->name, mz0380_gpio_dir_invert);
		mz0380_gpio_dir_invert = !mz0380_gpio_dir_invert;
		ret = mz0380_mst3367_reset(dev);
		if (ret)
			goto failed;

		alive = mst_bus_alive(dev);
		if (alive < 0) {
			ret = alive;
			goto failed;
		}
		if (alive) {
			pr_info("%s: MST3367 answering with dir_invert=%u - GPIO direction polarity now known\n",
				dev->name, mz0380_gpio_dir_invert);
		} else {
			mz0380_gpio_dir_invert = !mz0380_gpio_dir_invert;
			pr_warn("%s: MST3367 still silent under both GPIO_DIR senses - receiver held in reset or unpowered; a mains-off cold boot restores the card's own pin config\n",
				dev->name);
			ret = -ENODEV;
			goto failed;
		}
	}

	ret = mz0380_mst3367_init_regs(dev);
	if (ret)
		goto failed;

	/*
	 * Present the card as a sink: EDID first, then HPD. Toggle HPD low
	 * across the load so a source that was already attached re-reads the
	 * EDID instead of keeping whatever it saw before.
	 */
	ret = mz0380_mst3367_hpd(dev, false);
	if (ret)
		goto failed;
	/*
	 * M61: an EDID failure must NOT abort the bring-up. Nothing on this
	 * board has ever ACKed an EDID write - there is no EEPROM on the
	 * receiver's bus (M43/M51) - so with a synchronous edid_timeout_ms
	 * this returns -110 every single time. Gating on it left the receiver
	 * un-reset, un-initialised and HPD-less, which is why a run could
	 * produce no detect samples at all rather than merely no lock.
	 *
	 * The receiver locks perfectly well without an EDID: every lock this
	 * project has ever recorded happened with the EDID push failing. EDID
	 * only decides whether the SOURCE keeps transmitting.
	 */
	ret = mz0380_mst3367_load_edid(dev);
	if (ret)
		pr_warn("%s: EDID push failed (%d) - continuing; the receiver still detects, but the source will not hold its output\n",
			dev->name, ret);
	msleep(100);            /* let the source settle before the edge */
	ret = mz0380_mst3367_hpd(dev, true);
	if (ret)
		goto failed;

	dev->mst3367_ready = true;
	pr_info("%s: MST3367 receiver brought up (reset released, init applied)\n",
		dev->name);
	goto out;

failed:
	dev->mst3367_ready = false;
	pr_warn("%s: MST3367 bring-up failed (%d); receiver remains not ready\n",
		dev->name, ret);
out:
	mutex_unlock(&mst3367_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_bringup);

/* one sample of the receiver's mode-detect block, in hdcapm's units */
struct mst3367_measured {
	u16 htotal, vtotal, hactive;
	u16 hperiod, vperiod;       /* scaled, comparable to the table */
	u16 hperiod_raw, vperiod_raw;
	u16 lines;                  /* hfreq/vfreq: lines per vertical period */
	u8 detect;                   /* coherent post-snapshot R55 */
	const char *reject;          /* M62: which gate rejected this pass */
	bool interlace_from_geometry;/* M64: interlace decided by lines vs vtotal */
	bool lock_ended_during_pass; /* M64: source stopped under the last reads */
	u8 r5f;                     /* hdcapm's interlace register, logged raw */
	bool interlaced;
};

static int mst3367_measure(struct mz0380_dev *dev, struct mst3367_measured *out);
static bool mst3367_measurements_agree(const struct mst3367_measured *a,
				       const struct mst3367_measured *b);
static int mst3367_set_auto_position(struct mz0380_dev *dev, bool enable);
static const struct v4l2_dv_timings *
mst3367_match_mode(const struct mst3367_measured *m, bool *scaled);

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
	u32 reply[3] = { 0 };
	int ret = mz0380_send_command_reply(dev, MZ0380_CMD_GPIO_READ,
					     params, 1, NULL, 500,
					     reply, ARRAY_SIZE(reply));

	if (ret)
		return ret;
	/* op 0x14 returns the sampled bitmap in PARAM2 (BAR0+0x0c) */
	*out = !!(reply[2] & (1u << pin));
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

	mutex_lock(&mst3367_lock);
	ret = mst_bank(dev, MST3367_BANK3);
	if (ret)
		goto out_unlock;

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
	ret = hits ? 0 : -ENODEV;
out_unlock:
	mutex_unlock(&mst3367_lock);
	return ret;
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
 * (mask 0x3c) are the lock bits the GPL driver gates on.
 *
 * M173 settled what the other bits are, by watching R55 across a live
 * unplug/replug. This comment used to say bits 0..1 were "most plausibly
 * 5V/clock presence" and that guess stood unexamined for the life of the
 * project. It is WRONG: bits 0..1 read 3 with the cable in AND with it out,
 * while the lock bits went 0x3c -> 0x00 -> 0x3c. They are stuck high and carry
 * no status. This card has no +5V detect, which is why
 * V4L2_CID_DV_RX_POWER_PRESENT is not implemented.
 *
 * What does track the cable is mask 0x7c - so bit 6 moves with the signal
 * exactly as the 0x3c lock bits do, and neither we nor hdcapm gate on it.
 * Leave the mask alone: 0x3c has reported LOCKED correctly in every capture
 * this project has made.
 */
int mz0380_mst3367_watch(struct mz0380_dev *dev, unsigned int secs)
{
	unsigned long end;
	struct mst3367_measured last_m = { 0 };
	u8 last_detect = 0;
	int last_result = 0;
	bool last_matched = false;
	bool have_last_m = false;
	bool have_last_result = false;
	bool auto_position = false;
	bool have_auto_position = false;
	bool first = true;
	int ret = 0;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		if (ret)
			return ret;
	}

	pr_info("%s: watching MST3367 detect for %us - plug/unplug or power-cycle the source now\n",
		dev->name, secs);
	have_auto_position = false;
	end = jiffies + secs * HZ;

	while (time_before(jiffies, end)) {
		const struct v4l2_dv_timings *t = NULL;
		struct mst3367_measured m = { 0 };
		bool scaled = false;
		bool locked;
		bool changed;
		u8 detect;
		int sample_ret = 0;

		mutex_lock(&mst3367_lock);
		ret = mst_bank(dev, MST3367_BANK0);
		if (!ret)
			ret = mst_rd(dev, MST3367_B0_DETECT, &detect);
		if (ret) {
			mutex_unlock(&mst3367_lock);
			break;
		}

		locked = mst3367_status_locked(detect);
		if (locked) {
			sample_ret = mst3367_measure(dev, &m);
			if (!sample_ret)
				t = mst3367_match_mode(&m, &scaled);
			else if (sample_ret != -ENOLCK && sample_ret != -EAGAIN) {
				ret = sample_ret;
				mutex_unlock(&mst3367_lock);
				break;
			}
		}

		/*
		 * Any non-mode state goes back to acquisition - but only on a
		 * CHANGE. M63: this used to write AUTO_POSITION unconditionally
		 * every 250 ms, and that register restarts position/phase
		 * acquisition, so the watch was re-perturbing the very counters
		 * it then read. A measurement that "moved mid-pass" can be the
		 * driver's own doing.
		 */
		{
			bool want_acq = !locked || sample_ret || !t;

			if (!have_auto_position || auto_position != want_acq) {
				ret = mst3367_set_auto_position(dev, want_acq);
				if (!ret) {
					auto_position = want_acq;
					have_auto_position = true;
				}
			} else {
				ret = 0;
			}
		}
		mutex_unlock(&mst3367_lock);
		if (ret)
			break;

		if (!locked) {
			changed = first || !have_last_result || detect != last_detect ||
				  last_result != -ENOLCK;
			if (changed)
				pr_info("%s: detect 55=%02x %s (auto-position on; timing not sampled)\n",
					dev->name, detect,
					(detect & MST3367_B0_DETECT_LOCK_MASK) ?
					"settling" : "no-lock");
			last_result = -ENOLCK;
			last_detect = detect;
			last_matched = false;
			have_last_result = true;
			have_last_m = false;
		} else if (!t) {
			if (!sample_ret) {
				changed = first || !have_last_m || last_matched ||
					  !mst3367_measurements_agree(&last_m, &m);
				if (changed)
					pr_info("%s: detect 55=%02x LOCKED coherent htot=%u vtot=%u hact=%u hper=%u vper=%u lines=%u 5f=%02x %s => UNSUPPORTED (auto-position on)\n",
						dev->name, m.detect, m.htotal, m.vtotal,
						m.hactive, m.hperiod, m.vperiod,
						m.lines, m.r5f,
						m.interlaced ? "i" : "p");
				last_m = m;
				last_detect = m.detect;
				last_result = -ERANGE;
				last_matched = false;
				have_last_m = true;
				have_last_result = true;
			} else {
				changed = first || !have_last_result ||
					  last_result != -EAGAIN ||
					  detect != last_detect;
				if (changed)
					pr_info("%s: detect 55=%02x full lock but no coherent timing yet: %s (htot=%u vtot=%u hact=%u hper=%u vper=%u 5f=%02x) (auto-position on)\n",
						dev->name, detect,
						m.reject ? m.reject : "no reason recorded",
						m.htotal, m.vtotal, m.hactive,
						m.hperiod, m.vperiod, m.r5f);
				last_result = -EAGAIN;
				last_detect = detect;
				last_matched = false;
				have_last_result = true;
				have_last_m = false;
			}
		} else {
			changed = first || !have_last_m || !last_matched ||
				  !mst3367_measurements_agree(&last_m, &m);
			if (changed)
				pr_info("%s: detect 55=%02x LOCKED coherent htot=%u vtot=%u hact=%u hper=%u vper=%u lines=%u 5f=%02x %s => MATCHED%s (auto-position off)\n",
					dev->name, m.detect, m.htotal, m.vtotal,
					m.hactive, m.hperiod, m.vperiod, m.lines,
					m.r5f, m.interlaced ? "i" : "p",
					scaled ? " (host units)" : "");
			last_m = m;
			last_detect = m.detect;
			last_result = 0;
			last_matched = true;
			have_last_m = true;
			have_last_result = true;
		}
		first = false;
		msleep(250);
	}

	pr_info("%s: watch done%s\n", dev->name, ret ? " (I/O error)" : "");
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_watch);

/*
 * M73: dump the receiver's OUTPUT stage.
 *
 * Everything upstream is now proven: the receiver locks, measures a clean
 * 1080p60 and holds it for tens of seconds. Everything downstream is armed:
 * SET_VIC carries legal values, the encoder spawns, START fires. Yet the card
 * writes nothing at all - the buffer poison scan reads 0/1024 pages touched -
 * and tinyvenc5 opens the VIC itself ("Can't create video capture -> exit"),
 * so a VIC that never sees a BT1120 clock is the remaining explanation.
 *
 * We configure that output stage in commit_digital_output() and have never
 * once read it back. These are the registers that gate it:
 *   BANK0 0xab bit7 - freeze/hold, set around the 0xb0 update and cleared
 *   BANK0 0xb0      - output format/clock select (hdcapm writes 0x20 for
 *                     "YUV422 / 8-bit output"; we write 0x21 for non-720p30)
 *   BANK0 0xb1/0xb2 - output config / special-mode select
 *   BANK0 0xb3      - written 0xff by the Windows indirect-port path
 *   BANK2 0x07      - the timing latch (bit4 pulsed on first acquisition)
 * plus 0x55 so the lock state at the same instant is on the same line.
 */
void mz0380_mst3367_output_diag(struct mz0380_dev *dev, const char *tag)
{
	u8 r55 = 0, ab = 0, b0 = 0, b1 = 0, b2 = 0, b3 = 0, b7 = 0, r51 = 0;
	u8 ad = 0, ae = 0, b4 = 0, b5 = 0;
	u8 b2_01 = 0, b2_02 = 0, b2_07 = 0;
	u8 b1_01 = 0, b1_34 = 0, b2_0b = 0, b2_0c = 0, b2_0e = 0, b2_48 = 0;
	bool link_read = false;

	if (dev->fw_state != MZ0380_FW_STATE_READY || !dev->mst3367_ready)
		return;

	mutex_lock(&mst3367_lock);
	if (mst_bank(dev, MST3367_BANK0))
		goto out;
	mst_rd(dev, MST3367_B0_DETECT, &r55);
	mst_rd(dev, 0xab, &ab);
	/*
	 * M99: the three registers the Windows output-stage block writes and
	 * that we never read back. Write ops return result 0x00 whatever the
	 * bus does - the firmware forces it - so ret=0 on mst_wr() is NOT
	 * evidence a write landed. Only a read-back is. M99 scored MSTOUT=1 as
	 * neutral without them; that verdict is only sound if these three read
	 * ad=00 ae|=04 b4=54 on a MSTOUT=1 run.
	 */
	mst_rd(dev, 0xad, &ad);
	mst_rd(dev, 0xae, &ae);
	mst_rd(dev, 0xb4, &b4);
	/* M126: 0xb5 is resolution-dependent in gchd and a fixed constant here. */
	mst_rd(dev, 0xb5, &b5);
	mst_rd(dev, 0xb0, &b0);
	mst_rd(dev, 0xb1, &b1);
	mst_rd(dev, 0xb2, &b2);
	mst_rd(dev, 0xb3, &b3);
	mst_rd(dev, 0xb7, &b7);
	mst_rd(dev, 0x51, &r51);
	if (!mst_bank(dev, MST3367_BANK2)) {
		mst_rd(dev, 0x01, &b2_01);
		mst_rd(dev, 0x02, &b2_02);
		mst_rd(dev, 0x07, &b2_07);
		/* M78: HDMI packet reception (hdcapm MST3367_HdmiGetPacketStatus). */
		mst_rd(dev, 0x0b, &b2_0b);
		mst_rd(dev, 0x0c, &b2_0c);
		mst_rd(dev, 0x0e, &b2_0e);
		mst_rd(dev, 0x48, &b2_48);
		mst_bank(dev, MST3367_BANK0);
		/*
		 * M133: cache it for mz0380_mst3367_apply_csc_mode(). This read
		 * path is the one proven to work on hardware; the standalone one
		 * returned 0x00 while this returned 0xd2 156 ms later.
		 */
		dev->mst_b2_48 = b2_48;
		dev->mst_b2_48_valid = true;
	}
	/*
	 * M78: the LINK layer, which we have never read. R55 lock only says the
	 * timing front end recovered a clock; whether the receiver forwards
	 * pixels to BT1120 depends on whether the source came up in HDMI mode
	 * and on HDCP. hdcapm's RxTmdsGetType: BANK1 0x01 bit2 = HDMI (else
	 * DVI), bit0 = HDCP present, BANK1 0x34 bit7 = HDCP active. These are
	 * the same two registers the Windows driver reads right after its
	 * output-stage commit.
	 */
	if (!mst_bank(dev, MST3367_BANK1)) {
		mst_rd(dev, 0x01, &b1_01);
		mst_rd(dev, 0x34, &b1_34);
		link_read = true;
		mst_bank(dev, MST3367_BANK0);
	}

	pr_info("%s: output stage [%s]: R55=%02x %s | B0: ab=%02x ad=%02x ae=%02x b0=%02x b1=%02x b2=%02x b3=%02x b4=%02x b5=%02x b7=%02x 51=%02x | B2: 01=%02x 02=%02x 07=%02x\n",
		dev->name, tag, r55,
		mst3367_status_locked(r55) ? "LOCKED" : "no-lock",
		ab, ad, ae, b0, b1, b2, b3, b4, b5, b7, r51,
		b2_01, b2_02, b2_07);
	if (link_read)
		pr_info("%s: link [%s]: B1 01=%02x 34=%02x -> %s, HDCP %s%s | B2 0b=%02x 0c=%02x 0e=%02x 48=%02x, input colorspace %s\n",
			dev->name, tag, b1_01, b1_34,
			(b1_01 & 0x04) ? "HDMI" : "DVI (source fell back - EDID?)",
			(b1_01 & 0x01) ? "present" : "absent",
			(b1_34 & 0x80) ? ", ACTIVE (encrypted)" : "",
			b2_0b, b2_0c, b2_0e, b2_48,
			/* hdcapm MST3367_HdmiGetPacketColor: B2 0x48 bits 6:5 */
			(b2_48 & 0x60) == 0x00 ? "RGB" :
			(b2_48 & 0x60) == 0x20 ? "YUV422" :
			(b2_48 & 0x60) == 0x40 ? "YUV444" : "undefined");
	if (ab & 0x80)
		pr_warn("%s: output stage [%s]: 0xab bit7 is STILL SET - the output is frozen\n",
			dev->name, tag);
out:
	mutex_unlock(&mst3367_lock);
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_output_diag);

/*
 * M130: choose the CSC mode from the colour space the source is ACTUALLY
 * sending, and apply it.
 *
 * hdcapm reads the input colour space (BANK2 0x48 bits 6:5) into
 * regb2r48_cached and then never uses it: its CSC table goes out
 * unconditionally. That is harmless on its board, whose EDID makes sources
 * send RGB, so its fixed RGB->YCbCr matrix is always right. We push no EDID
 * (M127) and this source picks YUV444, so the same matrix converts YCbCr as
 * though it were RGB - measured on hardware as chroma 96% ANTI-correlated
 * with luma (M129/M130), because HDMI puts Cb on blue, Y on green and Cr on
 * red and an RGB matrix therefore computes Cb_out = -0.291*Y + ...
 *
 * With a YCbCr input and a YCbCr 4:2:2 output over BT1120 there is nothing to
 * convert, and clearing 0x92 bypasses the conversion. Verified on hardware:
 * corr(chroma, luma) went -0.966/-0.749 to -0.319/+0.349 and the picture came
 * out in correct, natural colour.
 *
 * This cannot live in init_regs(): that runs at bring-up, before HPD is even
 * asserted, so nothing is transmitting and 0x48 is meaningless. It has to be
 * re-applied once the receiver has locked, which is why the stream-start path
 * calls it.
 */
void mz0380_mst3367_apply_csc_mode(struct mz0380_dev *dev)
{
	static const char * const names[] = {
		"RGB", "YUV422", "YUV444", "undefined"
	};
	u8 b2_48 = 0;
	unsigned int cs;
	bool cached;
	u8 want;

	if (dev->fw_state != MZ0380_FW_STATE_READY || !dev->mst3367_ready)
		return;

	/*
	 * M133: prefer the value the output diag last read. A standalone
	 * BANK2 0x48 read from here is NOT reliable - on the M133 run it
	 * returned 0x00 (which decodes as RGB, the wrong branch) while the
	 * diag read 0xd2 (YUV444) from the same register 156 ms later, and the
	 * receiver was locked throughout. Rather than add a second read path
	 * and hope, reuse the one that demonstrably works. This is also why
	 * hdcapm caches the register instead of re-reading it.
	 *
	 * The stream-start path calls the diag immediately before us, so the
	 * cache is fresh. Falling back to our own read keeps this correct for
	 * any caller that has not.
	 */
	cached = dev->mst_b2_48_valid;
	if (cached) {
		b2_48 = dev->mst_b2_48;
		mutex_lock(&mst3367_lock);
	} else {
		mutex_lock(&mst3367_lock);
		if (mst_bank(dev, MST3367_BANK2))
			goto out;
		if (mst_rd(dev, 0x48, &b2_48))
			goto out;
		if (mst_bank(dev, MST3367_BANK0))
			goto out;
	}

	cs = (b2_48 & 0x60) >> 5;
	if (mz0380_mst_csc_ctl == MZ0380_MST_CSC_CTL_AUTO) {
		/*
		 * Convert only for an RGB source. "undefined" keeps hdcapm's
		 * value: it is what every board that works today ships with,
		 * so it is the safer thing to fall back to when 0x48 has not
		 * settled.
		 */
		want = (cs == 1 || cs == 2) ? 0x00 : MZ0380_MST_CSC_CTL_HDCAPM;
	} else {
		want = mz0380_mst_csc_ctl & 0xff;
	}

	if (mst_wr(dev, 0x92, want))
		goto out;
	pr_info("%s: MST3367 CSC 0x92 = 0x%02x (%s, input colorspace %s from 0x48=%02x, %s)\n",
		dev->name, want,
		mz0380_mst_csc_ctl == MZ0380_MST_CSC_CTL_AUTO ? "auto" : "forced",
		names[cs], b2_48, cached ? "cached" : "read here");
out:
	mutex_unlock(&mst3367_lock);
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_apply_csc_mode);

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

	mutex_lock(&mst3367_lock);
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
		goto out_unlock;
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
out_unlock:
	mutex_unlock(&mst3367_lock);
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_diag);

/* --- signal detect -------------------------------------------------------- */

/*
 * Map measured geometry to a standard v4l2_dv_timings. We know active width,
 * interlace, and an approximate frame rate; that uniquely picks a CEA preset
 * for the modes this HDMI capture card supports. The preset carries the exact
 * blanking/pixelclock the raw detect registers do not give us directly.
 *
 * vperiod is vertical-update rate times ten: about 600 for either 60p frames
 * or 60i fields. Interlace therefore comes from R5F bit3, not from halving
 * vperiod, and every counter is matched against an explicit tolerance range.
 */
struct mst3367_mode {
	struct v4l2_dv_timings timings;
	u16 htotal_min, htotal_max;
	u16 vtotal_min, vtotal_max;
	u16 hperiod_min, hperiod_max;
	u16 vperiod_min, vperiod_max;
	bool interlaced;
};


/*
 * M56: the progressive rows below are hdcapm's table verbatim (GPL, same
 * MST3367); the 1080i rows extend it from the corresponding CEA line/field
 * rates. Matching is on four measured ranges plus interlace - NOT hactive.
 * Note htotal here is the receiver's own counter, not always the video
 * horizontal total: 720p60 appears at ~2475 or ~1650 depending on the TMDS
 * clock domain, so both are listed.
 */
static const struct mst3367_mode mst3367_modes[] = {
	/*                          htot_min htot_max vtot_min vtot_max
	 *                          hper_min hper_max vper_min vper_max il  */
	{ V4L2_DV_BT_CEA_720X480P59_94,  845,  865,  520,  525,
					 310,  320,  595,  605, false },
	{ V4L2_DV_BT_CEA_1280X720P30,   2300, 2500,  745,  755,
					 215,  235,  290,  310, false },
	{ V4L2_DV_BT_CEA_1280X720P50,   2965, 2985,  745,  755,
					 360,  380,  480,  520, false },
	{ V4L2_DV_BT_CEA_1280X720P60,   2470, 2480,  745,  755,
					 445,  455,  595,  605, false },
	/* same 720p60 seen in the other clock domain (hdcapm: "Tivo") */
	{ V4L2_DV_BT_CEA_1280X720P60,   1645, 1655,  745,  755,
					 445,  455,  595,  605, false },
	{ V4L2_DV_BT_CEA_1920X1080P24,  4080, 4105, 1120, 1130,
					 260,  280,  230,  250, false },
	{ V4L2_DV_BT_CEA_1920X1080P25,  3950, 3970, 1120, 1130,
					 270,  290,  240,  254, false },
	{ V4L2_DV_BT_CEA_1920X1080P30,  2295, 3305, 1120, 1130,
					 330,  345,  290,  310, false },
	/* Interlaced totals count fields; I60 also represents 59.94 Hz. */
	{ V4L2_DV_BT_CEA_1920X1080I50,  3950, 3970,  555,  570,
					 270,  290,  490,  510, true  },
	{ V4L2_DV_BT_CEA_1920X1080I60,  3290, 3310,  555,  570,
					 330,  345,  590,  610, true  },
	{ V4L2_DV_BT_CEA_1920X1080P50,  3950, 3970, 1120, 1130,
					 550,  570,  480,  520, false },
	{ V4L2_DV_BT_CEA_1920X1080P60,  3290, 3310, 1120, 1130,
					 665,  685,  595,  605, false },
};

static bool mst3367_in_range(const struct mst3367_mode *e, u16 htotal,
			     u16 vtotal, u16 hperiod, u16 vperiod, bool il)
{
	return htotal  >= e->htotal_min  && htotal  <= e->htotal_max &&
	       vtotal  >= e->vtotal_min  && vtotal  <= e->vtotal_max &&
	       hperiod >= e->hperiod_min && hperiod <= e->hperiod_max &&
	       vperiod >= e->vperiod_min && vperiod <= e->vperiod_max &&
	       il == e->interlaced;
}

/*
 * M58 (hardware). Only htotal is in a different domain than hdcapm's table.
 * The clean full-lock sample reads
 *
 *   htot=2200 vtot=1125 hper=674 vper=599
 *
 * where vtotal (1125) and vperiod (599 == 60.0Hz, from 1250000/raw) already
 * sit inside the 1080p60 row, confirming hdcapm's vertical constants on this
 * silicon; hperiod likewise (raw 2372 -> 160MHz reference -> 67.4kHz). But
 * htotal is the TRUE video total, 2200, against the table's 3290-3310. That
 * split is not new: hdcapm's own table carries 720p60 twice, at 1650 (true)
 * and 2475 (x1.5), because the chip reports either depending on the clock
 * domain it locks in.
 *
 * So the table stays verbatim and only htotal is normalised. Raw units are
 * tried first, since the chip does report the x1.5 domain for some sources.
 *
 * M79 (hardware). A third domain exists, and it is HDMI DEEP COLOUR. The
 * chip counts htotal in TMDS character clocks, which deep colour scales:
 * 24-bit = x1, 30-bit = x1.25, 36-bit = x1.5. That is exactly why hdcapm
 * carries 720p60 twice (1650 true, 2475 = x1.5): the second row is a 36-bit
 * source, not a different mode.
 *
 * A source arriving here read htot=2750 vtot=1125 hact=1920 hper=674
 * vper=599 - self-consistent 1080p60 in every field except htotal, and
 * 2750 == 2200 x 1.25, i.e. 30-bit deep colour. Only the TMDS-domain counter
 * moves: hperiod and vperiod run off a fixed reference and read unchanged
 * (67.4 kHz / 59.9 Hz), and hactive is a video-domain counter and reads a
 * true 1920. The table sits in the x1.5 domain, so the correction from a
 * 30-bit measurement is x1.5/x1.25 = x6/5: 2750 * 6 / 5 == 3300, dead centre
 * of the 1080p60 row. Without it a perfectly good 1080p60 source is rejected
 * as "coherent but unsupported" and never reaches STREAMON at all.
 *
 * (M57 read x1.25/x0.8 into the vertical fields too. That was fitted to one
 * torn sample - htot=2200 vtot=899 vper=750 is a self-consistent 75Hz, the
 * right TMDS clock with vsync still settling, not a unit mismatch. Retracted.)
 */
static const struct v4l2_dv_timings *
mst3367_match_mode(const struct mst3367_measured *m, bool *scaled)
{
	unsigned int i;

	if (scaled)
		*scaled = false;

	for (i = 0; i < ARRAY_SIZE(mst3367_modes); i++)
		if (mst3367_in_range(&mst3367_modes[i], m->htotal, m->vtotal,
				     m->hperiod, m->vperiod, m->interlaced))
			return &mst3367_modes[i].timings;

	for (i = 0; i < ARRAY_SIZE(mst3367_modes); i++)
		if (mst3367_in_range(&mst3367_modes[i], m->htotal * 3 / 2,
				     m->vtotal, m->hperiod,
				     m->vperiod, m->interlaced)) {
			if (scaled)
				*scaled = true;
			return &mst3367_modes[i].timings;
		}

	/* M79: 30-bit deep colour reports x1.25; the table is x1.5. */
	for (i = 0; i < ARRAY_SIZE(mst3367_modes); i++)
		if (mst3367_in_range(&mst3367_modes[i], m->htotal * 6 / 5,
				     m->vtotal, m->hperiod,
				     m->vperiod, m->interlaced)) {
			if (scaled)
				*scaled = true;
			return &mst3367_modes[i].timings;
		}

	return NULL;
}

static int mst3367_set_auto_position(struct mz0380_dev *dev, bool enable)
{
	int ret;

	lockdep_assert_held(&mst3367_lock);

	/*
	 * M74: never re-arm acquisition while the encoder is capturing.
	 *
	 * AUTO_POSITION restarts the receiver's position/phase hunt. Doing that
	 * to a receiver that is actively feeding BT1120 to the SoC's VIC can
	 * only disturb the very frames we are trying to capture - and the
	 * diagnostic watch runs concurrently with capture by design, writing
	 * this register on every lock/no-lock transition of a source that
	 * flaps. Reads during streaming stay allowed; writes do not.
	 */
	if (READ_ONCE(dev->streaming) && enable) {
		pr_info_ratelimited("%s: skipping auto-position re-arm while streaming\n",
				    dev->name);
		return 0;
	}
	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;
	return mst_wr(dev, MST3367_B0_AUTO_POSITION,
		      enable ? MST3367_B0_AUTO_POSITION_ON :
			       MST3367_B0_AUTO_POSITION_OFF);
}

/*
 * Commit the receiver's digital output after a mode has stabilized.  On the
 * first accepted mode after init or lock loss, the HD60 Pro driver pulses
 * BANK2:07 bit4 to latch the new timing.  It then gates output with AB[7],
 * preserves B0's clock/polarity bits and selects 0x21 for ordinary modes.
 * The one special timing in our advertised table is 1280x720p30: it writes
 * BANK0:B2=3 and selects 0x20.  Treating every non-1080 mode as special was a
 * sibling-board shortcut and misclocked 720p50/60 and SD input.
 */
static int
mst3367_commit_digital_output(struct mz0380_dev *dev,
			      const struct v4l2_dv_timings *timings)
{
	const struct v4l2_bt_timings *bt = &timings->bt;
	u64 total = (u64)V4L2_DV_BT_FRAME_WIDTH(bt) *
		    V4L2_DV_BT_FRAME_HEIGHT(bt);
	u32 fps = total && bt->pixelclock ?
		  div_u64(bt->pixelclock + total / 2, total) : 0;
	bool special_720p30 = bt->width == 1280 && bt->height == 720 &&
			      !bt->interlaced && fps == 30;
	bool initial_acquisition = !dev->signal_locked;
	u8 reg07;
	u8 b0;
	int clear_ret;
	int ret;

	lockdep_assert_held(&mst3367_lock);
	if (initial_acquisition) {
		ret = mst_bank(dev, MST3367_BANK2);
		if (ret)
			return ret;
		ret = mst_rd(dev, 0x07, &reg07);
		if (ret)
			return ret;
		ret = mst_wr(dev, 0x07, reg07 | 0x10);
		if (ret)
			return ret;
		ret = mst_wr(dev, 0x07, reg07 & ~0x10);
		if (ret)
			return ret;
	}

	ret = mst_bank(dev, MST3367_BANK0);
	if (ret)
		return ret;

	/*
	 * M94: the Windows output-stage block, recovered from
	 * e60MZ0380.X64.SYS 0x14024fc08..0x14024fd0e. The I2C write helper takes
	 * the register in [rsp+0x20] and the value in [rsp+0x28] with r8b = 0x9c
	 * and edx = 0 (bank 0), which makes the whole sequence readable:
	 *
	 *   0xb0  = 0x14          (plain write - dil, loaded at 0x14024fc08)
	 *   0xae |= 0x04          (read-modify-write via the read helper)
	 *   0xad  = 0 or 1        (seta on a context flag at rbx+0x8540)
	 *   0xb1  = 0xc0
	 *   0xb2  = 0             (bpl)
	 *   0xb3  = 0             (0xff only on the board whose id bytes are
	 *                          0x5f/0x05 - explicitly not this one)
	 *   0xb4  = 0x55, then &= 0xfc   -> 0x54
	 *
	 * Our long-standing commit writes 0xb2, brackets 0xab bit7 and
	 * read-modify-writes 0xb0 - and never touches 0xad, 0xae or 0xb4 at all.
	 * 0xb1/0xb2/0xb3 read back matching Windows already; 0xb0 does not
	 * (0x21 vs 0x14), and three registers are simply unset.
	 *
	 * M80 swept vic_b0 0x21 against 0x14 and found 0x14 WORSE - but that was
	 * 0xb0 alone, with 0xae/0xad/0xb4 still at power-on. This is the first
	 * time the block has been written as a block. Off by default; the
	 * receiver output stage is the one place a "VIC sees nothing on BT1120"
	 * verdict can originate that the host can still reach.
	 */
	if (mz0380_mst_win_output) {
		ret = mst_set(dev, 0xab, 0x80);	/* freeze while retiming */
		if (!ret)
			ret = mst_wr(dev, 0xb0, mz0380_vic_b0 & 0xff);
		if (!ret)
			ret = mst_set(dev, 0xae, 0x04);
		if (!ret)
			ret = mst_wr(dev, 0xad, mz0380_mst_ad & 0xff);
		if (!ret)
			ret = mst_wr(dev, 0xb1, mz0380_mst_b1 & 0xff);
		if (!ret)
			ret = mst_wr(dev, 0xb2, mz0380_mst_b2 & 0xff);
		if (!ret)
			ret = mst_wr(dev, 0xb3, 0x00);
		if (!ret)
			ret = mst_wr(dev, 0xb4, 0x55);
		if (!ret)
			ret = mst_clr(dev, 0xb4, 0x03);
		clear_ret = mst_clr(dev, 0xab, 0x80);
		pr_info("%s: MST3367 output stage: Windows block applied (b0=%02x ae|=04 ad=%02x b1=%02x b2=%02x b3=00 b4=54) ret=%d\n",
			dev->name, mz0380_vic_b0 & 0xff,
			mz0380_mst_ad & 0xff, mz0380_mst_b1 & 0xff,
			mz0380_mst_b2 & 0xff, ret);
		return ret ? ret : clear_ret;
	}

	/*
	 * M100: BANK0 0xb0 is fully decoded now, from hdcapm's own commented-out
	 * alternatives (mst3367-drv.c, end of the init function) - the same
	 * receiver, values straight off a vendor trace:
	 *
	 *   0x25  RX_OUTPUT_YUV422 / 10.BITS / (EMBEDDED sync, by the bit rule)
	 *   0x24  RX_OUTPUT_YUV422 / 10.BITS / EXTERNAL SYNC
	 *   0x21  RX_OUTPUT_YUV422 / 08.BITS / EMBEDDED SYNC   <- ours
	 *   0x20  RX_OUTPUT_YUV422 / 08.BITS / EXTERNAL SYNC
	 *
	 * so: bit0 = embedded (1) vs external (0) sync, bit2 = 10-bit (1) vs
	 * 8-bit (0). This RETIRES the "bit0 is unidentified" comment that stood
	 * for months, and corrects M96, which inferred bit0 was a bus-width /
	 * clock-rate select from the receiver's period counters halving.
	 *
	 * It also explains every result we have. Both values that write nothing
	 * at all (Windows' 0x14 and hdcapm's 0x20) have bit0 = 0, i.e. EXTERNAL
	 * sync; the only value that gets the VIC to initialise (0x21) is the
	 * EMBEDDED-sync one. The SoC's VIC wants CCIR timing codes in-stream,
	 * which is also the half of "(CCIR or width chck fail)" we can now see
	 * we are on the right side of.
	 *
	 * Untried and the obvious next step: 0x25, which is 0x21 plus bit2 -
	 * one bit off the only value known to reach VIC init, and BT.1120 is
	 * natively a 20-bit interface (10-bit Y + 10-bit C). Feeding a 10-bit
	 * VIC an 8-bit stream is precisely a width/structure mismatch.
	 */
	ret = mst_wr(dev, 0xb2, special_720p30 ? 0x03 : (mz0380_mst_b2 & 0xff));
	if (ret)
		return ret;
	ret = mst_set(dev, 0xab, 0x80);
	if (ret)
		return ret;
	ret = mst_rd(dev, 0xb0, &b0);
	if (!ret) {
		u8 b0val = (b0 & 0xc2) |
			   (special_720p30 ? 0x20 : (mz0380_vic_b0 & 0x3d));

		/*
		 * M126: gchd's order - configuration first with the embedded-sync
		 * bit low, then assert it as the last write, outside the freeze.
		 */
		ret = mst_wr(dev, 0xb0,
			     mz0380_mst_b0_late ? (u8)(b0val & ~0x01) : b0val);
		clear_ret = mst_clr(dev, 0xab, 0x80);
		if (mz0380_mst_b0_late && !ret && !clear_ret) {
			ret = mst_wr(dev, 0xb0, b0val);
			pr_info("%s: MST3367 0xb0 committed in two steps: %02x (frozen) then %02x (after unfreeze) ret=%d\n",
				dev->name, (u8)(b0val & ~0x01), b0val, ret);
		}
	} else {
		clear_ret = mst_clr(dev, 0xab, 0x80);
	}

	return ret ? ret : clear_ret;
}

/*
 * Read one complete timing snapshot.  The HD60 Pro Windows driver accepts a
 * sample only with all four R55 lock bits, re-reads R5C, and verifies that the
 * measured vertical rate agrees with hfreq / vtotal.  Those checks reject the
 * partial-lock rows whose counters changed while mailbox reads were in flight.
 * mst3367_lock must cover the whole banked transaction.
 */
static int mst3367_measure_once(struct mz0380_dev *dev,
				struct mst3367_measured *out)
{
	u8 detect, detect_after, hi, lo, vtotal_hi_after, vtotal_lo_after;
	u16 vtotal_after;
	u8 r57, r58, r59, r5a, r5f;
	u16 raw;
	u32 converted, calculated_vperiod;
	int ret;

	memset(out, 0, sizeof(*out));
	lockdep_assert_held(&mst3367_lock);

	MST3367_TRY(mst_bank(dev, MST3367_BANK0));
	MST3367_TRY(mst_rd(dev, MST3367_B0_DETECT, &detect));
	out->detect = detect;   /* M63: so a REJECTED sample still reports R55 */
	if (!mst3367_status_locked(detect)) {
		out->reject = "no full lock at start of pass";
		return -ENOLCK;
	}

	MST3367_TRY(mst_rd(dev, MST3367_B0_HTOTAL_HI, &hi));
	MST3367_TRY(mst_rd(dev, MST3367_B0_HTOTAL_LO, &lo));
	out->htotal = (((u16)hi << 8) | lo) & 0xfff;

	MST3367_TRY(mst_rd(dev, MST3367_B0_VTOTAL_HI, &hi));
	MST3367_TRY(mst_rd(dev, MST3367_B0_VTOTAL_LO, &lo));
	out->vtotal = (((u16)hi << 8) | lo) & 0x7ff;

	MST3367_TRY(mst_rd(dev, MST3367_B0_HPERIOD_HI, &r57));
	MST3367_TRY(mst_rd(dev, MST3367_B0_HPERIOD_LO, &r58));
	MST3367_TRY(mst_rd(dev, MST3367_B0_VPERIOD_HI, &r59));
	MST3367_TRY(mst_rd(dev, MST3367_B0_VPERIOD_LO, &r5a));
	MST3367_TRY(mst_rd(dev, MST3367_B0_INTERLACE, &r5f));

	raw = ((u16)(r57 & 0x3f) << 8) | r58;
	out->hperiod_raw = raw;
	converted = raw ? 1600000u / raw : 0;
	if (converted > U16_MAX) {
		out->reject = "hperiod counter too small (rate overflowed u16)";
		return -EAGAIN;
	}
	out->hperiod = converted;

	raw = ((u16)(r59 & 0x3f) << 8) | r5a;
	out->vperiod_raw = raw;
	converted = raw ? 1250000u / raw : 0;
	if (converted > U16_MAX) {
		out->reject = "vperiod counter too small (rate overflowed u16)";
		return -EAGAIN;
	}
	out->vperiod = converted;

	/*
	 * The actual HD60 Pro binary uses R5F bit3.  This also explains the
	 * known progressive R5F=0x17 sample: old hdcapm bit1 is set, bit3 is not.
	 *
	 * M64 (hardware): but bit3 alone is not trustworthy either. A sample
	 * measuring htot=2200 vtot=1120 hper=674 vper=602 - a textbook 1080p60,
	 * and progressive beyond doubt - carried R5F=0x48, bit3 set. Note 0x40
	 * is set in every torn sample we have ever logged, so R5F most likely
	 * carries a validity flag and its other bits mean nothing while it is
	 * raised.
	 *
	 * The geometry does not lie: lines = hfreq/vfreq is the number of lines
	 * per VERTICAL period, which equals vtotal for a progressive source and
	 * half of it when the vertical counter is timing fields. Decide on that
	 * and keep the register bit only for the case where the geometry is too
	 * degenerate to call.
	 */
	out->r5f = r5f;
	out->lines = out->vperiod ?
		(u16)((u32)out->hperiod * 1000u / out->vperiod) : 0;

	if (out->lines && out->vtotal) {
		int d_prog = abs((int)out->lines - (int)out->vtotal);
		int d_int  = abs((int)out->lines - (int)out->vtotal / 2);

		out->interlaced = d_int < d_prog;
		out->interlace_from_geometry = true;
	} else {
		out->interlaced = !!(r5f & MST3367_B0_INTERLACE_BIT);
		out->interlace_from_geometry = false;
	}

	/*
	 * M64: prove the snapshot before spending anything else. hactive used
	 * to be fetched here, costing a BANK2 switch, two reads and a switch
	 * back - four mailbox round-trips, ~25ms of a ~300ms burst - ahead of
	 * the checks that decide whether the sample is usable at all. Nothing
	 * in mst3367_match_mode() reads hactive, so it is now collected after
	 * the verdict, best-effort.
	 */
	MST3367_TRY(mst_rd(dev, MST3367_B0_VTOTAL_HI, &vtotal_hi_after));
	MST3367_TRY(mst_rd(dev, MST3367_B0_VTOTAL_LO, &vtotal_lo_after));
	vtotal_after = (((u16)vtotal_hi_after << 8) | vtotal_lo_after) & 0x7ff;
	MST3367_TRY(mst_rd(dev, MST3367_B0_DETECT, &detect_after));
	/*
	 * M64: losing lock on this LAST read is not by itself a reason to throw
	 * the sample away, so the verdict is deferred until the sample has been
	 * checked on its own merits below. A bursty source (the microscope
	 * transmits for ~300ms after a power-cycle) routinely stops during the
	 * trailing round-trips, after the whole timing block has already been
	 * read - and a block that re-reads identical and passes its internal
	 * cross-check is good data no matter what the source did afterwards.
	 */
	/*
	 * M63: compare the WHOLE vtotal as a number, with a tolerance.
	 *
	 * The previous gate was (vtotal_lo ^ vtotal_lo_after) & 0xfe on the low
	 * byte alone, which is not a tolerance at all - it is a bit pattern:
	 *
	 *   1125 -> 1126 : 0x65 ^ 0x66 = 0x03, & 0xfe = 0x02 -> REJECTED
	 *   1124 -> 1125 : 0x64 ^ 0x65 = 0x01, & 0xfe = 0x00 -> accepted
	 *   1125 -> 1381 : low bytes identical                -> accepted (!)
	 *
	 * So a one-line wobble was rejected or accepted purely on the parity of
	 * the low byte, while a 256-line jump - the exact corruption an
	 * unlatched HI/LO pair produces - sailed through. Our own 1080p60
	 * sample sits at vtotal=1125, low byte 0x65, on the rejecting side of
	 * that coin: a settling source that moves one line is thrown away.
	 */
	if (abs((int)vtotal_after - (int)out->vtotal) > 2) {
		out->reject = "vtotal moved during the pass";
		return -EAGAIN;
	}

	/* Windows accepts 20.0--150.0 Hz and at most 0.5 Hz disagreement. */
	if (!out->htotal || !out->hperiod ||
	    out->vtotal < 150 || out->vperiod < 200 || out->vperiod > 1500) {
		out->reject =
			!out->htotal   ? "htotal is zero" :
			!out->hperiod  ? "hperiod is zero" :
			out->vtotal < 150 ? "vtotal below 150 lines" :
			out->vperiod < 200 ? "vperiod below 20.0 Hz" :
					 "vperiod above 150.0 Hz";
		return -EAGAIN;
	}
	/*
	 * M64: check the geometry, which covers both field orders.
	 *
	 * The old form was calculated_vperiod = hperiod*1000/(vtotal+1) against
	 * vperiod within 5 (0.5Hz). That silently assumed progressive: for
	 * 1080i60 the receiver reports vtotal as FRAME lines (1125) while
	 * vperiod times fields (600), so it computed 299 against 600 and threw
	 * every interlaced source away.
	 *
	 * lines (= hfreq/vfreq) is already the lines per vertical period, so
	 * the honest test is that it lands near vtotal (progressive) or near
	 * vtotal/2 (interlaced) - the same comparison that picked the field
	 * order above. Tolerance is in lines: both inputs are truncated
	 * integer divisions, so a couple of lines of slop is arithmetic, not
	 * signal instability.
	 */
	calculated_vperiod = out->interlaced ? out->vtotal / 2u : out->vtotal;
	if (abs((int)out->lines - (int)calculated_vperiod) > 8) {
		out->reject = "line count disagrees with vtotal";
		return -EAGAIN;
	}

	/*
	 * M64: everything above passed - the block re-read identical and the
	 * rate counters agree with the line total. Now, and only now, does the
	 * trailing lock state matter, and it is worth no more than a note.
	 */
	if (!mst3367_status_locked(detect_after)) {
		out->lock_ended_during_pass = true;
		pr_info("%s: MST3367 source stopped during the trailing reads (R55 %02x -> %02x) but the sample is self-consistent - keeping it\n",
			dev->name, detect, detect_after);
	} else {
		out->detect = detect_after;
	}

	/*
	 * Diagnostic only, and deliberately last: a failure here does not
	 * invalidate a sample that has already proved itself, and by now the
	 * source may legitimately have stopped.
	 */
	if (!mst_bank(dev, MST3367_BANK2) &&
	    !mst_rd(dev, MST3367_B2_HACTIVE_HI, &hi) &&
	    !mst_rd(dev, MST3367_B2_HACTIVE_LO, &lo)) {
		out->hactive = (((u16)hi << 8) | lo) & 0x1fff;
		if (out->hactive & 1)
			out->hactive++;
	}
	mst_bank(dev, MST3367_BANK0);

	return 0;
}

static bool mst3367_measurements_agree(const struct mst3367_measured *a,
				       const struct mst3367_measured *b)
{
	return abs((int)a->htotal - (int)b->htotal) <= 2 &&
	       abs((int)a->vtotal - (int)b->vtotal) <= 2 &&
	       abs((int)a->hactive - (int)b->hactive) <= 2 &&
	       abs((int)a->hperiod - (int)b->hperiod) <= 2 &&
	       abs((int)a->vperiod - (int)b->vperiod) <= 2 &&
	       a->interlaced == b->interlaced;
}

static int mst3367_measure(struct mz0380_dev *dev, struct mst3367_measured *out)
{
	struct mst3367_measured first, second;
	int ret;

	lockdep_assert_held(&mst3367_lock);
	ret = mst3367_measure_once(dev, &first);
	if (ret) {
		/*
		 * M62: hand the rejected sample back too. Its numbers plus
		 * ->reject are the only evidence of WHY a lock that really
		 * happened produced no timing, and without them a failed run
		 * says nothing more than "-EAGAIN".
		 */
		*out = first;
		return ret;
	}

	/*
	 * M62: the confirming pass is opt-in. See signal_confirm in core.c -
	 * two agreeing passes need ~170ms of unbroken lock, which the only
	 * source we have does not hold. measure_once already proves its own
	 * snapshot did not move (detect + vtotal_lo re-read) and that the
	 * fields are mutually consistent.
	 */
	if (!mz0380_signal_confirm) {
		*out = first;
		return 0;
	}

	usleep_range(10000, 12000);
	ret = mst3367_measure_once(dev, &second);
	if (ret) {
		*out = second;
		return ret;
	}
	if (!mst3367_measurements_agree(&first, &second)) {
		second.reject = "the two confirming passes disagreed";
		*out = second;
		return -EAGAIN;
	}

	*out = second;
	return 0;
}

static bool mst3367_force_is_safe_1080p60(const struct mst3367_measured *m)
{
	return !m->interlaced &&
	       m->hactive >= 1918 && m->hactive <= 1924 &&
	       m->vtotal >= 1120 && m->vtotal <= 1130 &&
	       m->hperiod >= 665 && m->hperiod <= 685 &&
	       m->vperiod >= 595 && m->vperiod <= 605 &&
	       ((m->htotal >= 2190 && m->htotal <= 2210) ||
		(m->htotal >= 3290 && m->htotal <= 3310));
}

/*
 * Read the MST3367 mode-detect block and fill *out. Returns 0 with a valid
 * timing when a signal is locked, -ENOLCK when no coherent signal, -ERANGE
 * for a coherent unsupported mode, or an I/O errno. Runs bring-up on first use.
 */
int mz0380_mst3367_read_signal(struct mz0380_dev *dev,
			       struct v4l2_dv_timings *out)
{
	static const struct v4l2_dv_timings forced_p60 =
		V4L2_DV_BT_CEA_1920X1080P60;
	const struct v4l2_dv_timings *match = NULL;
	struct mst3367_measured m = { 0 };
	/*
	 * M63: m now also receives REJECTED samples (that is how the failure
	 * path reports real numbers), so the last coherent-but-unsupported one
	 * has to be kept separately or a later rejection overwrites it and the
	 * -ERANGE report describes the wrong sample.
	 */
	struct mst3367_measured unsupported = { 0 };
	bool scaled = false;
	bool acquisition_enabled = false;
	bool forced = false;
	bool have_sample = false;
	bool saw_full_lock = false;
	/* M62: m holds the last REJECTED sample too, so the failure path can
	 * report the numbers that were actually read. */
	unsigned int lock_samples = 0, sample_attempts = 0;
	int last_sample_ret = 0;
	unsigned long deadline;
	u8 detect = 0;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY)
		return -ENODEV;
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		if (ret)
			return ret;
	}

	/*
	 * The Windows driver gates timing reads on the complete R55 lock mask.
	 * Partial masks are acquisition states, not lower-quality locks.  Keep
	 * auto-position enabled while acquiring or after any unstable snapshot;
	 * disable it only once two coherent samples identify a supported mode.
	 */
	mutex_lock(&mst3367_lock);
	deadline = jiffies + msecs_to_jiffies(mz0380_signal_poll_ms);
	for (;;) {
		ret = mst_bank(dev, MST3367_BANK0);
		if (ret)
			goto out_unlock;
		ret = mst_rd(dev, MST3367_B0_DETECT, &detect);
		if (ret)
			goto out_unlock;

		if (!mst3367_status_locked(detect)) {
			if (!acquisition_enabled) {
				ret = mst3367_set_auto_position(dev, true);
				if (ret)
					goto out_unlock;
				acquisition_enabled = true;
			}
		} else {
			saw_full_lock = true;
			lock_samples++;
			sample_attempts++;
			ret = mst3367_measure(dev, &m);
			last_sample_ret = ret;
			if (!ret) {
				have_sample = true;
				unsupported = m;
				match = mst3367_match_mode(&m, &scaled);
				if (!match && mz0380_force_timings &&
				    mst3367_force_is_safe_1080p60(&m)) {
					match = &forced_p60;
					forced = true;
					scaled = false;
				}
				if (match)
					goto matched;
			} else if (ret != -ENOLCK && ret != -EAGAIN) {
				goto out_unlock;
			}

			/* Lost lock, incoherent, or coherent but unsupported. */
			if (!acquisition_enabled) {
				ret = mst3367_set_auto_position(dev, true);
				if (ret)
					goto out_unlock;
				acquisition_enabled = true;
			}
		}

		if (time_after_eq(jiffies, deadline))
			break;
		msleep(20);
	}

	if (have_sample) {
		pr_info("%s: MST3367 coherent but unsupported: htot=%u vtot=%u hact=%u hper=%u(raw %u) vper=%u(raw %u) lines=%u 5f=%02x %s [R55=0x%02x] - please report\n",
			dev->name, unsupported.htotal, unsupported.vtotal,
			unsupported.hactive, unsupported.hperiod,
			unsupported.hperiod_raw, unsupported.vperiod,
			unsupported.vperiod_raw, unsupported.lines,
			unsupported.r5f,
			unsupported.interlaced ? "i" : "p", unsupported.detect);
		ret = -ERANGE;
	} else {
		if (saw_full_lock)
			pr_info("%s: MST3367 full lock appeared %u time(s), %u measurement attempt(s), none coherent (last %d: %s) - rejected sample was htot=%u vtot=%u hact=%u hper=%u(raw %u) vper=%u(raw %u) lines=%u 5f=%02x %s R55=%02x\n",
				dev->name, lock_samples, sample_attempts,
				last_sample_ret,
				m.reject ? m.reject : "no reason recorded",
				m.htotal, m.vtotal, m.hactive, m.hperiod,
				m.hperiod_raw, m.vperiod, m.vperiod_raw,
				m.lines, m.r5f, m.interlaced ? "i" : "p",
				m.detect);
		ret = -ENOLCK;
	}
	goto out_unlock;

matched:
	ret = mst3367_set_auto_position(dev, false);
	if (ret)
		goto out_unlock;
	ret = mst3367_commit_digital_output(dev, match);
	if (ret)
		goto out_unlock;
	*out = *match;
	pr_info("%s: MST3367 signal: %ux%u%s%s%s (htot=%u vtot=%u hper=%u vper=%u hact=%u R55=0x%02x)%s%s\n",
		dev->name, out->bt.width, out->bt.height,
		out->bt.interlaced ? "i" : "p",
		scaled ? " [host units]" : "",
		forced ? " [strict forced fallback]" : "", m.htotal, m.vtotal,
		m.hperiod, m.vperiod, m.hactive, m.detect,
		m.interlace_from_geometry ? "" : " [interlace from R5F bit]",
		m.lock_ended_during_pass ? " [source stopped mid-pass]" : "");
	ret = 0;

out_unlock:
	mutex_unlock(&mst3367_lock);
	return ret;
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

/*
 * M51b. op 0x17's data polarity is NOT verified: ep.ko has both
 * gpio_direction_input and _output, but which data bit selects which is a
 * guess. Getting it backwards swaps release/drive, and the bus then looks
 * permanently stuck low - exactly the first hardware symptom. Runtime
 * switch so both readings can be tried without a rebuild.
 */
static int mz0380_bb_dir(struct mz0380_dev *dev, u8 pin, bool output)
{
	bool bit = mz0380_gpio_dir_invert ? !output : output;
	u32 params[2] = { 1u << pin, bit ? (1u << pin) : 0 };

	return mz0380_send_command(dev, MZ0380_CMD_GPIO_DIR, params, 2,
				   NULL, 500);
}

/*
 * M51b. Which pins even exist, and which float high? An I2C pair sits on
 * pull-ups, so it reads 1 when nobody drives it; a pin that is absent or
 * grounded reads 0 forever. One masked read of the whole port, then a
 * per-pin sweep (the firmware may only honour single-pin masks), gives the
 * candidate list before any bit-banging is attempted.
 */
int mz0380_gpio_dump(struct mz0380_dev *dev)
{
	u32 params[1] = { 0xffffffffu };
	u32 reply[4] = { 0 };
	u32 bitmap = 0;
	unsigned int pin;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_info("%s: gpiodump: firmware not READY (state %s)\n",
			dev->name, mz0380_fw_state_name(dev->fw_state));
		return -ENODEV;
	}

	ret = mz0380_send_command_reply(dev, MZ0380_CMD_GPIO_READ, params, 1,
					 NULL, 500, reply, ARRAY_SIZE(reply));
	pr_info("%s: gpiodump full-mask read ret=%d PARAM0=%08x PARAM1=%08x PARAM2=%08x PARAM3=%08x\n",
		dev->name, ret, reply[0], reply[1], reply[2], reply[3]);

	for (pin = 0; pin < 32; pin++) {
		u32 one[1] = { 1u << pin };
		u32 one_reply[3] = { 0 };

		if (mz0380_send_command_reply(dev, MZ0380_CMD_GPIO_READ,
					       one, 1, NULL, 500, one_reply,
					       ARRAY_SIZE(one_reply)))
			continue;
		if (one_reply[2] & (1u << pin))
			bitmap |= 1u << pin;
	}

	pr_info("%s: gpiodump per-pin idle bitmap = 0x%08x (HIGH pins are pull-up candidates for an I2C pair)\n",
		dev->name, bitmap);
	for (pin = 0; pin < 32; pin++)
		if (bitmap & (1u << pin))
			pr_info("%s: gpiodump   pin %2u reads HIGH\n",
				dev->name, pin);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_gpio_dump);

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
	u32 reply[3] = { 0 };
	int ret = mz0380_send_command_reply(dev, MZ0380_CMD_GPIO_READ,
					     params, 1, NULL, 500,
					     reply, ARRAY_SIZE(reply));

	if (ret)
		return ret;
	*val = !!(reply[2] & (1u << pin));
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

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_info("%s: i2cbb scan: firmware not READY (state %s) - mailbox dead, cold boot needed?\n",
			dev->name, mz0380_fw_state_name(dev->fw_state));
		return -ENODEV;
	}

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
		pr_info("%s: i2cbb scan sda=%u scl=%u (dir_invert=%u): SDA reads LOW when released - wrong pin, wrong DIR polarity, or no pull-up; aborting. Run 'gpiodump' for the pins that float HIGH, and try gpio_dir_invert=1\n",
			dev->name, sda, scl, mz0380_gpio_dir_invert);
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

	/*
	 * Hand the pins back as OUTPUTS. Leaving them as inputs is not
	 * cosmetic: pin9 is the receiver's reset and pin1 is HPD, so a probe
	 * that walks away mid-emulation leaves the receiver held in reset with
	 * a dead I2C bus, and no rmmod/insmod can undo it (only a cold boot).
	 */
	mz0380_bb_dir(dev, sda, true);
	mz0380_bb_dir(dev, scl, true);
	dev->mst3367_ready = false;   /* force a fresh bring-up after this */

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

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_info("%s: i2cbb burn: firmware not READY (state %s) - mailbox dead, cold boot needed?\n",
			dev->name, mz0380_fw_state_name(dev->fw_state));
		return -ENODEV;
	}

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

/* --- M53: hunt an INDIRECT address/data port into the EDID RAM ----------- */

/*
 * M49b closed the DIRECT search: no bank holds an EDID-sized writable window
 * (longest contiguous run anywhere is 33 bytes). But that scan could never
 * have found the other way receivers expose EDID RAM - an INDIRECT port: one
 * register holds an address, a second reads/writes the byte at that address,
 * and the RAM behind them is invisible to a register sweep. Two ordinary
 * config registers, indistinguishable from the rest.
 *
 * They are separable by BEHAVIOUR, though, and cheaply. Storage indexed by an
 * address register remembers a different byte per address; a plain register
 * remembers only the last byte written to it:
 *
 *     A=0x10, D=0xa5 ; A=0x20, D=0x5a ; then A=0x10 -> D reads 0xa5?
 *                                            A=0x20 -> D reads 0x5a?
 *
 * A plain register returns 0x5a both times and is rejected. Both values
 * surviving means the pair (A,D) is a window onto address-indexed storage -
 * the EDID RAM, if it exists at all. Every register touched is restored.
 */
static bool mst_indirect_pair_is_ram(struct mz0380_dev *dev, u8 areg, u8 dreg)
{
	u8 v1 = 0, v2 = 0;

	if (mst_wr(dev, areg, 0x10) || mst_wr(dev, dreg, 0xa5) ||
	    mst_wr(dev, areg, 0x20) || mst_wr(dev, dreg, 0x5a) ||
	    mst_wr(dev, areg, 0x10) || mst_rd(dev, dreg, &v1) ||
	    mst_wr(dev, areg, 0x20) || mst_rd(dev, dreg, &v2))
		return false;

	return v1 == 0xa5 && v2 == 0x5a;
}

int mz0380_mst3367_edidhunt(struct mz0380_dev *dev)
{
	unsigned int bank, hits = 0, probe;
	bool answered = false;
	int ret;

	if (dev->fw_state != MZ0380_FW_STATE_READY) {
		pr_info("%s: edidhunt: firmware not READY (state %s)\n",
			dev->name, mz0380_fw_state_name(dev->fw_state));
		return -ENODEV;
	}

	/*
	 * The receiver powers up held in reset (pin9, active-low) and NAKs
	 * every access until released - and the firmware turns a NAK into a
	 * 0x00 result byte, so an un-brought-up bus looks exactly like a chip
	 * whose registers are all read-only. Bring it up first.
	 */
	if (!dev->mst3367_ready) {
		ret = mz0380_mst3367_bringup(dev);
		if (ret)
			return ret;
	}

	mutex_lock(&mst3367_lock);
	/*
	 * Then prove the bus is alive before believing any negative: read a
	 * spread of bank0 registers and require at least one non-zero. All
	 * zeros means nobody is answering, which is a broken probe, not the
	 * absence of an EDID window.
	 */
	mst_bank(dev, MST3367_BANK0);
	for (probe = 0x01; probe <= 0xf1; probe += 0x10) {
		u8 v = 0;

		if (!mst_rd(dev, probe, &v) && v)
			answered = true;
	}
	if (!answered) {
		pr_info("%s: edidhunt ABORTED: every register reads 0x00 - the receiver is not answering I2C (NAK), so this run would prove nothing. Check the bring-up (reset pin9) first\n",
			dev->name);
		ret = -ENXIO;
		goto out_unlock;
	}

	pr_info("%s: edidhunt: receiver answering; looking for an indirect address/data port (up to %u candidates per bank)\n",
		dev->name, mz0380_edidhunt_max_regs);

	for (bank = 0; bank <= 3; bank++) {
		u8 cand[256];
		u8 orig[256];
		unsigned int n = 0, i, j, tried = 0;
		unsigned int reg;

		if (mst_bank(dev, bank))
			continue;

		/* candidates = registers that hold an arbitrary byte */
		for (reg = 1; reg <= 0xff && n < mz0380_edidhunt_max_regs; reg++) {
			u8 was = 0, rb = 0;

			if (mst_rd(dev, reg, &was))
				continue;
			if (mst_wr(dev, reg, 0x5a) || mst_rd(dev, reg, &rb)) {
				mst_wr(dev, reg, was);
				continue;
			}
			mst_wr(dev, reg, was);
			if (rb == 0x5a) {
				orig[n] = was;
				cand[n++] = reg;
			}
			cond_resched();
		}

		pr_info("%s: edidhunt bank%u: %u writable candidates -> %u ordered pairs\n",
			dev->name, bank, n, n * (n ? n - 1 : 0));

		if (!n)
			pr_info("%s: edidhunt bank%u: no register accepted a write - M49 found writable runs here, so treat this as a dead bus, not a result\n",
				dev->name, bank);

		for (i = 0; i < n; i++) {
			for (j = 0; j < n; j++) {
				if (i == j)
					continue;
				tried++;
				if (mst_indirect_pair_is_ram(dev, cand[i],
							     cand[j])) {
					pr_info("%s: edidhunt HIT bank%u: addr=0x%02x data=0x%02x behaves as address-indexed RAM - EDID window candidate\n",
						dev->name, bank, cand[i],
						cand[j]);
					hits++;
				}
				/* put both registers back as we go */
				mst_wr(dev, cand[i], orig[i]);
				mst_wr(dev, cand[j], orig[j]);
				cond_resched();
				if (!(tried % 500))
					pr_info("%s: edidhunt bank%u: %u pairs tested\n",
						dev->name, bank, tried);
			}
		}
	}

	if (hits)
		pr_info("%s: edidhunt done: %u candidate port(s) - write the EDID through one and re-pulse HPD\n",
			dev->name, hits);
	else
		pr_info("%s: edidhunt done: NO indirect port found. The receiver holds no host-writable EDID storage, so the EDID must come from elsewhere on the DDC lines (card-side bus) - static RE is exhausted, a live Windows trace is the remaining route\n",
			dev->name);
	ret = 0;
out_unlock:
	mutex_unlock(&mst3367_lock);
	return ret;
}
EXPORT_SYMBOL_GPL(mz0380_mst3367_edidhunt);
