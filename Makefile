#
# Linux driver for Elgato Game Capture HD60 Pro (YUAN MZ0380).
#
# Default target builds only mz0380.ko. The legacy sc0710 source set
# (4K60 Pro Mk.2) is preserved in the repo but no longer included in
# the default build because it does not compile on kernel 6.x (it
# depends on media/videobuf-vmalloc.h which was removed).
#
# To attempt the legacy build:
#     make legacy
#

mz0380-objs := \
	mz0380-cards.o \
	mz0380-core.o \
	mz0380-video.o \
	mz0380-mst3367.o \
	mz0380-dma.o \
	mz0380-fw.o \
	mz0380-audio.o

sc0710-objs := \
	sc0710-cards.o sc0710-core.o sc0710-i2c.o \
	sc0710-dma-channel.o sc0710-dma-channels.o \
	sc0710-dma-chains.o sc0710-dma-chain.o \
	sc0710-things-per-second.o sc0710-video.o \
	sc0710-audio.o

obj-m += mz0380.o

ifeq ($(MZ0380_LEGACY_SC0710),1)
obj-m += sc0710.o
endif

TARFILES = Makefile *.h *.c *.txt *.md

KVERSION = $(shell uname -r)
KBUILD   = /lib/modules/$(KVERSION)/build

#
# Toolchain auto-detect.
# CachyOS / Arch / Fedora ship a clang-built kernel; gcc cannot compile
# modules against it (rejects -mretpoline-external-thunk etc).
# If clang + ld.lld are present, use them; override on CLI to force.
#
ifeq ($(shell command -v clang >/dev/null && command -v ld.lld >/dev/null && echo yes),yes)
KBUILD_FLAGS ?= CC=clang LD=ld.lld
else
KBUILD_FLAGS ?=
endif

all:
	$(MAKE) -C $(KBUILD) M=$(PWD) $(KBUILD_FLAGS) modules

clean:
	$(MAKE) -C $(KBUILD) M=$(PWD) $(KBUILD_FLAGS) clean

legacy:
	$(MAKE) -C $(KBUILD) M=$(PWD) $(KBUILD_FLAGS) MZ0380_LEGACY_SC0710=1 modules

#
# Firmware install. The MZ0380.HD.HEX blob ships with the Windows
# driver and is not redistributable here. Copy it from the extracted
# installer (or from a Windows install) into /lib/firmware/mz0380/.
#
FW_DIR ?= /lib/firmware/mz0380
fw-install:
	@if [ ! -f $(FW_DIR)/MZ0380.HD.HEX ]; then \
		echo "ERROR: copy MZ0380.HD.HEX into $(FW_DIR) first."; \
		echo "       (extract from Game_Capture_HD60_Pro_*.exe with 7z)"; \
		exit 1; \
	fi
	@echo "firmware present in $(FW_DIR)"

#
# Load helpers.
#
load: all
	sudo dmesg -c >/dev/null
	sudo modprobe videobuf2-common
	sudo modprobe videodev
	sudo insmod ./mz0380.ko procfs_verbosity=2 enable_video=1

load-streaming: all fw-install
	sudo modprobe videobuf2-common
	sudo modprobe videodev
	sudo insmod ./mz0380.ko procfs_verbosity=2 \
		enable_video=1 \
		firmware_upload=1 \
		enable_dma=1 \
		dma_iova_remap=1 \
		aic_on=1

unload:
	sudo rmmod mz0380 || true
	sync

tarball:
	tar zcf ../mz0380-dev-$(shell date +%Y%m%d-%H%M%S).tgz $(TARFILES)

#
# Smoke tests (require an actual capture + HDMI source).
#
list:
	v4l2-ctl --list-devices

probe:
	@echo "Use: sudo ./mz0380-m55-real-capture.sh 6 45"

capture-h264:
	sudo ./mz0380-m55-real-capture.sh 6 45

capture-audio:
	@echo "ALSA PCM DMA is not implemented; enable_audio must remain disabled."
	@false
