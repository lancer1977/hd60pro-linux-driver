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

#
# Kernel feature probes (kbuild pass only - $(srctree) is the kernel tree).
#
# Probe the headers instead of testing LINUX_VERSION_CODE. The exact release
# that dropped a symbol is rarely worth guessing, and a wrong guess produces a
# module that builds but misbehaves, which is far more expensive to diagnose
# than a build error.
#
ifneq ($(KERNELRELEASE),)

# vb2_ops->wait_prepare/wait_finish and the vb2_ops_wait_* helpers exist up to
# 6.x. vb2 core took the q->lock handling over and they were removed; present
# in 6.18, gone in 7.2.
MZ0380_HAVE_VB2_WAIT_OPS := $(shell grep -sqw vb2_ops_wait_prepare \
	$(srctree)/include/media/videobuf2-v4l2.h && echo 1)
ifeq ($(MZ0380_HAVE_VB2_WAIT_OPS),1)
ccflags-y += -DMZ0380_HAVE_VB2_WAIT_OPS
endif

endif

.PHONY: all all-kernels kernels kcheck clean objclean distclean legacy fw-install \
        load load-streaming unload tarball list probe \
        capture-h264 capture-audio

TARFILES = Makefile *.h *.c *.txt *.md

#
# Which kernel to build against.
#
# Defaults to the running kernel, which is the only one whose module can
# actually be insmod'd, so the scripts' plain `make` stays correct. Override
# to cross-build for another installed kernel:
#
#     make KVER=6.18.42-1-cachyos-lts
#     make all-kernels          # every installed kernel that has headers
#
KVER     ?= $(shell uname -r)
KVERSION  = $(KVER)
KBUILD    = /lib/modules/$(KVER)/build

# Installed kernels that can actually be built against, newest first.
KVERS_BUILDABLE = $(shell for d in /lib/modules/*/; do \
	k=$${d%/}; k=$${k##*/}; \
	[ -e "$$d/build/Makefile" ] && echo "$$k"; \
	done | sort -rV)

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

all: kcheck
	$(MAKE) -C $(KBUILD) M=$(PWD) $(KBUILD_FLAGS) modules

#
# Fail with a usable message instead of kbuild's bare "No such file or
# directory". A running kernel whose package has been upgraded away leaves no
# /lib/modules entry at all, so nothing out-of-tree can be built OR loaded on
# that boot - the only fixes are a reboot into an installed kernel or a
# downgrade, and the error has to say so.
#
kcheck:
	@if [ ! -e "$(KBUILD)/Makefile" ]; then \
		echo "ERROR: no kernel build tree for '$(KVER)'"; \
		echo "       ($(KBUILD) is missing)"; \
		if [ "$(KVER)" = "$$(uname -r)" ]; then \
			echo; \
			echo "  The RUNNING kernel has no /lib/modules entry - its package was"; \
			echo "  upgraded or removed. No out-of-tree module can be built or loaded"; \
			echo "  on this boot. Reboot into one of the kernels below, or reinstall"; \
			echo "  the running kernel's headers."; \
		fi; \
		echo; \
		echo "  buildable kernels: $(KVERS_BUILDABLE)"; \
		echo "  cross-build with:  make KVER=<version>"; \
		exit 1; \
	fi

#
# Build for every installed kernel that has headers, keeping one
# $(KO_DIR)/mz0380-<kver>.ko per kernel.
#
# The copies live in a subdirectory, and the loop uses objclean rather than
# `make clean`: kbuild's clean deletes *.ko RECURSIVELY under M=, so both
# keeping the copies alongside and keeping them in $(KO_DIR) lose the previous
# kernel's module to the next kernel's clean.
#
# The running kernel is built LAST so the plain mz0380.ko that the load scripts
# insmod is the one matching this boot.
#
KO_DIR = ko

all-kernels:
	@set -e; \
	mkdir -p $(KO_DIR); \
	running="$$(uname -r)"; \
	others=""; mine=""; \
	for k in $(KVERS_BUILDABLE); do \
		if [ "$$k" = "$$running" ]; then mine="$$k"; else others="$$others $$k"; fi; \
	done; \
	[ -n "$$others$$mine" ] || { echo "no buildable kernel found"; exit 1; }; \
	for k in $$others $$mine; do \
		echo "=== $$k ==="; \
		$(MAKE) --no-print-directory objclean >/dev/null; \
		$(MAKE) --no-print-directory KVER=$$k all; \
		cp mz0380.ko $(KO_DIR)/mz0380-$$k.ko; \
		echo "    -> $(KO_DIR)/mz0380-$$k.ko"; \
	done; \
	if [ -z "$$mine" ]; then \
		rm -f mz0380.ko; \
		echo; \
		echo "NOTE: running kernel '$$running' has no build tree, so mz0380.ko was"; \
		echo "      removed - a module with the wrong vermagic only fails at insmod"; \
		echo "      with 'Invalid module format', which reads like a driver bug."; \
		echo "      Reboot into a built kernel and run make; the per-kernel copies"; \
		echo "      in $(KO_DIR)/ are kept either way."; \
	fi

kernels:
	@echo "running:   $$(uname -r)"
	@echo "buildable: $(KVERS_BUILDABLE)"
	@echo "default:   $(KVER)"

# Drop just enough build state to force a full rebuild against a different
# kernel, without kbuild's recursive walk. Top level only, and $(KO_DIR) is
# untouched by construction.
objclean:
	@rm -f *.o .*.o *.mod *.mod.c .*.cmd mz0380.ko \
		Module.symvers modules.order modules.builtin*
	@rm -rf .tmp_versions

# clean must work even when the running kernel has no build tree, otherwise
# the one command that unsticks a broken tree is itself unavailable.
clean:
	@if [ -e "$(KBUILD)/Makefile" ]; then \
		$(MAKE) -C $(KBUILD) M=$(PWD) $(KBUILD_FLAGS) clean; \
	else \
		echo "no build tree for '$(KVER)' - removing build products directly"; \
		rm -f *.o *.ko *.mod *.mod.c .*.cmd Module.symvers modules.order; \
		rm -rf .tmp_versions; \
	fi

# clean deliberately leaves $(KO_DIR) alone - all-kernels runs clean between
# kernels, so anything it removed there would not survive the next pass.
distclean: clean
	rm -rf $(KO_DIR)

legacy: kcheck
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
