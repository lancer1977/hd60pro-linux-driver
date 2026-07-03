/*
 *  Driver for MZ0380 based capture cards.
 *
 *  Firmware upload state machine.
 *
 *  The Elgato HD60 Pro is a "smart" capture card. Its YUAN MZ0380 PCIe
 *  endpoint boots its own embedded Linux on an onboard ARMv5 SoC. The
 *  blob we ship as /lib/firmware/mz0380/MZ0380.HD.HEX is a gzipped
 *  tar archive rooted at "yuan_demo_sdi/" containing ARM ELF binaries
 *  (tinyvenc, video_capture_mgr, etc.) plus an on-card kernel module
 *  named ep.ko that talks PCIe to us.
 *
 *  Upload protocol (RE-confirmed from e60MZ0380.X64.SYS, see RE_FINDINGS.md):
 *    - BEGIN_FIRMWARE_DOWNLOAD (opcode 0x0b): mailbox command with the
 *      total byte count as param0.
 *    - host then memcpy's the ENTIRE blob into the BAR0 aperture at
 *      MZ0380_MB_FW_BUFFER (0x60) as ascending 32-bit writes. There is
 *      no chunk/sequence/ack protocol.
 *    - COMMIT_FW (opcode 0x0c): mailbox command; the card reboots into
 *      the new firmware. Success is the mailbox result slot (BAR0+0x08)
 *      reading back 0.
 *
 *  The mailbox and firmware buffer are in BAR0, not BAR5 (the earlier
 *  BAR5 chunked-upload hypothesis was wrong).
 */

#include "mz0380.h"

#define MZ0380_FW_BOOT_TIMEOUT_MS    60000	/* boot observed at ~21 s */

static const struct firmware *current_fw;

const char *mz0380_fw_state_name(enum mz0380_fw_state s)
{
	switch (s) {
	case MZ0380_FW_STATE_NONE:      return "none";
	case MZ0380_FW_STATE_REQUESTED: return "requested";
	case MZ0380_FW_STATE_UPLOADING: return "uploading";
	case MZ0380_FW_STATE_READY:     return "ready";
	case MZ0380_FW_STATE_FAILED:    return "failed";
	}
	return "unknown";
}
EXPORT_SYMBOL_GPL(mz0380_fw_state_name);

/*
 * Copy the whole firmware blob into the BAR0 download aperture at
 * MZ0380_MB_FW_BUFFER (0x60), as 32-bit little-endian writes. This mirrors
 * the Windows driver, which memcpy's the entire blob into BAR0+0x60 between
 * the BEGIN and COMMIT mailbox commands - there is no chunk/seq/ack protocol
 * (see RE_FINDINGS.md).
 */
static void mz0380_fw_write_buffer(struct mz0380_dev *dev,
				   const u8 *data, size_t len)
{
	size_t i;

	for (i = 0; i + 4 <= len; i += 4)
		mz_mmio_write(dev, MZ0380_MB_FW_BUFFER + i,
			      get_unaligned_le32(data + i));

	if (i < len) {
		u8 tail[4] = { 0 };

		memcpy(tail, data + i, len - i);
		mz_mmio_write(dev, MZ0380_MB_FW_BUFFER + i,
			      get_unaligned_le32(tail));
	}

	wmb();
}

static int mz0380_fw_upload_blob(struct mz0380_dev *dev,
				 const struct firmware *fw)
{
	u32 params[1];
	u32 status;
	u32 result;
	int ret;

	dev->fw_state = MZ0380_FW_STATE_UPLOADING;

	/* 1. BEGIN_FIRMWARE_DOWNLOAD, param0 = total byte count */
	params[0] = (u32)fw->size;
	ret = mz0380_send_command(dev, MZ0380_CMD_BEGIN_FW_DL,
				  params, 1, &status, 2000);
	if (ret) {
		pr_err("%s: BEGIN_FW_DL command failed (%d)\n",
		       dev->name, ret);
		return ret;
	}

	/* 2. push the entire blob into the BAR0 aperture */
	mz0380_fw_write_buffer(dev, fw->data, fw->size);

	/*
	 * 3. COMMIT/EXECUTE - the card reboots into the new firmware and
	 * never posts a completion for this opcode, so fire-and-forget
	 * (timeout 0), exactly like the Windows driver.
	 */
	ret = mz0380_send_command(dev, MZ0380_CMD_COMMIT_FW,
				  NULL, 0, &status, 0);
	if (ret) {
		pr_err("%s: COMMIT_FW command failed (%d)\n",
		       dev->name, ret);
		return ret;
	}

	/*
	 * Success is signalled by the mailbox result slot (BAR0+0x08) going
	 * from the BEGIN size echo to 0 once the card has booted the new
	 * image (observed on hardware). Give it time to come up (Windows
	 * waits ~100 ms after commit), and keep draining/acking events on
	 * the way so the card is not left with INTx asserted.
	 */
	msleep(100);
	result = mz_mmio_read(dev, MZ0380_MB_RESULT);
	{
		unsigned long deadline =
			jiffies + msecs_to_jiffies(MZ0380_FW_BOOT_TIMEOUT_MS);

		while (time_before(jiffies, deadline)) {
			u32 event = mz_mmio_read(dev, MZ0380_MB_EVENT);

			/*
			 * Ack/rearm every tick like the Windows event thread
			 * does (unconditionally, 1 ms cadence) - it doubles
			 * as a host-alive rearm and keeps INTx deasserted.
			 */
			mz0380_mb_ack_event(dev);
			if (event)
				pr_info("%s: EVENT=0x%08x during fw boot wait, acked\n",
					dev->name, event);
			result = mz_mmio_read(dev, MZ0380_MB_RESULT);
			if (result == 0)
				break;
			msleep(1);
		}
	}

	if (result != 0) {
		pr_err("%s: firmware boot timed out (result=0x%08x)\n",
		       dev->name, result);
		return -ETIMEDOUT;
	}

	dev->fw_state = MZ0380_FW_STATE_READY;
	wake_up_all(&dev->fw_wait);
	return 0;
}

/*
 * M0 observability: read-only dump of firmware state plus the live CHECKME
 * mailbox/firmware-window registers. Surfacing the raw STATUS/COMMAND/SEQ/ACK
 * and PARAM[0..3] values in /proc/mz0380-state lets a firmware-upload attempt
 * be watched register-by-register without any writes. BAR5 is always mapped.
 */
void mz0380_fw_info_dump(struct seq_file *m, struct mz0380_dev *dev)
{
	unsigned int i;

	seq_printf(m, "  fw state   : %s\n", mz0380_fw_state_name(dev->fw_state));
	seq_printf(m, "  fw version : %u.%u\n",
		   dev->fw_version_major, dev->fw_version_minor);
	if (current_fw)
		seq_printf(m, "  fw blob    : %zu bytes\n", current_fw->size);
	else
		seq_puts(m, "  fw blob    : not loaded\n");

	/*
	 * Mailbox lives in BAR0 (RE-confirmed). Pre-boot BAR0 may read back
	 * 0xffffffff on the FPGA register file; the low mailbox region is what
	 * we watch during a firmware-upload attempt.
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
	 * With booted firmware the bridge register file is reachable via
	 * mailbox reg-read commands. Reg 0x12 bit0 = input signal present
	 * (safe to read; the read-to-clear IRQ regs 0x10/13/14/15 are NOT
	 * dumped here to avoid eating events).
	 */
	if (dev->fw_state == MZ0380_FW_STATE_READY) {
		u32 sig;

		if (mz0380_periph_read(dev, MZ0380_CHIP_BRIDGE,
				       MZ0380_BRIDGE_SIGNAL, &sig) == 0)
			seq_printf(m, "  hdmi signal: %s (bridge[0x12]=0x%08x)\n",
				   (sig & 1) ? "present" : "absent", sig);
		else
			seq_puts(m, "  hdmi signal: query failed\n");
	}
}
EXPORT_SYMBOL_GPL(mz0380_fw_info_dump);

int mz0380_firmware_query_version(struct mz0380_dev *dev)
{
	u32 status;
	int ret;

	ret = mz0380_send_command(dev, MZ0380_CMD_GET_FW_VERSION,
				  NULL, 0, &status, 500);
	if (ret)
		return ret;

	/*
	 * ep.ko prints "FIRMWARE VERSION: %d.%d" - param0 holds the
	 * packed major/minor or two adjacent params.
	 */
	dev->fw_version_major = (dev->cmd_last_param[0] >> 16) & 0xffff;
	dev->fw_version_minor = dev->cmd_last_param[0] & 0xffff;

	pr_info("%s: firmware version %u.%u\n",
		dev->name, dev->fw_version_major, dev->fw_version_minor);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_firmware_query_version);

int mz0380_firmware_load(struct mz0380_dev *dev)
{
	const char *fwname;
	const struct firmware *fw;
	int ret;

	mutex_lock(&dev->fw_lock);

	if (dev->fw_state == MZ0380_FW_STATE_READY) {
		mutex_unlock(&dev->fw_lock);
		return 0;
	}

	fwname = mz0380_boards[dev->board].firmware_name;
	if (!fwname) {
		mutex_unlock(&dev->fw_lock);
		return -ENODEV;
	}

	dev->fw_state = MZ0380_FW_STATE_REQUESTED;

	ret = request_firmware(&fw, fwname, &dev->pci->dev);
	if (ret) {
		pr_err("%s: request_firmware(%s) failed (%d)\n",
		       dev->name, fwname, ret);
		dev->fw_state = MZ0380_FW_STATE_FAILED;
		mutex_unlock(&dev->fw_lock);
		return ret;
	}

	pr_info("%s: firmware %s loaded, %zu bytes\n",
		dev->name, fwname, fw->size);
	current_fw = fw;

	if (!mz0380_firmware_upload_enabled) {
		pr_info("%s: firmware_upload=0, blob is loaded but not pushed to device\n",
			dev->name);
		mutex_unlock(&dev->fw_lock);
		return 0;
	}

	ret = mz0380_fw_upload_blob(dev, fw);
	if (ret) {
		dev->fw_state = MZ0380_FW_STATE_FAILED;
		release_firmware(fw);
		current_fw = NULL;
		mutex_unlock(&dev->fw_lock);
		return ret;
	}

	mz0380_firmware_query_version(dev);

	mutex_unlock(&dev->fw_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(mz0380_firmware_load);

void mz0380_firmware_release(struct mz0380_dev *dev)
{
	mutex_lock(&dev->fw_lock);
	if (current_fw) {
		release_firmware(current_fw);
		current_fw = NULL;
	}
	dev->fw_state = MZ0380_FW_STATE_NONE;
	mutex_unlock(&dev->fw_lock);
}
EXPORT_SYMBOL_GPL(mz0380_firmware_release);
