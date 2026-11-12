# Firmware-agnostic transport plan

## Evidence and boundaries

This plan precedes all code changes on `firmware-agnostic-transport`.
It is built from measurements of released firmware and of the recovered EC
source, cited by what they are rather than by where they happen to be checked
out:

- **The vendor DSDT of each firmware image**, read from the capsules themselves:
  `ACUM027` (SoC 2.155.11), `ACUM028` (SoC 2.158.15) and `0ACUM10` (SoC 4.1.0).
  `ACUM028` declares `FAN0` with `_FSL`/`_FST`/`WFCF`, and its `_FSL` itself
  issues `SMC 0xC400008D` with endpoint `0x8003`. The other two declare no fan
  device at all, and the `ACUM027` and `0ACUM10` tables are byte-identical.
- **A recovered EC firmware image, rebuilt to source**: its fan controller
  (`fan_set_duty_cycle`, `fan_read_rpm`, `thermal_update_fan_rpm`) and its
  firmware-inventory command layout. The source tree is not distributed with
  this project; the facts taken from it are reproduced here so the change can
  be reviewed without it.
- **On-unit measurements of the packet path on an `ACUM027` unit**: reads,
  tracked writes, and a 13500 rpm floor whose EC-derived readback was
  8820/12420.

The EC source computes published RPM from duty, with zero-tach gating and a
more-than-5-percent publication filter. Published RPM is an **estimate**, not a
tachometer measurement. It cannot authenticate a write or justify recovery.

`0xc400008d` is `FFA_MSG_SEND_DIRECT_REQ2`, not a fan-specific SMC. Endpoint
`0x8003` hosts multiple services. UUID `884a63a0-3285-4120-83aa-eec008a0a546`
(the low word `0x20418532a0634a88`) identifies firmware inventory/OEM reads,
including command 12 at `0x06000760`; it is not the packet fan-service UUID
`78b04d80-d21d-4986-8acb-467b60247ac5`. Inventory boot traffic alone establishes
no fan-service compatibility.

## Why the existing gate is brittle

`dgx_ec_validate_transport` currently demands exactly FF-A 1.2, a 64-bit
endpoint, `vm_id == 0x8003`, `properties == 0x0109`, the packet UUID, and
`sync_send_receive2`. A valid FF-A 1.1 service, any later API version, or a
rediscovered endpoint/properties is rejected before its protocol can answer.
The OEM recovery helper repeats that gate. An FF-A-only module also cannot
select a firmware-declared ACPI fan independently of packet enumeration.

The UUID remains the service identity, not a version fingerprint; the packet
format and capability discriminator/unit/ranges remain fail-closed protocol
checks. Discovery values are logged, not treated as ABI constants. No
arbitrary endpoint scan, inventory-to-fan reinterpretation, or unvalidated
write is introduced.

## Probe order and ownership

1. On the existing DMI-supported DGX Spark, look for the vendor's declared
   `\\_SB.FAN0` and `\\_SB.FAN1` (`PNP0C0B`) and inspect their `_FIF`, `_FSL`,
   and `_FST` contracts without issuing `_FSL`.
2. If declared, select the **native Linux ACPI fan driver**, rather than create
   a second cooling device/writer for the same AML objects. Log the native
   device/driver and each rejected contract, and the ACPI level scale. Native
   fan devices remain Linux's control surface; the additive EC userspace
   daemon must not run against them. Missing native ownership or malformed AML
   is diagnosed as an ACPI refusal, not silently bypassed with packet writes.
3. Only when the ACPI fan is absent, register the existing FF-A packet driver.
   Log the packet UUID, reported FF-A version (or explicitly unavailable),
   discovered endpoint/properties, mode, and rejection stage. Also diagnose
   absence of a matching enumerated packet service at module initialization.
4. Accept API versions >= 1.1. Use `sync_send_receive` below 1.2 and
   `sync_send_receive2` at/above 1.2, selecting solely from the reported version
   and checking the corresponding operation. Never silently downgrade a
   failed direct-message-v2 transaction. Preserve packet submit/poll framing.
   Legacy direct messages carry five words rather than fourteen: request the
   usual 64-byte telemetry payload but consume only its first eight bytes,
   which contain both published RPM estimates. Never pretend the unreturned
   suffix was received. Capabilities, floor, and fixed OEM reads fit both.

Selection is fixed for the module lifetime. An ACPI contract failure does not
fall through to another writer; an FF-A transaction failure never switches to
ACPI. The module never unregisters or steals the native ACPI driver's device.

## State and scale contracts

- Packet path: keep `dgx_ec_fan_floor`, automatic/unset state 0, and the existing
  12 EC-scale common lower floors. CPU range 1260..9000, rear 1890..13500.
  Floor readback, not published RPM, remains the ownership/write/recovery
  evidence. Automatic restoration explicitly writes and verifies the unset
  lower clamp through the packet protocol; the EC does not clear it itself.
- ACPI path: native cooling state is a **level 0..100**, not a packet floor.
  The measured AML maps that level to `975 + 3375 * level / 100` RPM, i.e.
  975..4350. Do not translate either scale into the other. State 0 is the
  minimum level, never automatic or stopped. `_FSL` writes both clamps and has
  no reset-to-unset operation: a clamp can persist until reboot. Log this
  before any operator can mistake native level 0 for automatic restoration.
  No `_FSL` is issued by this module, including probe/remove/suspend/reboot.
- Telemetry: publish the EC's estimates as read-only hwmon input and explicitly
  label their meaning in code/logs/docs. Remove RPM plausibility as a bind or
  recovery validation gate; numeric speed does not establish service health.

## Expected firmware behavior

| Firmware | Measured declaration/evidence | Expected selection; remaining uncertainty |
| --- | --- | --- |
| ACUM027 / SoC 2.155.11 | No FAN0, `_FSL`, or WFCF. EC service reads and tracked writes worked on hardware; a 13500 request yielded published estimates 8820/12420. | FF-A packet path, subject to actual service discovery/protocol answers. The prior direct-SMC measurements do not prove this revised module's binding. |
| ACUM028 / SoC 2.158.15 (newest NVIDIA capsule) | FAN0/FAN1 and `_FSL`/`_FST`/WFCF exist; WFCF uses SMC 0xc400008d and endpoint 0x8003, writing 0x06000790..793 on the 975..4350 scale. | Prefer native ACPI, no duplicate EC floor device. The newest AML still uses the direct-request transport: the old exact-version/properties gate, not that SMC number, is brittle. Runtime binding/control remains unverified here. |
| 0ACUM10 / SoC 4.1.0 (ASUS image) | DSDT byte-identical to ACUM027, no fan device. Measured inventory calls are not fan control. | Attempt FF-A discovery; reject with complete diagnostics if packet service is absent or does not answer. Actual fan support remains open. |

## Specific change sites and preserved invariants

- `kernel/dgx_ec_fan_control.c`: includes/constants and transport validation:
  preserve UUID identity/64-bit framing; replace exact-version/endpoint/property
  gating with version ranges and diagnostic discovery.
- Packet submit, packet poll, fixed OEM read: one version-selected transfer
  helper for every call; preserve bounded polling, never replay a setter,
  never copy bytes outside the returned register capacity.
- Telemetry read/refresh and baseline/incident logs: consume only the published
  estimate prefix; speed values never establish write success, bind health,
  ownership, or recovery.
- Probe/remove/registration error paths: preserve mutex serialization against
  thermal/hwmon callbacks; unregister each owned cooling device exactly once,
  finish restoration while data is alive, and do not touch an unregistered
  object. Reboot-notifier failure must not strand a thermal-applied floor.
- Module initialization/exit and ACPI selection helper: choose native ACPI
  before FF-A registration, hold/release any discovery reference correctly,
  never own or unregister the native cooling device, and diagnose missing
  candidates even when an FF-A probe callback cannot run.
- `tests/test_kernel_transactions.py` and transport contract tests: preserve
  executable transaction/recovery coverage; add legacy/version-range,
  changed-discovery-value, telemetry-prefix, ACPI ownership/no-write, and
  failed-bind diagnostic cases using the actual C helpers where practical.
- `README.md`, `docs/protocol.md`, and this plan: distinguish transport state
  contracts and evidence limits. The existing userspace additive-floor API
  intentionally remains packet-only; no curve/scale translation or fake
  automatic state is introduced for ACPI.

## Verification and delivery

Commit this plan alone first. Then implement requirements R1 (version/API),
R2 (discovery), R3 (ACPI-first native path), R4 (separate scales), R5 (estimated
RPM only), and R6 (complete candidate diagnostics), naming them in the
implementation commit. Record repeated self-review rounds, covering version
ranges, ownership/error unwind, integer arithmetic, callback concurrency, and
truthfulness of logging; fix findings and finish with a clean round. Run the
tree's hardware-free checks and attempt its module build serially after all
changes, recording exact results in the final commit/note.

Local C harnesses can establish framing, version selection, bounds and error
behavior under simulated peers; a module compile can establish compatibility
with the headers/compiler used, not the real firmware. There is no available
ACUM028 or 0ACUM10 unit, and the emulator FF-A model cannot validate binding.
Do not claim runtime acceptance for them (or for this revised module on
ACUM027). Settling it requires a real unit with recorded firmware/kernel,
complete load/bind `dmesg`, enumerated services and native cooling devices,
read-only estimates/control-state reads, and separately authorized tracked
writes with reboot available to release any ACPI clamps. The future firmware
upgrade/downgrade path makes those measurements possible, not completed.

## Self-review record (2026-11-11)

### Round 1 — transport width, state ownership, and scale boundaries

Reviewed the implementation against R1–R6 and the recovered source. Findings
fixed: a legacy five-word response cannot carry the previous 64-byte telemetry
copy; request the original wire length but consume only the eight-byte estimate
prefix, with explicit response-capacity checks. The direct-message-v2 API must
not be required on 1.1, nor silently retried through a different API. Invalid
high-bit version/error values must be rejected before unsigned comparisons.
All packet/OEM transfers now share the version-selected helper.

Reviewed error-path ownership against thermal callbacks. The previous
reboot-notifier registration failure could unregister after thermal callbacks
had installed a floor without restoring it. A shared unwind now unregisters
the owned cooling device, nulls its pointer, and attempts restoration while
data/hwmon are alive, outside any held transaction lock. No native ACPI cooling
device is borrowed or unregistered. Distinct native ACPI control prevents a
second writer, a scale conversion, or a fake automatic state.

### Round 2 — diagnostics, arithmetic, concurrency, and evidence claims

Rechecked range comparisons (including 1.3/2.0), integer promotions, callback
locking and diagnostic paths. Findings fixed: an ACPI-declared-but-unusable
boot also needs a nonbinding FF-A metadata census for diagnosis; metadata
unavailable without enumeration must be stated rather than invented.
Kernel configurations without reachable FF-A must not reference its bus symbol.
Probe logs now describe an unset clamp verified *during probe*, not permanent
automatic policy, and restoration logs claim only verified lower-clamp state.
Avoided clearing/copying an unreturned legacy response suffix.

The only new level arithmetic is `100ULL / step`, after validating the full
64-bit AML integer is 1..9. There is no ACPI↔EC RPM arithmetic. Packet floor
ownership/recovery still uses control-state/physical reply bytes under the
existing mutex, never speed. RPM estimates are labelled in hwmon, logs and
documentation; zero and values above the former plausibility threshold are
covered as estimates, not bind/recovery evidence. Documentation was scoped
explicitly to packet-only restoration and ACPI's reboot-only measured release.

### Round 3 — clean

Reviewed the resulting version/API branches, response lengths, all new ACPI
allocation/device-reference exits, iterator reference handling, cooling-device
registration/unwind/removal, native-driver ownership checks under its device
lock, and packet/hwmon/thermal callback serialization. Rechecked all new log
claims against what each call actually establishes. **No further findings.**
No load/control result on a real unit, FFH operation, physical fan speed, or
successful firmware restoration is inferred from this review.

### Round 4 — executable harness integration

The first full `./scripts/check` exposed a missing
`DGX_EC_PACKET_COMPLETE` definition in the new test-only C shim, preventing that
test class from compiling. Added the actual constant's value to the shim;
no driver exception or weakened assertion was introduced. The rerun completed
all **27 tests successfully**, including the new compiled transport/ACPI peers,
both direct-message widths across the transaction/recovery cases, and the
existing policy/failure/signing checks and shell syntax checks.

## Executed build and evidence limits

Built serially, with the repository Makefile's `W=1`, using GCC **13.3.0**
(`aarch64-linux-gnu-gcc-13`) against Ubuntu
**`7.0.0-1019-nvidia-64k` ARM64 headers**, package version
**`7.0.0-1019.19~24.04.2`**. Headers were downloaded/unpacked only inside this
checkout; no installed kernel, global configuration, or adjacent repository
was changed. For cross-building on this x86 host, the matching amd64 package's
host executables `scripts/basic/fixdep` and `scripts/mod/modpost` were used with
the unmodified ARM64 headers/configuration and ARM64 `Module.symvers`.

Command (with the isolated headers present):

```sh
make -j1 -C kernel \
  KDIR="$PWD/.build-headers/target/usr/src/linux-headers-7.0.0-1019-nvidia-64k" \
  ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- CC=aarch64-linux-gnu-gcc-13
```

Result: **exit 0**; driver compilation, MODPOST, module-object compilation and
linking produced `kernel/dgx_ec_fan_control.ko`. `file` identified an
**ELF 64-bit LSB relocatable, ARM aarch64** object. There were no driver
compilation errors/warnings. The build reported the environment's missing
`pahole` (kernel version 125 versus local 0), and skipped BTF because no
`vmlinux` was available. The artifact was not signed, installed or loaded.

Header package provenance (Ubuntu `pool/main/l/linux-nvidia-7.0/`, common/host
from `archive.ubuntu.com/ubuntu`, ARM64 from `ports.ubuntu.com/ubuntu-ports`):

| Package (all version `7.0.0-1019.19~24.04.2`) | SHA-256 |
| --- | --- |
| `linux-nvidia-7.0-headers-7.0.0-1019`, all | `42ea7b7148c0af2481c3ad87aef66e6d451aa88400fc04f1307097af9b9ccca0` |
| `linux-headers-7.0.0-1019-nvidia-64k`, arm64 | `7a66dc37851c7f2c39b89f01d50f4f1773a9be58ad4146eee5ed5b89649b135c` |
| `linux-headers-7.0.0-1019-nvidia`, amd64 host tools | `963cb196495955dd7509fb36c1572343f8a46a7ed09037a00240e84282e6db90` |

The isolated downloaded headers and packaging scaffolding were removed after
the build; they are not shipped. The compile proves C/API/export compatibility
with those headers and target configuration, **not** a loadable signature for
any unit, a real FF-A bind, ACPI FFH operation, fan response, recovery on
changed firmware, or thermal safety. The hardware-free peers are models too.

**ACUM028 and ASUS 0ACUM10 runtime acceptance remain unverified because no
unit running either is available. This revised module is not runtime-tested
on ACUM027 either.** Settle each with the real-unit evidence listed above:
firmware/kernel identity, complete candidate-selection/bind `dmesg`, native
fan/FF-A enumeration and read-only control/estimate reads, then separately
authorized tracked writes with independent thermal observation and reboot
available to release ACPI clamps. The emulator's FF-A model cannot settle it.

### Round 5 — final clean review

Reviewed the test-shim-only fix and final evidence against the actual test and
ARM64 build output. Rechecked the R1–R6 change inventory, version ranges,
thermal unregister-before-restore ordering, reference/allocation ownership,
guarded integer division, separated scales, no RPM-based validation, and
native ACPI's lack of restore semantics. Historical measurements remain
explicitly distinct from runtime acceptance of this revision.
**No further findings; final review clean.**
