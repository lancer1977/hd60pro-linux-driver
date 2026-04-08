/*
 *  Driver for MZ0380 based capture cards.
 */

#include "mz0380.h"

MODULE_DESCRIPTION("Driver for MZ0380 based capture cards");
MODULE_AUTHOR("OpenAI");
MODULE_LICENSE("GPL");

unsigned int procfs_verbosity = 1;
module_param(procfs_verbosity, int, 0644);
MODULE_PARM_DESC(procfs_verbosity,
		 "procfs debug level; wide BAR scans are disabled as unsafe");

static unsigned int debug;
module_param(debug, int, 0644);
MODULE_PARM_DESC(debug, "enable debug messages");

static bool allow_bus_master;
module_param(allow_bus_master, bool, 0644);
MODULE_PARM_DESC(allow_bus_master,
		 "allow PCI bus mastering during probe; unsafe until DMA is understood");

static unsigned int card[] = { [0 ... (MZ0380_MAXBOARDS - 1)] = UNSET };
module_param_array(card, int, NULL, 0444);
MODULE_PARM_DESC(card, "card type");

#define dprintk(level, fmt, arg...) \
	do { if (debug >= level) \
		printk(KERN_DEBUG "%s: " fmt, dev->name, ##arg); \
	} while (0)

static unsigned int mz0380_devcount;
static DEFINE_MUTEX(devlist);
static LIST_HEAD(mz0380_devlist);

u32 mz_read(struct mz0380_dev *dev, int map, u32 reg)
{
	return readl(dev->lmmio[map] + reg);
}

void mz_write(struct mz0380_dev *dev, int map, u32 reg, u32 value)
{
	writel(value, dev->lmmio[map] + reg);
}

static int mz0380_request_bar(struct mz0380_dev *dev, int map, int bar)
{
	resource_size_t start = pci_resource_start(dev->pci, bar);
	resource_size_t len = pci_resource_len(dev->pci, bar);

	if (request_mem_region(start, len, dev->name) == 0) {
		printk(KERN_ERR "%s: can't get bar[%d] memory @ 0x%llx\n",
		       dev->name, bar, (unsigned long long)start);
		return -EBUSY;
	}

	dev->bar_nr[map] = bar;
	dev->bar_start[map] = start;
	dev->bar_len[map] = len;

	dev->lmmio[map] = ioremap(start, len);
	if (!dev->lmmio[map]) {
		release_mem_region(start, len);
		dev->bar_nr[map] = -1;
		dev->bar_start[map] = 0;
		dev->bar_len[map] = 0;
		printk(KERN_ERR "%s: failed to ioremap bar[%d]\n",
		       dev->name, bar);
		return -ENOMEM;
	}

	dev->bmmio[map] = (u8 __iomem *)dev->lmmio[map];
	return 0;
}

static void mz0380_release_bar(struct mz0380_dev *dev, int map)
{
	if (dev->lmmio[map]) {
		iounmap(dev->lmmio[map]);
		dev->lmmio[map] = NULL;
		dev->bmmio[map] = NULL;
	}

	if (dev->bar_nr[map] >= 0) {
		release_mem_region(dev->bar_start[map], dev->bar_len[map]);
		dev->bar_nr[map] = -1;
		dev->bar_start[map] = 0;
		dev->bar_len[map] = 0;
	}
}

static int mz0380_dev_setup(struct mz0380_dev *dev)
{
	int i;
	int ret;

	mutex_init(&dev->lock);

	dev->nr = mz0380_devcount++;
	snprintf(dev->name, sizeof(dev->name), "mz0380[%u]", dev->nr);

	for (i = 0; i < MZ0380_MAX_MAPS; i++)
		dev->bar_nr[i] = -1;

	dev->board = UNSET;
	if (card[dev->nr] < mz0380_bcount)
		dev->board = card[dev->nr];
	for (i = 0; dev->board == UNSET && i < mz0380_idcount; i++) {
		if (dev->pci->subsystem_vendor == mz0380_subids[i].subvendor &&
		    dev->pci->subsystem_device == mz0380_subids[i].subdevice)
			dev->board = mz0380_subids[i].card;
	}
	if (dev->board == UNSET) {
		dev->board = MZ0380_BOARD_UNKNOWN;
		mz0380_card_list(dev);
	}

	ret = mz0380_request_bar(dev, MZ0380_MAP_BAR_MMIO, MZ0380_BAR_MMIO);
	if (ret < 0)
		goto fail;

	ret = mz0380_request_bar(dev, MZ0380_MAP_BAR_CFG, MZ0380_BAR_CFG);
	if (ret < 0)
		goto fail;

	if (dev->bar_len[MZ0380_MAP_BAR_MMIO] < MZ0380_BAR_MMIO_SIZE ||
	    dev->bar_len[MZ0380_MAP_BAR_CFG] < MZ0380_BAR_CFG_SIZE) {
		printk(KERN_ERR
		       "%s: unexpected BAR sizes mmio=0x%llx cfg=0x%llx\n",
		       dev->name,
		       (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_MMIO],
		       (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_CFG]);
		ret = -ENODEV;
		goto fail;
	}

	printk(KERN_INFO
	       "%s: subsystem: %04x:%04x, board: %s [card=%d,%s]\n",
	       dev->name,
	       dev->pci->subsystem_vendor,
	       dev->pci->subsystem_device,
	       mz0380_boards[dev->board].name,
	       dev->board,
	       card[dev->nr] == dev->board ? "insmod option" : "autodetected");
	printk(KERN_INFO
	       "%s: bar%d @ 0x%llx [0x%llx bytes], bar%d @ 0x%llx [0x%llx bytes]\n",
	       dev->name,
	       dev->bar_nr[MZ0380_MAP_BAR_MMIO],
	       (unsigned long long)dev->bar_start[MZ0380_MAP_BAR_MMIO],
	       (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_MMIO],
	       dev->bar_nr[MZ0380_MAP_BAR_CFG],
	       (unsigned long long)dev->bar_start[MZ0380_MAP_BAR_CFG],
	       (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_CFG]);

	return 0;

fail:
	mz0380_release_bar(dev, MZ0380_MAP_BAR_CFG);
	mz0380_release_bar(dev, MZ0380_MAP_BAR_MMIO);
	mz0380_devcount--;
	return ret;
}

static void mz0380_dev_unregister(struct mz0380_dev *dev)
{
	mz0380_release_bar(dev, MZ0380_MAP_BAR_CFG);
	mz0380_release_bar(dev, MZ0380_MAP_BAR_MMIO);
}

static void mz0380_dump_pci_config(struct seq_file *m, struct mz0380_dev *dev)
{
	u32 reg;
	u32 val = 0;

	seq_puts(m, "  pci cfg dump:\n");
	for (reg = 0; reg < 0x100; reg += 0x10) {
		pci_read_config_dword(dev->pci, reg + 0x0, &val);
		seq_printf(m, "    %02x: %08x", reg + 0x0, val);
		pci_read_config_dword(dev->pci, reg + 0x4, &val);
		seq_printf(m, " %08x", val);
		pci_read_config_dword(dev->pci, reg + 0x8, &val);
		seq_printf(m, " %08x", val);
		pci_read_config_dword(dev->pci, reg + 0xc, &val);
		seq_printf(m, " %08x\n", val);
		}
}

static void mz0380_dump_pci_caps(struct seq_file *m, struct mz0380_dev *dev)
{
	int pm_cap;
	int msi_cap;
	int pcie_cap;

	pm_cap = pci_find_capability(dev->pci, PCI_CAP_ID_PM);
	msi_cap = pci_find_capability(dev->pci, PCI_CAP_ID_MSI);
	pcie_cap = pci_find_capability(dev->pci, PCI_CAP_ID_EXP);

	seq_printf(m, "  caps       : PM=%#x MSI=%#x PCIe=%#x\n",
		   pm_cap, msi_cap, pcie_cap);

	if (pm_cap) {
		u16 pmcsr = 0;

		pci_read_config_word(dev->pci, pm_cap + PCI_PM_CTRL, &pmcsr);
		seq_printf(m, "  pm csr     : %04x\n", pmcsr);
	}

	if (msi_cap) {
		u16 msi_flags = 0;

		pci_read_config_word(dev->pci, msi_cap + PCI_MSI_FLAGS, &msi_flags);
		seq_printf(m,
			   "  msi flags  : %04x (enable=%u 64bit=%u multimsg_cap=%u multimsg_en=%u)\n",
			   msi_flags,
			   !!(msi_flags & PCI_MSI_FLAGS_ENABLE),
			   !!(msi_flags & PCI_MSI_FLAGS_64BIT),
			   (msi_flags & PCI_MSI_FLAGS_QMASK) >> 1,
			   (msi_flags & PCI_MSI_FLAGS_QSIZE) >> 4);
	}

	if (pcie_cap) {
		u16 pcie_flags = 0;
		u16 devctl = 0;
		u16 devsta = 0;
		u32 lnkcap = 0;
		u16 lnksta = 0;

		pcie_capability_read_word(dev->pci, PCI_EXP_FLAGS, &pcie_flags);
		pcie_capability_read_word(dev->pci, PCI_EXP_DEVCTL, &devctl);
		pcie_capability_read_word(dev->pci, PCI_EXP_DEVSTA, &devsta);
		pcie_capability_read_dword(dev->pci, PCI_EXP_LNKCAP, &lnkcap);
		pcie_capability_read_word(dev->pci, PCI_EXP_LNKSTA, &lnksta);

		seq_printf(m, "  pcie flags : %04x (ver=%u type=%u)\n",
			   pcie_flags,
			   pcie_flags & PCI_EXP_FLAGS_VERS,
			   (pcie_flags & PCI_EXP_FLAGS_TYPE) >> 4);
		seq_printf(m, "  pcie dev   : ctl=%04x sta=%04x\n",
			   devctl, devsta);
		seq_printf(m,
			   "  pcie link  : cap=%08x speed_cap=%u width_cap=x%u status=%04x speed=%u width=x%u\n",
			   lnkcap,
			   lnkcap & PCI_EXP_LNKCAP_SLS,
			   (lnkcap & PCI_EXP_LNKCAP_MLW) >> 4,
			   lnksta,
			   lnksta & PCI_EXP_LNKSTA_CLS,
			   (lnksta & PCI_EXP_LNKSTA_NLW) >> 4);
	}
}

static int mz0380_proc_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  device     : %s\n", pci_name(dev->pci));
		seq_printf(m, "  pci id     : %04x:%04x\n",
			   dev->pci->vendor, dev->pci->device);
		seq_printf(m, "  subsystem  : %04x:%04x\n",
			   dev->pci->subsystem_vendor,
			   dev->pci->subsystem_device);
		seq_printf(m, "  board      : %s\n",
			   mz0380_boards[dev->board].name);
		seq_printf(m, "  win driver : %s\n",
			   mz0380_boards[dev->board].windows_driver);
		seq_printf(m, "  firmware   : %s\n",
			   mz0380_boards[dev->board].firmware_name);
		seq_printf(m, "  bar%d start: 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_MMIO],
			   (unsigned long long)dev->bar_start[MZ0380_MAP_BAR_MMIO]);
		seq_printf(m, "  bar%d len  : 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_MMIO],
			   (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_MMIO]);
		seq_printf(m, "  bar%d start: 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_CFG],
			   (unsigned long long)dev->bar_start[MZ0380_MAP_BAR_CFG]);
		seq_printf(m, "  bar%d len  : 0x%llx\n",
			   dev->bar_nr[MZ0380_MAP_BAR_CFG],
			   (unsigned long long)dev->bar_len[MZ0380_MAP_BAR_CFG]);

		if (procfs_verbosity > 1)
			seq_puts(m, "  BAR scan disabled: wide MMIO reads proved unsafe on this hardware\n");
	}
	mutex_unlock(&devlist);

	return 0;
}

static int mz0380_proc_state_show(struct seq_file *m, void *v)
{
	struct mz0380_dev *dev;
	u16 pci_command = 0;
	u16 pci_status = 0;
	u8 pci_irq_line = 0;
	u8 pci_irq_pin = 0;

	mutex_lock(&devlist);
	list_for_each_entry(dev, &mz0380_devlist, devlist) {
		pci_read_config_word(dev->pci, PCI_COMMAND, &pci_command);
		pci_read_config_word(dev->pci, PCI_STATUS, &pci_status);
		pci_read_config_byte(dev->pci, PCI_INTERRUPT_LINE, &pci_irq_line);
		pci_read_config_byte(dev->pci, PCI_INTERRUPT_PIN, &pci_irq_pin);

		seq_printf(m, "%s\n", dev->name);
		seq_printf(m, "  rev        : %u\n", dev->pci_rev);
		seq_printf(m, "  latency    : %u\n", dev->pci_lat);
		seq_printf(m, "  irq        : %u\n", dev->pci->irq);
		seq_printf(m, "  pci cmd    : %04x\n", pci_command);
		seq_printf(m, "  pci status : %04x\n", pci_status);
		seq_printf(m, "  bus master : %s\n",
			   pci_command & PCI_COMMAND_MASTER ? "enabled" : "disabled");
		seq_printf(m, "  probe mode : %s\n",
			   allow_bus_master ? "bus-master enabled" : "probe-safe");
		seq_printf(m, "  irq line   : %02x\n", pci_irq_line);
		seq_printf(m, "  irq pin    : %02x\n", pci_irq_pin);
		mz0380_dump_pci_caps(m, dev);
		seq_printf(m, "  mmio[0x0000] = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_MMIO, 0x0000));
		seq_printf(m, "  mmio[0x0004] = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_MMIO, 0x0004));
		seq_printf(m, "  cfg[0x0000]  = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_CFG, 0x0000));
		seq_printf(m, "  cfg[0x0004]  = %08x\n",
			   mz_read(dev, MZ0380_MAP_BAR_CFG, 0x0004));
		if (procfs_verbosity > 1)
			mz0380_dump_pci_config(m, dev);
	}
	mutex_unlock(&devlist);

	return 0;
}

static int mz0380_proc_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_show, NULL);
}

static int mz0380_proc_state_open(struct inode *inode, struct file *filp)
{
	return single_open(filp, mz0380_proc_state_show, NULL);
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(4, 0, 0)
static struct file_operations mz0380_proc_fops = {
	.open = mz0380_proc_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};

static struct file_operations mz0380_proc_state_fops = {
	.open = mz0380_proc_state_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
};
#else
static struct proc_ops mz0380_proc_fops = {
	.proc_open = mz0380_proc_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};

static struct proc_ops mz0380_proc_state_fops = {
	.proc_open = mz0380_proc_state_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
};
#endif

static int mz0380_proc_create(void)
{
	struct proc_dir_entry *pe;

	pe = proc_create("mz0380", 0444, NULL, &mz0380_proc_fops);
	if (!pe)
		return -ENOMEM;

	pe = proc_create("mz0380-state", 0444, NULL, &mz0380_proc_state_fops);
	if (!pe) {
		remove_proc_entry("mz0380", NULL);
		return -ENOMEM;
	}

	return 0;
}

static int mz0380_initdev(struct pci_dev *pci_dev,
			  const struct pci_device_id *pci_id)
{
	struct mz0380_dev *dev;
	int err;

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	dev->pci = pci_dev;

	err = pci_enable_device(pci_dev);
	if (err) {
		kfree(dev);
		return err;
	}

	pci_clear_master(pci_dev);
	if (allow_bus_master)
		pci_set_master(pci_dev);
	pci_read_config_byte(pci_dev, PCI_CLASS_REVISION, &dev->pci_rev);
	pci_read_config_byte(pci_dev, PCI_LATENCY_TIMER, &dev->pci_lat);

	printk(KERN_INFO
	       "mz0380 device found at %s, rev: %u, irq: %u, latency: %u\n",
	       pci_name(pci_dev), dev->pci_rev, pci_dev->irq, dev->pci_lat);
	printk(KERN_INFO "%s: probe mode: %s\n",
	       pci_name(pci_dev),
	       allow_bus_master ? "bus master enabled" : "bus master disabled");

	err = mz0380_dev_setup(dev);
	if (err < 0)
		goto fail_disable;

	mz0380_card_setup(dev);
	pci_set_drvdata(pci_dev, dev);

	mutex_lock(&devlist);
	list_add_tail(&dev->devlist, &mz0380_devlist);
	mutex_unlock(&devlist);

	dprintk(1, "probe complete\n");
	return 0;

fail_disable:
	pci_disable_device(pci_dev);
	kfree(dev);
	return err;
}

static void mz0380_finidev(struct pci_dev *pci_dev)
{
	struct mz0380_dev *dev = pci_get_drvdata(pci_dev);

	if (!dev)
		return;

	mutex_lock(&devlist);
	list_del(&dev->devlist);
	mutex_unlock(&devlist);

	mz0380_dev_unregister(dev);
	pci_clear_master(pci_dev);
	pci_disable_device(pci_dev);
	kfree(dev);
}

static const struct pci_device_id mz0380_pci_tbl[] = {
	{
		.vendor = 0x12ab,
		.device = 0x0380,
		.subvendor = 0x1cfa,
		.subdevice = 0x0006,
	}, {
	}
};
MODULE_DEVICE_TABLE(pci, mz0380_pci_tbl);

static struct pci_driver mz0380_pci_driver = {
	.name = "mz0380",
	.id_table = mz0380_pci_tbl,
	.probe = mz0380_initdev,
	.remove = mz0380_finidev,
};

static int __init mz0380_init(void)
{
	int ret;

	printk(KERN_INFO "mz0380 driver version %d.%d.%d loaded\n",
	       (MZ0380_VERSION_CODE >> 16) & 0xff,
	       (MZ0380_VERSION_CODE >> 8) & 0xff,
	       MZ0380_VERSION_CODE & 0xff);

	ret = mz0380_proc_create();
	if (ret < 0)
		return ret;

	ret = pci_register_driver(&mz0380_pci_driver);
	if (ret < 0) {
		remove_proc_entry("mz0380", NULL);
		remove_proc_entry("mz0380-state", NULL);
		return ret;
	}

	return 0;
}

static void __exit mz0380_fini(void)
{
	remove_proc_entry("mz0380", NULL);
	remove_proc_entry("mz0380-state", NULL);
	pci_unregister_driver(&mz0380_pci_driver);
	printk(KERN_INFO "mz0380 driver unloaded\n");
}

module_init(mz0380_init);
module_exit(mz0380_fini);
