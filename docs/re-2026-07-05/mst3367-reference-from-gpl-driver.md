# MST3367 register reference (from GPL driver stoth68000/hdcapm)

Source: https://github.com/stoth68000/hdcapm  (`mst3367-drv.c`, `mst3367-common.h`,
`mst3367-drv.h`), GPL-v2, by Steven Toth / Michael Grzeschik. Reverse-engineered from the SAME
Elgato/Yuan MZ0380 Windows driver, for the Startech USB2HDCAPM (also Vatics Mozart 395s +
MST3367). Cross-validated against our own disassembly of `e60MZ0380.X64.SYS` — they agree.

## I2C + banking
- I2C address `0x9c` (8-bit) → `>> 1` → `0x4e` (7-bit).  [matches our finding exactly]
- Banks: `#define BANK0 0x00 / BANK1 0x01 / BANK2 0x02 / BANK3 0x03`.
- Bank select = write register `0x00` = bank number (only when it changes; driver caches
  `current_bank`).  [matches our page-select finding]
- 256-byte register shadow per bank.

## Init sequence (mst3367_init_setup)
```c
MST3367_TMDS_HOT_PLUG(sd, RX_TMDS_HPD_OFF);
mst3367_wr(BANK0, 0x41, 0x6f);
mst3367_wr(BANK0, 0xb8, 0x00);
mst3367_wr(BANK1, 0x0f, 0x02);
mst3367_wr(BANK1, 0x16, 0x30);
mst3367_wr(BANK1, 0x24, 0x40);   /* HDCP receive */
mst3367_wr(BANK0, 0xb0, 0x14);
mst3367_wr(BANK0, 0xb1, 0xe0);
mst3367_wr(BANK2, 0x01, 0x61);
mst3367_wr(BANK2, 0x02, 0xf5);   /* matches our extracted page2 0x02=0xf5 */
mst3367_wr(BANK0, 0x51, 0x89);
MST3367_TMDS_HOT_PLUG(sd, RX_TMDS_A_HPD_ON | RX_TMDS_A_LINK_ON);
mst3367_wr(BANK0, 0xB0, 0x20);   /* YUV422 / 8-bit output */
MST3367_HDMI_INIT(sd);
```

## Reset / HPD / HDCP
```c
/* HPD: BANK0 reg 0xB7 bit1 (clear bit1 = link/HPD on) */
MST3367_TMDS_HOT_PLUG: v=rd(BANK0,0xB7); v|=0x02; if(LINK_ON) v&=~0x02; wr(BANK0,0xB7,v); msleep(20);
enum hpt_e { RX_TMDS_HPD_OFF=0x00, RX_TMDS_A_HPD_ON=0x01, RX_TMDS_A_LINK_ON=0x02,
             RX_TMDS_B_HPD_ON=0x10, RX_TMDS_B_LINK_ON=0x20 };

MST3367_HDCP_RESET: wr(BANK0,0xb8,0x10); wr(BANK0,0xb8,0x00); msleep(20);   /* matches our 0xb8 10/00 */
MST3367_HDMI_RESET: wr(BANK2,0x07,0xf4); wr(BANK2,0x07,0x04); msleep(20);   /* matches our 0x07 f4/04 */
```

## Mode detect
```c
/* signal present if BANK0 reg 0x55 & 0x3c != 0  -- this is our R0055 */
if (mst3367_rd(BANK0, 0x55) & 0x3c) { ... extract timing ... }

/* BANK0 timing registers */
htotal   = rd(0x6a)<<8 | rd(0x6b);      /* 12-bit */
vtotal   = rd(0x5b)<<8 | rd(0x5c);      /* 11-bit */
hperiod_raw = rd(0x57)<<8 | rd(0x58);   /* 10-bit */
vperiod_raw = rd(0x59)<<8 | rd(0x5a);   /* 10-bit */
interleaved = rd(0x5f) & 0x02;          /* bit1 = interlaced */
/* BANK2 active pixels */
hactive  = rd(0x29)<<8 | rd(0x28);      /* 13-bit */

hperiod = 1600000 / hperiod_raw;
vperiod = 1250000 / vperiod_raw;
/* findVideoStandard(): match htotal/vtotal/hperiod/vperiod against the table below */
```

## Video-standard timing table (mst3367_video_standards[])
Columns: standard, htotal_min, htotal_max, vtotal_min, vtotal_max, hperiod_min, hperiod_max,
vperiod_min, vperiod_max, interleaved, encoded_fps, hdmi_fpsX100.
```
720x480p59.94   845  865   520  525  310 320  595 605  0 60 5994
1280x720p30    2300 2500   745  755  215 235  290 310  0 30 3000
1280x720p50    2965 2985   745  755  360 380  480 520  0 50 5000
1280x720p60    2470 2480   745  755  445 455  595 605  0 60 6000
1280x720p60    1645 1655   745  755  445 455  595 605  0 60 6000
1920x1080p24   4080 4105  1120 1130  260 280  230 250  0 24 2400
1920x1080p25   3950 3970  1120 1130  270 290  240 254  0 25 2500
1920x1080p30   2295 3305  1120 1130  330 345  290 310  0 30 3000
1920x1080p50   3950 3970  1120 1130  550 570  480 520  0 25 5000
1920x1080p60   3290 3310  1120 1130  665 685  595 605  0 30 6000
```
NOTE: a few fields look like upstream typos — 1080p30 htotal_max `3305` (likely `3295`),
1080p50 encoded_fps `25` (likely `50`), 1080p60 encoded_fps `30` (likely `60`). Sanity-check
against real captured `htotal/vtotal` before trusting. Table also omits 1080i/576i — extend as
needed from your own detect reads.

## Architecture takeaway
`mst3367-drv.c` is a self-contained V4L2 **i2c sub-device** driver that talks to the chip over a
standard `i2c_adapter` (`i2c_transfer`). To reuse it on the HD60 Pro (PCIe), provide an
`i2c_adapter` whose `master_xfer` tunnels each transaction through the card mailbox
(opcode 0x1b write / 0x1a read / 0x20 combo, dev 0x9c 8-bit — see main doc Appendix A). Then the
chip logic above works unchanged.
