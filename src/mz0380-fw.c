// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Card bring-up handshake. NO FIRMWARE IS EVER UPLOADED.
 *
 *  The Elgato HD60 Pro is a "smart" capture card: its YUAN MZ0380 PCIe
 *  endpoint boots its own embedded Linux from ONBOARD FLASH, on an ARMv5
 *  SoC, without any help from the host. That image carries the ARM binaries
 *  (tinyvenc, video_capture_mgr) plus the on-card kernel module ep.ko which
 *  serves the PCIe mailbox.
 *
 *  This driver deliberately does NOT push an image to the card. It is a
 *  standing project constraint, and it is also what the retail Windows
 *  driver does in practice - its DriverEntry logs
 *      TOTAL DOWNLOAD TIMES = 00000000 (6 BYTES) (SUCCESS) (1.11)
 *      BOARD VERSION: 1.11 / 1.11
 *  i.e. only the 6-byte version file is read, the versions match, and no
 *  image moves. The MZ0380.HD.HEX that used to ship with this tree was
 *  byte-identical to Elgato's shipping image (sha256 be0d5e19...), so there
 *  was never anything to gain by sending it - only ~21 s per insmod and a way
 *  to brick the card.
 *
 *  What this file does now:
 *    - read the expected version from the MZ0380.FW.TXT sidecar (there is
 *      no version-query opcode; this is where Windows gets it too),
 *    - handshake with the firmware already running on the card
 *      (mz0380_card_init: CMD_INIT + GET_BOARD_VERSION),
 *    - report any mismatch loudly and carry on with what is running.
 *
 *  Nothing here uploads, and no part of the upload protocol survives anywhere
 *  in the tree - not the opcodes, not the BAR0 aperture define, not the module
 *  parameter, not the .HEX blob names. Do not re-add any of it.
 *
 *  #56 EXCEPTION - fw_upload_path, opt-in, default OFF: the "card console"
 *  instrument needs to run a modified rootfs image (etc/rc.local redirecting
 *  daemon stdout to /mnt/flash/PIC_ENC) to get visibility into the on-card
 *  audio daemon, and the only way to get a modified image running is to
 *  upload it once. This re-adds the upload state machine this file's header
 *  used to forbid, but ONLY behind an explicit module parameter that
 *  defaults to empty/off - an unset fw_upload_path is byte-for-byte the same
 *  load path as before this change. The implementation is copied from the
 *  pre-removal upload code (commit 6b1d434, "feat: firmware upload and boot
 *  via BAR0 mailbox", opcodes/offsets RE-confirmed against the Windows
 *  driver there) rather than re-derived, so it carries the same verified
 *  BEGIN_FW_DL / memcpy-into-BAR0 / COMMIT_FW / ~21s-boot-wait behaviour.
 *  It still forces the same ~21s reboot and the same brick risk that got the
 *  original path deleted - see RUNBOOK.md before ever setting this param.
 */

#include "mz0380.h"
#include <linux/kernel_read_file.h>
#include <linux/jiffies.h>
#include <linux/vmalloc.h>

/*
 * #56 opt-in upload path. Default empty = OFF = no behaviour change at all.
 * Set to an absolute host filesystem path (not a /lib/firmware name - this
 * is read directly with kernel_read_file_from_path, not request_firmware,
 * because the console-instrument image lives in a build/scratch dir, not
 * the firmware search path) to upload that file to the card and reboot it
 * into it before the normal handshake proceeds.
 *
 * 0444: settable only at insmod time (module param, not sysfs-writable at
 * runtime) - an upload is a one-shot boot-time decision, not something to
 * flip live against a running card.
 */
static char *fw_upload_path = "";
module_param(fw_upload_path, charp, 0444);
MODULE_PARM_DESC(fw_upload_path,
	"#56 card-console instrument, OPT-IN, DEFAULT EMPTY (=off, no behaviour change): "
	"absolute host path to an MZ0380.HD.HEX-shaped image to upload via the "
	"BEGIN_FW_DL/COMMIT_FW mailbox opcodes at load time, exactly as the "
	"pre-removal upload path did (see mz0380-fw.c header, commit 6b1d434). "
	"DANGER: forces a ~21s card reboot and carries brick risk - this is why "
	"the original path was removed. Never set outside a deliberate "
	"console-instrument run with a verified /mnt/flash/yuan_demo_sdi_bak.");

/* Pre-removal upload protocol constants (RE-confirmed from e60MZ0380.X64.SYS,
 * see RE_FINDINGS.md and commit 6b1d434's log). Scoped to this file only -
 * not restored to mz0380-reg.h - so the opt-in nature of fw_upload_path is
 * visible from a single diff instead of spreading upload plumbing back
 * through the shared register header.
 */
#define MZ0380_FW_UPLOAD_MB_FW_BUFFER    0x60    /* firmware blob aperture (BAR0) */
#define MZ0380_FW_UPLOAD_CMD_BEGIN       0x0b    /* BEGIN_FIRMWARE_DOWNLOAD, param0 = byte count */
#define MZ0380_FW_UPLOAD_CMD_COMMIT      0x0c    /* COMMIT_FW: card reboots into the new image, no completion posted */
#define MZ0380_FW_UPLOAD_BOOT_TIMEOUT_MS 60000   /* boot observed at ~21s on hardware */

/*
 * Copy the whole firmware blob into the BAR0 download aperture as 32-bit
 * little-endian writes. This mirrors the Windows driver, which memcpy's the
 * entire blob into BAR0+0x60 between the BEGIN and COMMIT mailbox commands -
 * there is no chunk/seq/ack protocol (see RE_FINDINGS.md). Copied verbatim
 * (mz0380_fw_write_buffer) from the pre-removal implementation.
 */
static void mz0380_fw_upload_write_buffer(struct mz0380_dev *dev,
					  const u8 *data, size_t len)
{
	size_t i;

	for (i = 0; i + 4 <= len; i += 4)
		mz_mmio_write(dev, MZ0380_FW_UPLOAD_MB_FW_BUFFER + i,
			      get_unaligned_le32(data + i));

	if (i < len) {
		u8 tail[4] = { 0 };

		memcpy(tail, data + i, len - i);
		mz_mmio_write(dev, MZ0380_FW_UPLOAD_MB_FW_BUFFER + i,
			      get_unaligned_le32(tail));
	}

	wmb();
}

/*
 * BEGIN_FW_DL -> push blob -> COMMIT_FW -> wait for the card to reboot into
 * it. Copied from the pre-removal mz0380_fw_upload_blob(), same opcode
 * sequence, same fire-and-forget COMMIT (it never posts a completion), same
 * ack-every-tick boot wait.
 */
static int mz0380_fw_upload_blob(struct mz0380_dev *dev,
				 const void *data, size_t size)
{
	u32 params[1];
	u32 status;
	u32 result;
	int ret;
	unsigned long deadline;

	pr_warn("%s: fw_upload_path set - UPLOADING %zu bytes and forcing a card reboot (~21s). #56 console-instrument opt-in path, NOT standing driver behaviour.\n",
		dev->name, size);

	params[0] = (u32)size;
	ret = mz0380_send_command(dev, MZ0380_FW_UPLOAD_CMD_BEGIN,
				  params, 1, &status, 2000);
	if (ret) {
		pr_err("%s: fw_upload_path: BEGIN_FW_DL failed (%d)\n",
		       dev->name, ret);
		return ret;
	}

	mz0380_fw_upload_write_buffer(dev, data, size);

	ret = mz0380_send_command(dev, MZ0380_FW_UPLOAD_CMD_COMMIT,
				  NULL, 0, &status, 0);
	if (ret) {
		pr_err("%s: fw_upload_path: COMMIT_FW failed (%d)\n",
		       dev->name, ret);
		return ret;
	}

	msleep(100);
	result = mz_mmio_read(dev, MZ0380_MB_RESULT);
	deadline = jiffies + msecs_to_jiffies(MZ0380_FW_UPLOAD_BOOT_TIMEOUT_MS);
	while (time_before(jiffies, deadline)) {
		u32 event = mz_mmio_read(dev, MZ0380_MB_EVENT);

		mz0380_mb_ack_event(dev);
		if (event)
			pr_info("%s: fw_upload_path: EVENT=0x%08x during boot wait, acked\n",
				dev->name, event);
		result = mz_mmio_read(dev, MZ0380_MB_RESULT);
		if (result == 0)
			break;
		msleep(1);
	}

	if (result != 0) {
		pr_err("%s: fw_upload_path: boot timed out (result=0x%08x)\n",
		       dev->name, result);
		return -ETIMEDOUT;
	}

	pr_info("%s: fw_upload_path: image booted\n", dev->name);
	return 0;
}

const char *mz0380_fw_state_name(enum mz0380_fw_state s)
{
	switch (s) {
	case MZ0380_FW_STATE_NONE:      return "none";
	case MZ0380_FW_STATE_REQUESTED: return "requested";
	case MZ0380_FW_STATE_READY:     return "ready";
	case MZ0380_FW_STATE_FAILED:    return "failed";
	}
	return "unknown";
}
EXPORT_SYMBOL_GPL(mz0380_fw_state_name);

/*
 * M0 observability: read-only dump of card state plus the live mailbox
 * registers. Surfacing the raw STATUS/COMMAND/SEQ/ACK and PARAM[0..3] values
 * in /proc/mz0380-state lets the bring-up handshake be watched
 * register-by-register without any writes. BAR5 is always mapped.
 */
void mz0380_fw_info_dump(struct seq_file *m, struct mz0380_dev *dev)
{
	unsigned int i;

	seq_printf(m, "  fw state   : %s\n", mz0380_fw_state_name(dev->fw_state));
	seq_printf(m, "  fw version : %u.%u\n",
		   dev->fw_version_major, dev->fw_version_minor);
	seq_puts(m, "  fw image   : card's own flash - the host never uploads\n");

	/*
	 * Mailbox lives in BAR0 (RE-confirmed). Before the card has finished
	 * booting its own flash image, BAR0 may read back 0xffffffff on the
	 * FPGA register file; the low mailbox region is what we watch.
	 */
	seq_printf(m, "  bar0[DOORBELL 0x%02x] = %08x\n",
		   MZ0380_MB_DOORBELL, mz_mmio_read(dev, MZ0380_MB_DOORBELL));
	seq_printf(m, "  bar0[STATUS   0x%02x] = %08x\n",
		   MZ0380_MB_STATUS, mz_mmio_read(dev, MZ0380_MB_STATUS));
	seq_printf(m, "  bar0[RESULT   0x%02x] = %08x\n",
		   MZ0380_MB_RESULT, mz_mmio_read(dev, MZ0380_MB_RESULT));
	for (i = 0; i < 4; i++)
		seq_printf(m, "  bar0[PARAM%u   0x%02x] = %08x\n", i,
			   MZ0380_MB_PARAM(i), mz_mmio_read(dev, MZ0380_MB_PARAM(i)));

	/*
	 * Peripheral register probe over the (now working) mailbox. NOTE:
	 * the HDMI-signal register is NOT yet identified - reg 0x12 was a
	 * guess and reads 0 even with a source connected (the HD60 Pro's
	 * HDMI receiver is a different chip than the analog TVP5160 path).
	 * Dump a few candidate bridge regs read-only for RE correlation
	 * rather than asserting a signal state we cannot trust.
	 */
	/*
	 * M236: verbosity 3, not on every read.
	 *
	 * Each of these is a full mailbox command - write params, ring the
	 * doorbell, wait for the event bit, ack - and there are five of them.
	 * A status poll at 5 Hz therefore issued 25 mailbox transactions per
	 * second against a running 60 fps capture, which froze the machine for
	 * about a second at a time.
	 *
	 * The worse cost was to the evidence. A diagnostic that competes for
	 * the mailbox is a participant in what it measures, so recovery events
	 * and lock losses counted while polling cannot be attributed cleanly to
	 * the driver. Before/after comparisons at the same polling rate remain
	 * valid; absolute counts from those traces do not.
	 *
	 * The probe is labelled UNVERIFIED and exists for RE correlation, so it
	 * has no business on the path a watch script reads. Ask for it.
	 */
	if (dev->fw_state == MZ0380_FW_STATE_READY && procfs_verbosity > 2) {
		/*
		 * Non-clearing chip-0x90 regs only: the ISR treats 0x13/0x14/
		 * 0x15 (and re-reads 0x10) as read-to-clear, so cat'ing them
		 * from /proc would eat the card's signal-change events.
		 */
		static const u8 regs[] = { 0x11, 0x12, 0x16, 0x17, 0x8b };
		unsigned int i;
		u32 v;

		seq_puts(m, "  bridge probe (chip 0x90, UNVERIFIED):");
		for (i = 0; i < ARRAY_SIZE(regs); i++) {
			if (mz0380_periph_read(dev, MZ0380_CHIP_BRIDGE,
					       regs[i], &v) == 0)
				seq_printf(m, " [0x%02x]=%08x", regs[i], v);
			else
				seq_printf(m, " [0x%02x]=ERR", regs[i]);
		}
		seq_putc(m, '\n');
	}
}
EXPORT_SYMBOL_GPL(mz0380_fw_info_dump);

/*
 * Parse the "MM.mm" ASCII sidecar version file (MZ0380.FW.TXT). This is where
 * the Windows driver gets the EXPECTED firmware version from - there is no
 * version-query opcode. It is the only file this driver reads, and it is read
 * for comparison only.
 */
static bool mz0380_fw_parse_version(const u8 *d, size_t len,
				    u32 *major, u32 *minor)
{
	if (len < 5 || !isdigit(d[0]) || !isdigit(d[1]) || d[2] != '.' ||
	    !isdigit(d[3]) || !isdigit(d[4]))
		return false;
	*major = (d[0] - '0') * 10 + (d[1] - '0');
	*minor = (d[3] - '0') * 10 + (d[4] - '0');
	return true;
}

/*
 * Bring the card up. NOT an upload - see the file header. The card is already
 * running its own flash image by the time we probe; all we do is shake hands
 * with it, learn which version that is, and say so.
 */
int mz0380_firmware_load(struct mz0380_dev *dev)
{
	u32 want_major = 0, want_minor = 0;
	const struct firmware *vf;
	bool have_want = false;
	int ret;

	mutex_lock(&dev->fw_lock);

	if (dev->fw_state == MZ0380_FW_STATE_READY) {
		mutex_unlock(&dev->fw_lock);
		return 0;
	}

	dev->fw_state = MZ0380_FW_STATE_REQUESTED;

	/*
	 * Expected version, from the ASCII sidecar. Optional: its only job is
	 * to make a mismatch visible. There is no version-query opcode, so
	 * this file is also where the Windows driver gets the number it prints
	 * as "FIRMWARE VERSION".
	 */
	if (!firmware_request_nowarn(&vf, "mz0380/MZ0380.FW.TXT",
				     &dev->pci->dev)) {
		have_want = mz0380_fw_parse_version(vf->data, vf->size,
						    &want_major, &want_minor);
		release_firmware(vf);
	}

	/* Handshake with whatever the card booted: CMD_INIT + board version. */
	ret = mz0380_card_init(dev);
	if (ret) {
		pr_err("%s: card handshake failed (%d) - the mailbox is deaf. The card boots from its own flash; nothing here uploads firmware, so this is a card or link problem, not a missing image.\n",
		       dev->name, ret);
		dev->fw_state = MZ0380_FW_STATE_FAILED;
		mutex_unlock(&dev->fw_lock);
		return ret;
	}

	if (have_want && (dev->fw_version_major != want_major ||
			  dev->fw_version_minor != want_minor))
		pr_warn("%s: card runs firmware %u.%u but %s expects %u.%u - continuing with what the card has (this driver never uploads)\n",
			dev->name, dev->fw_version_major,
			dev->fw_version_minor, "MZ0380.FW.TXT",
			want_major, want_minor);
	else
		pr_info("%s: card runs firmware %u.%u (no upload attempted)\n",
			dev->name, dev->fw_version_major,
			dev->fw_version_minor);

	/*
	 * #56 opt-in only. fw_upload_path defaults to "" - this block is a
	 * no-op and every line above/below it behaves exactly as before this
	 * change when the param is unset.
	 */
	if (fw_upload_path && fw_upload_path[0]) {
		void *data = NULL;
		size_t size = 0;
		ssize_t rret;

		rret = kernel_read_file_from_path(fw_upload_path, 0, &data, 0,
						  &size, READING_FIRMWARE);
		if (rret < 0) {
			pr_err("%s: fw_upload_path=%s: read failed (%zd), NOT uploading - continuing with the card's own image\n",
			       dev->name, fw_upload_path, rret);
		} else {
			ret = mz0380_fw_upload_blob(dev, data, size);
			vfree(data);
			if (ret) {
				dev->fw_state = MZ0380_FW_STATE_FAILED;
				mutex_unlock(&dev->fw_lock);
				return ret;
			}

			/* Card just rebooted into the uploaded image - redo the handshake. */
			ret = mz0380_card_init(dev);
			if (ret) {
				pr_err("%s: fw_upload_path: post-upload handshake failed (%d)\n",
				       dev->name, ret);
				dev->fw_state = MZ0380_FW_STATE_FAILED;
				mutex_unlock(&dev->fw_lock);
				return ret;
			}
			pr_info("%s: fw_upload_path: post-upload card runs firmware %u.%u\n",
				dev->name, dev->fw_version_major,
				dev->fw_version_minor);
		}
	}

	dev->fw_state = MZ0380_FW_STATE_READY;
	mutex_unlock(&dev->fw_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_firmware_load);

void mz0380_firmware_release(struct mz0380_dev *dev)
{
	mutex_lock(&dev->fw_lock);
	dev->fw_state = MZ0380_FW_STATE_NONE;
	mutex_unlock(&dev->fw_lock);
}
EXPORT_SYMBOL_GPL(mz0380_firmware_release);
