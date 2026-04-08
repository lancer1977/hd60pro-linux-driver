/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Copyright (c) 2026
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 */

#ifndef _MZ0380_H_
#define _MZ0380_H_

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/version.h>

#include "mz0380-reg.h"

#define MZ0380_VERSION_CODE KERNEL_VERSION(0, 1, 0)

#define MZ0380_MAXBOARDS 8
#define MZ0380_MAX_MAPS 2
#define UNSET (-1U)

#define MZ0380_BOARD_NOAUTO             UNSET
#define MZ0380_BOARD_UNKNOWN            0
#define MZ0380_BOARD_ELGATO_HD60_PRO    1

struct mz0380_board {
	const char *name;
	const char *windows_driver;
	const char *firmware_name;
};

struct mz0380_subid {
	u16 subvendor;
	u16 subdevice;
	u32 card;
};

struct mz0380_dev {
	struct list_head devlist;
	struct pci_dev *pci;
	struct mutex lock;
	char name[32];
	unsigned int nr;
	unsigned int board;
	void __iomem *lmmio[MZ0380_MAX_MAPS];
	u8 __iomem *bmmio[MZ0380_MAX_MAPS];
	int bar_nr[MZ0380_MAX_MAPS];
	resource_size_t bar_start[MZ0380_MAX_MAPS];
	resource_size_t bar_len[MZ0380_MAX_MAPS];
	u8 pci_rev;
	u8 pci_lat;
};

extern struct mz0380_board mz0380_boards[];
extern const unsigned int mz0380_bcount;
extern struct mz0380_subid mz0380_subids[];
extern const unsigned int mz0380_idcount;

void mz0380_card_list(struct mz0380_dev *dev);
void mz0380_card_setup(struct mz0380_dev *dev);

u32 mz_read(struct mz0380_dev *dev, int map, u32 reg);
void mz_write(struct mz0380_dev *dev, int map, u32 reg, u32 value);

#endif
