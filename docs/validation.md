# Validation and provenance

## Version 0.1.2 guarded recovery

On **2026-09-08**, all **22 project tests** passed. The actual C transaction
test now executes **40 scenarios**, including 30 new recovery cases: stale
pending before submission, isolated read timeout, an applied or unapplied
setter, automatic/unset recovery, late completion during observation, distinct
capabilities/telemetry payloads, a retry that wedges again, busy or changing
status, zero/invalid OEM data, stale/overwritten/error replies, foreign floors,
sender rejection, each of six OEM-read transport failures, fixed-address
refusals, and cooldown expiry.
Tests assert bounded request counts, no recovery writes, retained ownership on
failure, telemetry invalidation, and no repeated diagnosis during cooldown.
Every rejected-recovery case also attempts a setter after cached pending clears
and verifies that the unverified recovery still blocks all new submissions.

Both Sparks compiled 0.1.2 with `W=1` against `6.17.0-1029-nvidia` and signed
it with their existing enrolled certificates. There were no driver compilation
errors; Kbuild reported the known GCC command-name difference and missing
`vmlinux` for BTF. The C tests substitute Linux lifetime/locking and FF-A
boundaries, so they do not prove kernel concurrency or secure-firmware timing.

Both hosts passed automatic → state 12 → automatic during rollout, reaching
9,000/13,500 RPM after eight seconds at maximum. The final signed build was
installed for the running kernel and loaded at **08:30 UTC**; loaded and
installed `srcversion` matched **`591D047A4087B5F270FB2B6`** on both hosts.
Both final-build services started successfully, progressed 0 → 2 → 4 → 5,
and reported zero automatic restarts. At 08:31:45 UTC, both read state 5 and
6,300/6,210 RPM; the hottest sensors were 54.8°C and 52.8°C respectively.
Boot IDs were unchanged. Previous signed 0.1.1 modules were retained locally
for rollback after verified automatic restoration.

Spark 1's pre-existing wedge was diagnosed and cleared using the pinned
one-shot helper while its 0.1.1 owner remained loaded, followed by that owner's
verified automatic restoration. This was required for a safe ownership
handoff; it is **not** a live demonstration of the new driver's automatic
recovery. No live fault was induced. The new recovery integration is verified
against simulated failures; unattended recurrence recovery and long-term
stability remain to be established. Fresh bounded 24-hour passive captures
started on both hosts at 08:31 UTC under transient unit
`dgx-ec-ffa-capture-20260908.service`; kernel recovery logs supply the verified
outcome if the request recorder stops at timeout onset.

## Prevention investigation, 2026-09-07

The firmware replay passed **24 scenarios**, including sixteen paired cases
that isolate metadata ordering for a single read or write. All four
original-order early-completion cases remain pending with an idle mailbox;
the four matching metadata-first cases complete. Normal timing passes with
both orders. This is an offline causal experiment, not a hardware firmware fix.
See the [analysis](firmware-pending-analysis.md#isolating-the-trigger).

The passive FF-A recorder compiled and ran for approximately 60 seconds on
each Spark with both direct-message probes enabled. Each observed 30 floor
reads and 60 cached polls, all successful, and no other calls to partition
`0x8003` in those windows. Its failure-trigger branch has not been exercised
against a live wedge; no fault was induced to test it. No runtime controller,
kernel module, or firmware was changed for this investigation.

At **18:21:13 UTC**, both hosts attached the same seven probes for a bounded
24-hour capture under transient unit `dgx-ec-ffa-capture-20260907.service`.
Both recorders were verified running. They exit on persistent pending or
duration expiry and perform no recovery. The normal **22 project tests**,
shell syntax checks, and changed documentation link checks also passed.

## Original hardware work

The release extraction comes from the original `kernel-mod` project at commit
`87b88039a5786f6669c3deef81cda723720cea8c` (2026-08-15). The kernel control
driver, Python controller, systemd files, and eight original tests were copied
without behavior changes. The public build/signing scripts and documentation
were adapted for standalone use. Exploratory modules, private operational
notes, signing material, and firmware binaries were not part of the extraction.
Subsequent bounded research tooling is published under `research/`.

The original notes record these results on two NVIDIA DGX Sparks, board P4242,
DGX OS 7.5.0, kernel `6.17.0-1029-nvidia`, EC 3.5.8, SoC firmware 2.155.11,
with Secure Boot enabled:

| Exercise | Recorded observation |
| --- | --- |
| Read-only RPM observer | 120 samples over two minutes on one system; stable 2700/4050 RPM |
| Bounded 4500 RPM floor | Approximately 4500/4455 RPM; independent unset-clamp readback after rollback |
| Bounded 13500 RPM floor | Channel maxima 9000/13500 RPM reached within 6.3 seconds |
| Control driver state sequence | 0 → 3 → 8 → 12 → 0, with readback and automatic restoration |
| Python curve on hot sensors | 0 → 2 → 4 → 6 → 8 → 10 → 12 at two-second intervals |
| Python termination | SIGTERM restored state 0 |
| Persistent installation | Both systems built/signed locally and started the systemd service |
| Service lifecycle | Stop/start on one installed system restored automatic control, then resumed the curve |

Earlier firmware versions were examined offline only. The recorded thermal
experiments are observations, not a controlled performance benchmark. Long-term
reliability and compatibility with later kernels/firmware remain unqualified.

## Checks for the public extraction

`./scripts/check` exercises userspace behavior using temporary files and mock
boundaries, checks the narrow kernel source contract, and parses the shell
scripts. It does not issue FF-A messages or require root. The source assertions
are regression guardrails, not proof of kernel or firmware correctness.

Target compilation must use a Spark's exact NVIDIA kernel headers with `W=1`.
Building is separate from signing, installation, module loading, and live
thermal testing. GitHub Actions runs the hardware-free checks only.

On **2026-09-06**, the public extraction passed all **13 tests** and shell
syntax checks. Signing-key generation, file permissions, and refusal to
overwrite an existing key pair passed an isolated temporary-directory check.
The Python runtime source matched the original byte for byte; the kernel source differed only in author metadata.

The extracted module also compiled on a Spark against
`6.17.0-1029-nvidia` with `W=1` and the expected aarch64 vermagic. Kbuild noted
the compiler command-name difference (`gcc-13` versus
`aarch64-linux-gnu-gcc-13`, both Ubuntu GCC 13.3.0) and skipped BTF generation
because `vmlinux` was unavailable. There were no driver compilation errors.
This check used a temporary build directory and did not sign, install, or load
the module, or modify the running controller. The original extraction check did
not exercise the signing wrapper; the 0.1.1 check below does.

## Installation-guide and DKMS checks

On **2026-09-06**, the added `dkms.conf` passed an isolated **DKMS 3.0.11
add/build** on a Spark with `6.17.0-1029-nvidia`. DKMS reported the module as
`built`; `modinfo` confirmed version 0.1.0, the target aarch64 vermagic, and
the test certificate's signing identity. This used an upstream DKMS script,
temporary source/state directories, and a disposable signing key. The test
did not register with the host's system DKMS tree, enroll a key, install or
load a module, or change the running controller. Temporary files were removed.

All 13 existing tests and shell syntax checks passed. Documentation links and
anchors were checked, and all 38 documented shell blocks passed `bash -n`.
The Bash job example was exercised with mock commands for job success, job
failure, and restoration failure; it attempted automatic restoration and
returned the expected exit status in each case. This does not verify real
signal delivery, sudo renewal, or EC recovery on hardware.

System-wide DKMS installation, distribution kernel-update hooks, and preboot
enrollment remain operator steps described in the guide, not actions performed
by these checks.

## Version 0.1.1 recovery checks

On **2026-09-06**, all **21 tests** and shell syntax checks passed. The suite now
compiles the driver's actual C transaction functions against a simulated FF-A
peer and exercises ten scenarios: lost write completion, lost readback,
reconciliation after transport recovery, an unrelated floor, a foreign change
during a write, lost automatic-restoration completion, delayed preflight
completion, permanent pending, bounded restoration retries, and unexpected
protocol/transport responses. This exercises the C behavior, not Linux locking,
real timing, or secure firmware execution.

Python checks cover transient and persistent failures, preserving the original
error through failed cleanup, ownership rejection, resume resynchronization,
signal interruption of retry waits, and the service's exit-69 contract. The
signing wrapper accepts only mokutil's exact affirmative enrollment message,
including the exit-status-1 convention observed on DGX OS; rejection cases are
also tested with isolated mock commands.

The 0.1.1 module built with `W=1` and was signed using an already-enrolled local
certificate on **both Sparks** against `6.17.0-1029-nvidia`. Version, aarch64
vermagic, and signature identity were verified. The only build notices were the
same GCC command-name difference and missing `vmlinux` described above. These
signed modules were initially staged without installation. The live recovery
below subsequently made it possible to install and load them safely.

The updated Python command and systemd unit were installed on both systems,
with backups of the previous files. Against the existing stuck relay and loaded
0.1.0 driver, each service made three attempts, reported failed automatic
restoration, exited 69, and remained failed with zero restarts. This validates
failure containment on hardware; it does **not** establish recovered fan control.
The current floor could not be verified. No reboot or cold power cycle was
performed during that failure-containment check.

## Rebootless recovery and 0.1.1 live checks

On **2026-09-07**, both Sparks were recovered through the version-pinned
[mailbox diagnostic](../research/mailbox/README.md). Both physical mailboxes
were idle despite cached packet state 2. One read-only retry per machine
recovered communication. Explicit removal of each observed floor passed two
physical/cached comparisons before UNSET and two afterward. Old modules
confirmed automatic policy on orderly removal; both 0.1.1 modules then loaded
in state 0. See the [firmware analysis](firmware-pending-analysis.md#live-recovery-on-both-sparks)
for timestamps, floors, the initially refused restoration, and limits of inference.

Both systems passed manual **0 → 12 → 0**, measured **9000/13500 RPM** after
eight seconds at state 12, and restarted the performance service successfully.
Both retained their original boot IDs and had zero service restarts. No firmware
was flashed and unrelated workloads were not restarted. The subsequent soak
**failed on Spark 1 at 12:58 UTC**, about 3½ hours after startup: the relay wedged
again, the daemon exited 69, and automatic restoration failed. At 17:59 UTC one
guarded read retry recovered it; the normal 0.1.1 driver then restored state 0
and the service restarted without a reboot. Spark 2 remained running. See the
[recurrence evidence](firmware-pending-analysis.md#recurrence-on-version-011).
Version 0.1.1 therefore has demonstrated failure containment, not unattended
reliability.

All **22 tests** and shell syntax checks passed. The additional compiled-C test
executes the actual diagnostic functions against a strict fake FF-A peer,
including busy/unreadable mailboxes, invalid clocks and headers, bounded retry,
conflicting modes, physical/cache disagreement, ownership mismatch, and failed
UNSET readback. It checks that diagnostic recovery issues no setters and that
explicit restoration can only send one UNSET. The helper also built with `W=1`
against the exact target headers on both Sparks.

## Known limitations

The subsequent [firmware investigation](firmware-pending-analysis.md) reproduced
eight scenarios by executing original SoC 2.155.11 instructions against a
simulated EC boundary. It demonstrates an early-completion ordering defect and
a conditional idle-mailbox recovery path. The subsequent hardware observations
validate that path on both Sparks, but do not establish the historical trigger.

- This is an out-of-tree kernel module tied to the observed platform and APIs.
  DKMS can rebuild it for new kernels, but there is no promise of compatibility
  with a changed kernel or firmware contract.
- Manual floors do not expire. SIGKILL, a kernel crash, or a broken transport
  can prevent restoration. A retained lower floor can leave the fans running
  faster; it does not suppress firmware cooling demand.
- Restoration accepts only the last confirmed floor, the driver's own uncertain
  attempted floor, or an unset clamp. An unrelated value requires operator
  investigation. Use only one policy writer.
- Version 0.1.1's ownership fixes do not themselves clear permanently pending
  firmware state. The operator helper recovered the observed idle-mailbox case;
  busy/unreadable mailboxes and notification failures remain outside that
  demonstrated recovery. The firmware race is still present. Version 0.1.1
  contains recurring failures with bounded retries and an explicit failed service.
- The daemon ignores individual unreadable or implausible thermal sensors.
  Maximum cooling is requested only when no valid sensor readings remain.
- Fan RPM is not an exact setpoint. Firmware demand, fan saturation, and ramp
  time affect observed values. State 0 removes this driver's floor; it does not
  force an immediate return to idle RPM.
- Some investigated boots had an unavailable secure eSPI transport. See the
  [troubleshooting guide](troubleshooting.md) before attempting recovery.
