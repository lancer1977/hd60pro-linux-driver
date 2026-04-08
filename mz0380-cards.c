/*
 *  Driver for MZ0380 based capture cards.
 */

#include "mz0380.h"

struct mz0380_board mz0380_boards[] = {
	[MZ0380_BOARD_UNKNOWN] = {
		.name = "UNKNOWN/GENERIC",
		.windows_driver = "unknown",
		.firmware_name = "unknown",
	},
	[MZ0380_BOARD_ELGATO_HD60_PRO] = {
		.name = "Elgato Game Capture HD60 Pro",
		.windows_driver = "e60MZ0380.X64.SYS",
		.firmware_name = "MZ0380.HD.HEX",
	},
};
const unsigned int mz0380_bcount = ARRAY_SIZE(mz0380_boards);

struct mz0380_subid mz0380_subids[] = {
	{
		.subvendor = 0x1cfa,
		.subdevice = 0x0006,
		.card = MZ0380_BOARD_ELGATO_HD60_PRO,
	},
};
const unsigned int mz0380_idcount = ARRAY_SIZE(mz0380_subids);

void mz0380_card_list(struct mz0380_dev *dev)
{
	int i;

	if (dev->pci->subsystem_vendor == 0 &&
	    dev->pci->subsystem_device == 0) {
		printk(KERN_INFO
		       "%s: Board has no valid PCIe subsystem ID.\n"
		       "%s: Pass card=<n> as an insmod option to override autodetect.\n",
		       dev->name, dev->name);
	} else {
		printk(KERN_INFO
		       "%s: Board %04x:%04x is not known to the driver.\n"
		       "%s: Pass card=<n> as an insmod option to override autodetect.\n",
		       dev->name,
		       dev->pci->subsystem_vendor,
		       dev->pci->subsystem_device,
		       dev->name);
	}

	printk(KERN_INFO "%s: Valid card=<n> values are:\n", dev->name);
	for (i = 0; i < mz0380_bcount; i++)
		printk(KERN_INFO "%s:    card=%d -> %s\n",
		       dev->name, i, mz0380_boards[i].name);
}

void mz0380_card_setup(struct mz0380_dev *dev)
{
	switch (dev->board) {
	case MZ0380_BOARD_ELGATO_HD60_PRO:
		/*
		 * Keep the first implementation probe-safe. We only identify
		 * and map resources here until the command and DMA interfaces
		 * have been reverse engineered.
		 */
		break;
	}
}
