# Shared module-build helper. Sourced, not executed.
#
# Every hardware script starts with `make`, which fails outright when the
# running kernel has no /lib/modules/$(uname -r)/build - the state a rolling
# distro leaves behind when it upgrades the kernel package out from under a
# still-running kernel. That failure is not a driver problem and the scripts
# should not present it as one, so this resolves the module to load:
#
#   1. build for the running kernel (normal path, always preferred - it is the
#      only way the module matches the sources you just edited)
#   2. otherwise fall back to a prebuilt ko/mz0380-$(uname -r).ko from
#      `make all-kernels`, loudly, because it may be older than the tree
#   3. otherwise re-run make so the caller sees the real error, and fail
#
# Usage:  . ./mz0380-build.sh ; KO=$(mz0380_resolve_module) || exit 1
#         insmod "$KO" ...
mz0380_resolve_module() {
	local kver prebuilt

	kver=$(uname -r)
	prebuilt="ko/mz0380-$kver.ko"

	if make >/dev/null 2>&1; then
		echo "./mz0380.ko"
		return 0
	fi

	if [ -f "$prebuilt" ]; then
		echo "build failed for running kernel $kver;" >&2
		echo "using prebuilt $prebuilt - it may PREDATE your source edits." >&2
		echo "$prebuilt"
		return 0
	fi

	echo "build failed and no prebuilt module for $kver:" >&2
	make >&2
	return 1
}
