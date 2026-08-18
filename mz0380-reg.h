/*
 *  Driver for MZ0380 based capture cards (Elgato HD60 Pro family).
 *
 *  Register map - revised after cross-referencing the in-tree sc0710
 *  (Elgato 4K60 Pro Mk.2) driver and the YUAN AMESDK 1.1.0.202.0
 *  sample profiles.
 *
 *  Hardware overview:
 *    The MZ0380 board has two layers:
 *      1. A Xilinx FPGA bridge in front of an ARMv5 Mozart SoC. The
 *         SoC boots its own embedded Linux ("yuan_demo_sdi", project
 *         code SC5C0) from a firmware blob the host uploads at probe.
 *      2. On the host side, the Xilinx fabric exposes two PCIe BARs:
 *           BAR0 : 1 MiB MMIO  - Xilinx-shaped register window
 *                                (XDMA-style DMA controller +
 *                                 Xilinx AXI IIC at 0x3100 +
 *                                 HDMI status at 0x00a8..0x00e4)
 *           BAR5 : 4 KiB CFG   - SC5C0/SC540-class mailbox window
 *                                used both for firmware upload AND
 *                                runtime SDK property writes.
 *
 *    Crucial pre-vs-post-boot distinction:
 *
 *      Before firmware ready:
 *        - BAR0 reads return 0xffffffff (Xilinx fabric idle).
 *        - BAR5 mailbox is reachable: identity, FW buffer, FW chunk
 *          seq/ack, status. The .sys "MZ0380_DownloadBaseFirmware"
 *          and "MZ0380_DownloadFirmware" sequences run here.
 *      After firmware ready:
 *        - BAR0 wakes up. XDMA descriptor controller, AXI IIC,
 *          HDMI status registers become live.
 *        - BAR5 keeps acting as the property/control mailbox
 *          (the existing 7 confirmed prop offsets at 0x0040..0x0088
 *          continue to work).
 *
 *    For that reason this header partitions offsets into:
 *      Section A : BAR5 mailbox (pre-boot AND post-boot)
 *      Section B : BAR0 XDMA controller (post-boot only)
 *      Section C : BAR0 Xilinx AXI IIC (EDID, post-boot only)
 *      Section D : BAR0 HDMI signal-status window (post-boot only)
 *
 *  Cross-references:
 *    - sc0710-reg.h               BAR0/BAR1 in the 4K60 Pro Mk.2
 *    - sc0710-dma-channel.c       XDMA register offsets (base+0x04..+0x88)
 *    - sc0710-dma-chain.c         8-DWORD descriptor format
 *    - sc0710-i2c.c               AXI IIC at BAR0 0x3100..0x310c
 *    - yuan_demo_sdi/drivers/ep.ko  device-side BAR layout strings
 *    - e60MZ0380.X64.SYS strings  MZ0380_SEND_COMMAND, BEGIN_FIRMWARE_DOWNLOAD
 */

#ifndef _MZ0380_REG_H_
#define _MZ0380_REG_H_

/* ---- BAR identification --------------------------------------------- */

#define MZ0380_BAR_MMIO        0   /* PCI BAR index                     */
#define MZ0380_BAR_CFG         5

#define MZ0380_MAP_BAR_MMIO    0   /* index into mz0380_dev->lmmio[]    */
#define MZ0380_MAP_BAR_CFG     1

#define MZ0380_BAR_MMIO_SIZE   0x100000
#define MZ0380_BAR_CFG_SIZE    0x1000

/* ====================================================================
 *  Section A : BAR5 mailbox window
 * ====================================================================
 */

/* ---- Confirmed SDK-property param fields (correlation-verified) ---- */

#define MZ0380_REG_PROP_INPUT          0x0040  /* mask 0x7    */
#define MZ0380_REG_PROP_RECORD_MODE    0x0058  /* mask 0x3    */
#define MZ0380_REG_PROP_BITRATE        0x005c  /* full 32 bit */
#define MZ0380_REG_PROP_QUALITY        0x0060  /* full 32 bit */
#define MZ0380_REG_PROP_GOP            0x0080  /* full 32 bit */
#define MZ0380_REG_PROP_QP_STEP        0x0084  /* full 32 bit */
#define MZ0380_REG_PROP_B_FRAMES       0x0088  /* full 32 bit */

/* ==================================================================== *
 *  IMPORTANT: the command mailbox and firmware download buffer are NOT
 *  in BAR5 - they live in BAR0. This was confirmed by decompiling the
 *  Windows driver e60MZ0380.X64.SYS (see RE_FINDINGS.md). The device
 *  context maps ctx+0x108 -> BAR0 (mailbox + fw buffer) and
 *  ctx+0x110 -> BAR5 (DMA-pointer window + property cache). The former
 *  CHECKME BAR5 offsets (0x00c0.. and 0x0300/0x0400) were wrong; the
 *  mailbox definitions below are BAR0-relative and RE-confirmed.
 * ==================================================================== */

/* ---- Command mailbox - BAR0 (RE-confirmed) ------------------------- */

#define MZ0380_MB_DOORBELL             0x00    /* write MZ0380_MB_FIRE to trigger */
#define MZ0380_MB_OPCODE               0x04    /* PARAM0 slot = opcode            */
#define MZ0380_MB_RESULT               0x08    /* PARAM1 slot = result (0 == ok)  */
#define MZ0380_MB_PARAM(i)             (0x04 + ((i) * 4)) /* i=0 -> opcode slot   */
#define MZ0380_MB_STATUS               0x2c    /* bit0 = command done             */
#define MZ0380_MB_EVENT                0x30    /* interrupt/event status word      */
#define MZ0380_MB_EVT_PAYLOAD0         0x40    /* event payload words (DPC args)   */
#define MZ0380_MB_EVT_PAYLOAD1         0x44
#define MZ0380_MB_EVT_PAYLOAD2         0x48
#define MZ0380_MB_EVT_PAYLOAD3         0x4c    /* payload for EVENT[23:16] events  */
#define MZ0380_MB_FW_BUFFER            0x60    /* firmware blob aperture (BAR0)    */

#define MZ0380_MB_FIRE                 0x800   /* doorbell value to fire a command */
#define MZ0380_MB_INT_ACK              0x400   /* doorbell value acking an event   */
#define MZ0380_MB_RESET                MZ0380_MB_INT_ACK /* old name, same value   */
#define MZ0380_MB_STATUS_DONE          BIT(0)  /* MZ0380_MB_STATUS bit0            */
#define MZ0380_MB_STATUS_OK_STAMP      0xaaaaaaaa /* fw stamps STATUS on success   */
#define MZ0380_MB_STATUS_BOOT_STAMP    0xdddddddd /* fw stamps STATUS after boot   */
#define MZ0380_MB_EVENT_CMD_DONE       BIT(11) /* EVENT bit11 = command complete   */
#define MZ0380_MB_POLL_ITERS           50      /* SEND_COMMAND polls 0x32 times    */

/* BAR5 notify pointers the host must program before CMD_INIT
 * (FUN_140278bb0): physical BAR0 addresses of the mailbox slots. */
#define MZ0380_CFG_NOTIFY_PTR0         0x30    /* <- bar0 phys + 0x04              */
#define MZ0380_CFG_NOTIFY_PTR1         0x38    /* <- bar0 phys + 0x5f              */

/*
 * BAR5 side of the interrupt handshake (Windows event thread
 * FUN_140284380): card sets BAR5[0xdc]=1 on event; host ack sequence is
 *   BAR5[0xdc]=2; BAR0[0x30]=0; BAR0[0x00]=0x400;
 */
#define MZ0380_CFG_INT_FLAG            0xdc
#define MZ0380_CFG_INT_ACK_VAL         2

/* opcode plus ten arguments; the next word is EVENT and is never payload */
#define MZ0380_REG_PARAM_MAX           11

/* Opcodes (RE-confirmed from DownloadFirmware/DownloadBaseFirmware) */
#define MZ0380_CMD_BEGIN_FW_DL          0x0b  /* begin main (HD) firmware, param=size */
#define MZ0380_CMD_COMMIT_FW            0x0c  /* commit/execute main firmware         */
#define MZ0380_CMD_BEGIN_BASE_FW_DL     0x0e  /* begin base firmware, param=size      */
#define MZ0380_CMD_COMMIT_BASE_FW       0x0f  /* commit/execute base firmware         */

/*
 * Peripheral register file access (RE-confirmed, FUN_1402777e4 /
 * FUN_1402851cc): cmd {opcode, chip, reg, value}; read result lands in
 * PARAM3 (BAR0+0x10).
 */
#define MZ0380_CMD_I2C_READ_S           0x1e  /* M69: block read twin of 0x1f;
                                               * on NAK leaves the payload
                                               * UNTOUCHED (clean NAK detector,
                                               * unlike 0x1a which forces 0)   */
#define MZ0380_CMD_REG_READ             0x1a
#define MZ0380_CMD_REG_WRITE            0x1b

/* chip ids seen in the Windows driver */
#define MZ0380_CHIP_BRIDGE              0x90  /* FPGA/bridge register file        */
#define MZ0380_CHIP_ENCODER             0x50  /* CHECKME - poked in fw-boot retry */
#define MZ0380_CHIP_TVP5160             0xb8  /* analog front-end                 */

/* bridge (chip 0x90) registers used by the Windows Interrupt_Handler */
#define MZ0380_BRIDGE_IRQ_MAIN          0x10  /* read-to-clear main status        */
#define MZ0380_BRIDGE_SIGNAL            0x12  /* bit0 = input signal present      */
#define MZ0380_BRIDGE_IRQ_SRC0          0x13  /* per-source status (read-to-clear)*/
#define MZ0380_BRIDGE_IRQ_SRC1          0x14
#define MZ0380_BRIDGE_IRQ_SRC2          0x15
#define MZ0380_BRIDGE_CTL18             0x18  /* bit7 toggled as a reset strobe   */
#define MZ0380_BRIDGE_CTL19             0x19  /* event ack/mask (bits 0x30 kept)  */

/* Post-boot handshake (RE-confirmed, FUN_140278bb0) */
#define MZ0380_CMD_INIT                 0x01  /* hello/init, no params, retried    */
#define MZ0380_CMD_GET_BOARD_VERSION    0x0a  /* STATUS=0xaaaaaaaa on success,
					       * running fw version -> PARAM 0x08/0x0c */

/*
 * Opcodes RE-confirmed from ep.ko disassembly (see RE_FINDINGS.md M6/M7).
 * SET_VIC_PARAMS is the host->card "select input + declare video standard"
 * command: field layout (byte offsets in the firmware command buffer, which
 * the mailbox packs as opcode->0x04, params->0x08..):
 *   cmd[5]=fps  cmd[6]=input-code  cmd[8:9]=width(u16)  cmd[10:11]=height(u16)
 *   cmd[0x22]=int_reduce flag. width==0||height==0 -> firmware sets no_signal.
 */
#define MZ0380_CMD_SET_VIC_PARAMS       0x29  /* 41: select input + WxH/fps    */
#define MZ0380_CMD_SET_ENC_PARAMS       0x2d  /* 45: masked H.264 encoder config */
#define MZ0380_CMD_GPIO_READ            0x14  /* 20: read GPIO bitmap          */
#define MZ0380_CMD_GPIO_SET             0x15  /* 21: set GPIO data (prop 941)  */
#define MZ0380_CMD_GPIO_DIR             0x17  /* 23: set GPIO direction (940)  */

/* pattern-check: skip register/pin #define constants, no types or behavior */

/*
 * GPIO pin map (RE_FINDINGS.md M14, from e60MZ0380.X64.SYS disasm). op 0x15
 * (GPIO_SET) is single-pin: PARAM1 = mask = (1<<pin), PARAM2 = data =
 * (level<<pin). Direction is never set over the mailbox (pins are pre-config'd
 * outputs), so there is no GPIO_DIR step. pin9 held low at power-up is what
 * kept the MST3367 in reset and the I2C bus dead (M11-M15).
 */
#define MZ0380_GPIO_HPD                 1     /* HDMI hot-plug detect. M69:
                                               * ACTIVE-LOW on the Elgato board:
                                               * win64 FUN_14024eeb8 computes
                                               * pin1 = ~(arg>>4)&1, so HPD_ON
                                               * drives the pin LOW. We had it
                                               * inverted for the whole project. */
#define MZ0380_GPIO_EDID_MUX            2     /* M69: DDC path mux, only ever
                                               * touched by the Windows EDID
                                               * handler: 1 = local EDID store
                                               * on the SoC's i2c-0, 0 = the
                                               * passthrough/monitor side.     */
#define MZ0380_GPIO_RX_ENABLE           3     /* receiver / mux enable (=1)    */
#define MZ0380_GPIO_RX_STRAP            8     /* companion reset/power strap   */
#define MZ0380_GPIO_RX_RESET            9     /* MST3367 reset, ACTIVE-LOW     */

/*
 * MST3367 HDMI receiver over the mailbox I2C proxy (op 0x1a read / 0x1b write).
 * 8-bit device address; the card firmware shifts it >>1 to 7-bit 0x4e. Register
 * space is banked: write reg 0x00 = bank before touching a banked register.
 * Register roles + detect math: RE_FINDINGS.md M15 + docs/re-2026-07-05/.
 */
/*
 * EDID bulk write, confirmed in both e60MZ0380.X64.SYS and yuan_ioctrl.
 * Opcode 0x1f consumes this packed command:
 *   cmd[4] = dev8, cmd[5] = EEPROM offset, cmd[6..7] = payload length,
 *   cmd[8..] = payload bytes.
 * The Windows driver writes 32-byte chunks and waits synchronously for the
 * shared STATUS completion before reusing the mailbox.  Opcode 0x20 is a
 * different generic combo transaction whose byte 5 is a direction flag.
 */
#define MZ0380_CMD_I2C_WRITE_S          0x1f
#define MZ0380_EDID_I2C_DEV             0xa0
#define MZ0380_EDID_CHUNK               32
#define MZ0380_EDID_SIZE                256

#define MST3367_I2C_DEV                 0x9c
#define MST3367_REG_BANK_SELECT         0x00
#define MST3367_BANK0                   0x00
#define MST3367_BANK1                   0x01
#define MST3367_BANK2                   0x02
/*
 * BANK3 exists per the GPL driver ("256-byte register shadow per bank",
 * BANK0..BANK3) but neither that driver nor our RE ever writes it. On
 * hardware it reads all zeros apart from the bank-select echo at reg 0x00,
 * while banks 0-2 are full of live config - the shape of an EDID RAM that
 * has never been loaded (M44).
 */
#define MST3367_BANK3                   0x03
/*
 * BANK0 0xb7 is the receiver's HPD/link enable (hdcapm: bit1 clear = HPD on).
 * The init sequence parks it at 0x02 (off) while configuring; the driver
 * raises HPD only after the EDID is in place.
 */
#define MST3367_B0_HPD                  0xb7
#define MST3367_B0_HPD_ON               0x00
#define MST3367_B0_HPD_OFF              0x02
/* BANK0 mode-detect block */
#define MST3367_B0_DETECT               0x55  /* usable lock iff mask == 0x3c   */
#define MST3367_B0_DETECT_LOCK_MASK     0x3c
/*
 * M67: bit 0x20 flaps independently of a usable lock, so the bits that must
 * ALL be set for the timing block to be readable are 0x04|0x08|0x10. Evidence
 * from every R55 this project has logged:
 *
 *   0x3f 0x5f 0x7f          -> timing block good (0x5f carried a textbook
 *                              1080p60, and 0x5f fails a 0x3c test)
 *   0xa3 0x23 0x27 0xc7 0x2b -> torn counters (vtot=5, htot=656, ...)
 *   0x83 0x03               -> no signal
 *
 * 0x1c classifies all ten correctly; 0x3c rejects good data whenever 0x20
 * happens to be low, which it was on three consecutive runs - and it can drop
 * BETWEEN the poll's read and the measurement's own re-read.
 */
#define MST3367_B0_DETECT_LOCK_CORE     0x1c
#define MST3367_B0_HPERIOD_HI           0x57  /* hperiod = 1600000/(hi<<8|lo)  */
#define MST3367_B0_HPERIOD_LO           0x58
#define MST3367_B0_VPERIOD_HI           0x59  /* vperiod = 1250000/(hi<<8|lo)  */
#define MST3367_B0_VPERIOD_LO           0x5a
#define MST3367_B0_VTOTAL_HI            0x5b
#define MST3367_B0_VTOTAL_LO            0x5c
#define MST3367_B0_INTERLACE            0x5f  /* HD60 Pro: bit3 = interlaced   */
#define MST3367_B0_INTERLACE_BIT        0x08
#define MST3367_B0_HTOTAL_HI            0x6a
#define MST3367_B0_HTOTAL_LO            0x6b
#define MST3367_B0_AUTO_POSITION        0xe2
#define MST3367_B0_AUTO_POSITION_ON     0x80
#define MST3367_B0_AUTO_POSITION_OFF    0x00
/* BANK2 active-pixel counter */
#define MST3367_B2_HACTIVE_LO           0x28  /* hactive = (0x29<<8)|0x28      */
#define MST3367_B2_HACTIVE_HI           0x29

/* Input codes for SET_VIC_PARAMS cmd[6] (QCAP qcap.h). */
#define MZ0380_INPUT_CODE_COMPOSITE     0
#define MZ0380_INPUT_CODE_HDMI          2
#define MZ0380_INPUT_CODE_DVI_D         3
#define MZ0380_INPUT_CODE_COMPONENT     4
#define MZ0380_INPUT_CODE_SDI           6
#define MZ0380_INPUT_CODE_AUTO          7

/*
 * START_STREAMING = mailbox opcode 0x06 (RE_FINDINGS M22, on-card RE).
 * SET_VIC_PARAMS (0x29) alone does NOT start frames: it makes video_capture_mgr
 * spawn tinyvenc5 (with the argv derived from the 44-byte SET_VIC command) and
 * clears no_signal, but tinyvenc5 then blocks reading /sys/vpl_pciep/epint for a
 * *separate* START_STREAMING command before it begins the per-frame host DMA
 * (channel_done pwrite -> ep.ko MSI + outbound ATU). tinyvenc5's command
 * dispatch (jump table indexed by cmd-6) maps command 6 to that start block;
 * ep.ko's op6 ISR handler (@0x1854) sysfs_notify()s epint whenever no_signal==0,
 * which is exactly the wake tinyvenc5 waits for. Op6 carries no payload.
 * (M6's "op6 = enc-status+notify" saw the ISR side but not the tinyvenc5
 * consumer, and so wrongly concluded no start opcode existed.)
 */
#define MZ0380_CMD_START_STREAMING      0x06  /* tinyvenc5 START (epint cmd 6) */

/* pattern-check: skip streaming register/opcode #define constants (M17) */
/*
 * Streaming/DMA protocol (RE_FINDINGS.md M17, both-sides RE). The card DMAs
 * encoded frames into host buffers via a PCIe iATU outbound window it programs
 * from host-supplied physical addresses. The host hands those addresses over
 * with the config-setter opcodes below (12-word command: opcode, channel,
 * stride, then up to 4 {phys_hi, phys_lo} pairs), arms with SET_VIC_PARAMS
 * (0x29), and stops with 0x07. Frame completion arrives as an EVENT (BAR0+0x30)
 * with the buffer token in BAR0+0x40.
 */
#define MZ0380_CMD_SET_BUF_2            0x02  /* buffer phys addrs (primary)   */
#define MZ0380_CMD_SET_BUF_3            0x03
#define MZ0380_CMD_SET_BUF_4            0x04
#define MZ0380_CMD_SET_BUF_5            0x05
#define MZ0380_CMD_SET_BUF_8            0x08
/*
 * M33, from video_capture_mgr's own dispatch (it preads the 44-byte command and
 * switches on word 0):
 *     cmp #7  -> STOP_STREAMING     cmp #41 (0x29) -> SET_VIC
 *     cmp #42 -> SET_AIC_PARAMS
 * So STOP is op 7 - right next to START (op 6) - and 0x2a, which M17 guessed
 * was STOP, is actually the audio-parameter command. We had been sending
 * SET_AIC with an all-zero payload (i.e. "audio off") at every streamoff, and
 * never issuing a stop at all.
 */
#define MZ0380_CMD_STOP_STREAMING       0x07  /* was 0x2a (wrong) - M33        */

/*
 * SET_AIC_PARAMS. Field offsets recovered from video_capture_mgr's printf
 * ("Set AIC PARAMS-> bits, channel_num, mono, freq, frame_num_of_period,
 * period_num_of_buffer, on") by following the ARM vararg registers/stack, and
 * cross-checked against the Windows driver's "ai=%d, chs=%d, bits=%d, freq=%d,
 * period=%d.%d, is_aic_on=%d, aic_int_mode=%d". Offsets are into the card's
 * command buffer, where cmd+N == BAR0 + 4 + N == our PARAM((N/4)):
 *     cmd+4  u8  channel_num       cmd+5  u8  mono
 *     cmd+6  u16 bits              cmd+8  u32 freq
 *     cmd+12 u16 frame_num_of_period
 *     cmd+14 u16 period_num_of_buffer
 *     cmd+16 u8  on                cmd+17 u8  aic_int_mode (ep.ko G[0x63c])
 * on=1 makes video_capture_mgr run "echo '1' > /sys/audio_status/audio_ready",
 * which is what tinyvenc5's is_nosg path blocks on before it will ACK
 * START_STREAMING and start completing frames (M33).
 */
#define MZ0380_CMD_SET_AIC_PARAMS       0x2a  /* 42: audio params + on flag    */

/* BAR0 frame-completion status window (M17). EVENT is MZ0380_MB_EVENT (0x30). */
#define MZ0380_MB_FRAME_TOKEN           0x40  /* (token & 7) = buffer index    */
#define MZ0380_MB_ENC_STATUS            0x50  /* +N = per-channel status byte  */
/*
 * M40: that byte is the card's /sys/vpl_pciep/enc_stat<idx> (idx = ch*2 +
 * stream), and it is a HANDSHAKE, not a status report. Per frame the encoder
 * preads it: 0 = "host has consumed the last bitstream, encode another",
 * 1 = busy (retries 10x then skips the frame), 2 = skip. After DMAing a
 * bitstream the CARD writes 1 - and nothing card-side ever writes it back to
 * 0. Only the host can (ep.ko's encode_status_storeN backs this same byte),
 * so the host must clear it to acknowledge each consumed frame or the encoder
 * stops producing after one.
 */
#define MZ0380_MB_ENC_STAT_IDX(ch, stream)  ((ch) * 2 + (stream))
#define MZ0380_MB_ENC_STAT_FREE         0     /* host ack: buffer consumed     */

/* First-cut streaming geometry (video channel 0). Tunable once frames flow. */
#define MZ0380_STREAM_VIDEO_CHANNEL     0
#define MZ0380_STREAM_NR_BUFS           4     /* 4 phys pairs per SET_BUF cmd  */
/*
 * M29: 512 KiB was too small - the card streamed straight past the end of the
 * buffer and the overrun was what produced every IOMMU fault from M26 on (the
 * writes INSIDE the buffer had been succeeding silently all along). 4 MiB is
 * one alloc_pages order-10 block, and comfortably over an uncompressed
 * 1920x1080 NV12 frame (~3.1 MiB) as well as any sane H.264 access unit. The
 * card's outbound aperture is 32 MiB total, so 4 x 4 MiB still fits.
 */
#define MZ0380_STREAM_BUF_SIZE          0x400000 /* 4 MiB per frame buffer     */
#define MZ0380_STREAM_BUF_STRIDE        MZ0380_STREAM_BUF_SIZE

/*
 * M36/M37 (proven on hw): the fake-frame path lands ONE fully contiguous raw
 * burst of exactly 1920 x 1107 x 1.5 bytes (0x30a5c0) in buf0 within 450 ms
 * of START, then the card's encoder loop parks (M39: a fresh spawn yields
 * exactly one more frame).
 *
 * M50 (decoded from a delivered capture): the burst SIZE is 1920x1107-based,
 * but the PICTURE inside is NV12 720x1080 - the fake-frame renderer draws
 * its "NO SIGNAL" splash (spinner + text, 0x11 background) at 720 wide no
 * matter what width SET_VIC carried. Layout: Y 720x1080 @0, UV 720x540
 * @0x11cc40*2/3 (=0xbdd80, all 0x80 = neutral chroma), 0xff junk fill to
 * the end of the burst. So the deliverable payload is the leading 0x11cc40
 * bytes.
 */
#define MZ0380_STREAM_RAW_WIDTH         1920
#define MZ0380_STREAM_RAW_HEIGHT        1107
#define MZ0380_STREAM_RAW_FRAME_SIZE \
	(MZ0380_STREAM_RAW_WIDTH * MZ0380_STREAM_RAW_HEIGHT * 3 / 2)
#define MZ0380_NOSG_NV12_WIDTH          720
#define MZ0380_NOSG_NV12_HEIGHT         1080
#define MZ0380_NOSG_NV12_SIZEIMAGE \
	(MZ0380_NOSG_NV12_WIDTH * MZ0380_NOSG_NV12_HEIGHT * 3 / 2)

/*
 * Disproven guesses (absent from the ep.ko dispatcher): SET_AIC=0x11,
 * QUERY_SIGNAL=0x20. There is no card->host signal query.
 */
#define MZ0380_CMD_RESET                0xFF  /* CHECKME */

/*
 * Firmware download (RE-confirmed): there is NO chunked seq/ack protocol.
 * The whole blob is written word-by-word into the BAR0 aperture at
 * MZ0380_MB_FW_BUFFER, bracketed by BEGIN and COMMIT mailbox commands.
 */

/* ====================================================================
 *  Section B : BAR0 XDMA controller (post-firmware)
 *
 *  The sc0710 driver uses a Xilinx XDMA Subsystem layout per channel:
 *    base+0x04  reg_dma_control       (read/write control)
 *    base+0x08  reg_dma_control_w1s   (write-1-to-set)
 *    base+0x0c  reg_dma_control_w1c   (write-1-to-clear)
 *    base+0x40  reg_dma_status1
 *    base+0x44  reg_dma_status2
 *    base+0x48  reg_dma_completed_descriptor_count
 *    base+0x88  reg_dma_poll_wba_l
 *    base+0x8c  reg_dma_poll_wba_h
 *  and a paired scatter-gather block at base+0x4000:
 *    sg_base+0x80  reg_sg_start_l
 *    sg_base+0x84  reg_sg_start_h
 *    sg_base+0x88  reg_sg_adj
 *    sg_base+0x8c  reg_sg_credits
 *
 *  mz0380 has only one video + one audio channel. Guessed base addrs
 *  are 0x0000 (video) and 0x0100 (audio). The sc0710 driver uses
 *  larger separations; refine empirically.
 * ====================================================================
 */

#define MZ0380_XDMA_VIDEO_BASE          0x0000  /* CHECKME */
#define MZ0380_XDMA_AUDIO_BASE          0x0100  /* CHECKME */

#define MZ0380_XDMA_CTRL                0x04
#define MZ0380_XDMA_CTRL_W1S            0x08
#define MZ0380_XDMA_CTRL_W1C            0x0c
#define MZ0380_XDMA_STATUS1             0x40
#define MZ0380_XDMA_STATUS2             0x44
#define MZ0380_XDMA_CDC                 0x48  /* completed descriptor count */
#define MZ0380_XDMA_POLL_WBA_L          0x88
#define MZ0380_XDMA_POLL_WBA_H          0x8c

#define MZ0380_XDMA_SG_OFFSET           0x4000
#define MZ0380_XDMA_SG_START_L          0x80
#define MZ0380_XDMA_SG_START_H          0x84
#define MZ0380_XDMA_SG_ADJ              0x88
#define MZ0380_XDMA_SG_CREDITS          0x8c

#define MZ0380_XDMA_CTRL_RUN            BIT(0)

/*
 * Xilinx XDMA descriptor (8 DWORDs). Linked-list scatter-gather.
 */
struct mz0380_xdma_descriptor {
	__le32 control;      /* MAGIC + flags                       */
	__le32 length_bytes;
	__le32 src_l;
	__le32 src_h;
	__le32 dst_l;
	__le32 dst_h;
	__le32 next_l;
	__le32 next_h;
} __packed;

#define MZ0380_XDMA_DESC_MAGIC          0xad4b0000   /* sc0710 ref */
#define MZ0380_XDMA_DESC_STOP           BIT(0)
#define MZ0380_XDMA_DESC_COMPLETED      BIT(1)
#define MZ0380_XDMA_DESC_EOP            BIT(4)

/* ====================================================================
 *  Section C : BAR0 Xilinx AXI IIC (post-firmware)
 *
 *  Standard Xilinx AXI IIC IP control block. Used for EDID read/write
 *  on the HDMI input port. Layout copied from sc0710-i2c.c.
 * ====================================================================
 */

#define MZ0380_AXI_IIC_CR               0x3100  /* control register   */
#define MZ0380_AXI_IIC_SR               0x3104  /* status register    */
#define MZ0380_AXI_IIC_TX_FIFO          0x3108
#define MZ0380_AXI_IIC_RX_FIFO          0x310c
#define MZ0380_AXI_IIC_GIE              0x3120  /* global int enable  */

#define MZ0380_AXI_IIC_CR_ENABLE        BIT(0)
#define MZ0380_AXI_IIC_CR_TX_FIFO_RESET BIT(1)

#define MZ0380_AXI_IIC_TX_FIFO_START    BIT(8)  /* start bit  */
#define MZ0380_AXI_IIC_TX_FIFO_STOP     BIT(9)  /* stop bit   */

/* ====================================================================
 *  Section D : BAR0 HDMI signal status (post-firmware)
 *
 *  Inferred from sc0710 register journal:
 *    0x00c4   00f0000 idle / streaming flag
 *    0x00c8   source height (e.g. 0x438 == 1080)
 *    0x00a8   source width (in upper 16 bits)
 *    0x00d0   00004100 idle, 00004101 streaming
 *    0x00d4   source format hash
 *    0x00d8   horizontal blanking (or similar)
 *    0x00e4   0=idle 1=streaming
 *
 *  mz0380 may surface the same fields once firmware brings BAR0 up.
 * ====================================================================
 */

#define MZ0380_REG_HDMI_WIDTH           0x00a8
#define MZ0380_REG_HDMI_DETECT_A        0x00ac
#define MZ0380_REG_HDMI_CONTROL         0x00c4
#define MZ0380_REG_HDMI_HEIGHT          0x00c8
#define MZ0380_REG_HDMI_STATUS_A        0x00d0
#define MZ0380_REG_HDMI_FORMAT_HASH     0x00d4
#define MZ0380_REG_HDMI_BLANK           0x00d8
#define MZ0380_REG_HDMI_STREAMING       0x00e4

/* ====================================================================
 *  Interrupt status/mask (CHECKME)
 *
 *  ep.ko references "pciep_isr_clrint" and "msi_enable" but the host-
 *  visible interrupt register layout is not yet known. Hypothesised
 *  block lives in BAR0 above the HDMI status window.
 * ====================================================================
 */

#define MZ0380_REG_IRQ_STATUS           0x0100  /* CHECKME (BAR0) */
#define MZ0380_REG_IRQ_MASK             0x0104  /* CHECKME (BAR0) */
#define MZ0380_REG_IRQ_ACK              0x0108  /* CHECKME (BAR0) */

#define MZ0380_IRQ_CMD_COMPLETE         BIT(0)
#define MZ0380_IRQ_VIDEO_RING_READY     BIT(1)
#define MZ0380_IRQ_AUDIO_RING_READY     BIT(2)
#define MZ0380_IRQ_SIGNAL_CHANGE        BIT(3)
#define MZ0380_IRQ_FW_READY             BIT(4)
#define MZ0380_IRQ_ERROR                BIT(5)

/* ====================================================================
 *  Ring base/size/head/tail (legacy mailbox - kept for now)
 *
 *  Two conflicting models:
 *    Model A (mailbox):  ring base programmed via BAR5 0x0200..0x0234.
 *                        Card pushes 4 KiB-aligned descriptors of the
 *                        custom flags+len+pts format below.
 *    Model B (XDMA):     ring is an XDMA descriptor table, programmed
 *                        via SG_START_L/H on the BAR0 XDMA controller
 *                        above. Card walks the linked-list of 8-DWORD
 *                        descriptors and pokes completion counter.
 *
 *  We keep Model A definitions for the bring-up scaffolding in
 *  mz0380-dma.c. If correlation testing shows no movement, switch to
 *  Model B by reprogramming via XDMA_SG_START_*.
 * ====================================================================
 */

#define MZ0380_REG_VIDEO_RING_BASE_LO   0x0200  /* CHECKME (BAR5) */
#define MZ0380_REG_VIDEO_RING_BASE_HI   0x0204
#define MZ0380_REG_VIDEO_RING_SIZE      0x0208
#define MZ0380_REG_VIDEO_RING_ENTRIES   0x020c
#define MZ0380_REG_VIDEO_RING_HEAD      0x0210
#define MZ0380_REG_VIDEO_RING_TAIL      0x0214

#define MZ0380_REG_AUDIO_RING_BASE_LO   0x0220  /* CHECKME (BAR5) */
#define MZ0380_REG_AUDIO_RING_BASE_HI   0x0224
#define MZ0380_REG_AUDIO_RING_SIZE      0x0228
#define MZ0380_REG_AUDIO_RING_ENTRIES   0x022c
#define MZ0380_REG_AUDIO_RING_HEAD      0x0230
#define MZ0380_REG_AUDIO_RING_TAIL      0x0234

/* Per-entry descriptor in our Model A ring (CHECKME) */
#define MZ0380_DESC_FLAGS_OFFSET        0x00
#define MZ0380_DESC_BYTECOUNT_OFFSET    0x04
#define MZ0380_DESC_PTS_LO_OFFSET       0x08
#define MZ0380_DESC_PTS_HI_OFFSET       0x0c
#define MZ0380_DESC_PAYLOAD_OFFSET      0x14

#define MZ0380_DESC_FLAG_KEY_FRAME      BIT(0)
#define MZ0380_DESC_FLAG_END_OF_STREAM  BIT(1)
#define MZ0380_DESC_FLAG_ERROR          BIT(2)

#endif /* _MZ0380_REG_H_ */
