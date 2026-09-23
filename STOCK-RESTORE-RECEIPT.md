# Stock restoration fallback: COMMIT refused by card-side read-back gate

Target: Bazzite 192.168.0.178, PCI 0000:03:00.0,
12ab:0380 / 1cfa:0003. Last observed after operator cold boot: firmware
1.15; driver subsequently unloaded, device unbound and enable=0.
These are prior-session observations, not a new hardware check.

Stock artifact outside Git: 1.1.0.194/MZ0380.HD.HEX, 936191 bytes,
SHA-256 be0d5e1992084c20ba1f2511fbf7927ad174af2c9876748f5ea12561362a783e.
Expected stock firmware: 1.11. No firmware file is included here.

## Implementation and checks

Local instrument based on e84199c plus 7133486; it is intentionally separate
from upstream delivery branches. COMMIT defaults off. The verifier performs
at most 17 deterministic dword reads, reports BEGIN and RETURN for each,
rejects the first mismatch or all-ones value, and checks a two-second elapsed
deadline before and after reads. Empty input is rejected. The padded write
extent is checked against the host BAR size before BEGIN.

`python3 scripts/test-fw-verifier.py` compiles the actual verifier functions
with mock MMIO and ASan/UBSan: PASS for endpoints, tails, sample count,
first-error stopping, matching all-ones rejection, and elapsed deadlines.
`make KVER=7.2.6-arch2-1 -j4`: PASS, with pahole-version warning.
`git diff --check`: PASS. This is a local build, not a Bazzite kernel build.
The hd-pro60 `make check` and `scripts/validate.sh` also passed; ShellCheck
was unavailable and was skipped by that gate.

Fresh independent source review returned NO-GO for a second COMMIT attempt.

## One explicitly authorized hardware attempt: 2026-09-23

The operator explicitly requested the fallback attempt after the review.
The Bazzite build used commit `8e547c9`, module SHA-256
`1f72b04d1dbed43a890df01bde371d1cfa6bcbb58ac1c7f640400cbc750c61ef`,
and the stock artifact hash and size above. The card was unbound, no video
client was open, and only PipeWire/WirePlumber held unrelated motherboard
audio controls.

CMD_INIT reported firmware 1.15. BEGIN_FW_DL accepted the exact 936191-byte
image and exposed BAR5 window `0xfa000060..0xfa0e495e` targeting `0x40000000`.
The payload push completed, but sample 1 at BAR0+0x60 returned after 202 ms
as `0xffffffff`, not expected `0x00088b1f`. The verifier returned `-EIO`,
and the firmware-load path skipped COMMIT_FW. The driver later unloaded
cleanly; the card was unbound after unload.

An AMD-Vi IOMMU event for address `0x90000000` appeared immediately after
bus mastering was enabled. This is the address recorded as a known
pre-driver observation in the readiness plan; it is not evidence of safety.
No DMA ring was allocated (`enable_dma=0`) and no IRQ/frame activity occurred.

## Blocking evidence gaps

1. A software deadline cannot interrupt a readl() that never returns.
   A fixed operation count is not a guaranteed wall-clock bound.
2. Matching 17 samples leaves unsampled bytes unverified. The host file hash
   authenticates the selected host bytes, not the card-side transferred image.
3. The stock image is not dword-aligned. Its last padded word extends one byte
   beyond the exact BEGIN window even though it fits within the host BAR.
   Device acceptance and access-width semantics remain unproven.

RE_FINDINGS.md, M4b, records roughly 200 ms completion timeouts and all-ones
reads above BAR0+0x60 on an earlier idle-card experiment. This is relevant
historical evidence, not proof of the later post-BEGIN window behavior.
Earlier claims that CPU time/context switches proved verifier progress,
and the changing 10/16/50/100 ms completion estimates, were unsupported.

## Restart point

No further Linux firmware attempts are supported by this evidence. Power-cycle
the Bazzite host before any later hardware work because an IOMMU event
occurred. Resolve the device-side readback/checksum mechanism and exact-length
tail transfer using vendor-driver/card-firmware evidence first. Require full
transfer integrity evidence, a reviewed recovery plan, a Bazzite-kernel build,
and a new explicit hardware checkpoint before proposing another write.
