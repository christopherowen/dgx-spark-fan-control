# Firmware interface

These are reverse-engineering findings from the original project, checked
against the shipped implementation and live observations. They are not a
vendor-published ABI guarantee. No firmware image or extracted firmware code
is distributed here.

## Path from userland to the fans

```text
dgx-fan-control (Python, root)
  -> /sys/class/thermal/cooling_deviceN/cur_state
  -> dgx_ec_fan_control (kernel)
  -> Arm FF-A direct message (legacy on API 1.1, v2 on API >= 1.2)
  -> NVIDIA secure partition / EC packet relay
  -> EC common lower RPM clamp
  -> EC automatic policy and fan ramp
```

Duty-derived RPM estimates use the same relay and are exposed under the
`dgx_ec_fan` hwmon device, labelled as estimates. They are not tachometer
measurements and never validate a clamp write, ownership, bind, or recovery.
The driver leaves other services with their existing drivers, and does not map
the eSPI controller or expose raw EC memory access.

## Probe-time transport selection

On the supported DGX Spark DMI platform, declared `PNP0C0B` fans take priority.
Their `_FIF` must describe revision-0 fine-grained control with a valid step,
and `_FPS`, `_FSL`, `_FST` must exist. The module requires their native
`acpi-fan` platform driver to be bound and uses its existing thermal interface;
it never borrows/unregisters that cooling device, invokes `_FSL`, or creates a
second writer. A declared but unusable fan is a diagnosed refusal, not a
silent packet fallback. Load the native `fan` module before retrying if needed.

On measured ACUM028 the native state is level 0..100 and the AML request scale
is **975..4350 RPM** (`975 + 3375 * level / 100`). State zero is minimum,
**not automatic**. `_FSL` writes both host clamps and no ACPI method clears
them; reboot is the only measured release. This is not the EC's published
estimate scale (CPU **1260..9000**, rear **1890..13500**). There is no conversion
between these interfaces, and the packet userspace daemon is not used here.

Only when no fan is declared does the module bind the packet service below.
ACUM027 and ASUS 0ACUM10 have no fan in their measured, identical DSDTs.
ACUM028 (newest NVIDIA capsule, SoC 2.158.15) declares FAN0/FAN1. No ACUM028
or 0ACUM10 unit is available for runtime acceptance here; an FF-A emulator
model cannot prove binding. See the [plan and evidence limits](firmware-agnostic-transport-plan.md).

## Packet service contract

| Field | Accepted value |
| --- | --- |
| DMI vendor / product / board match | `NVIDIA` / `NVIDIA_DGX_Spark` / `P4242` |
| Service UUID | `78b04d80-d21d-4986-8acb-467b60247ac5` |
| FF-A API | >= 1.1, 64-bit service; legacy calls below 1.2, v2 calls at/above 1.2 |
| Partition ID / properties | Discovered and logged, never compared against fixed constants |
| Capabilities discriminator | 1 (exact vendor name is unknown) |
| Unit mode | 0 (RPM) |
| Fan 0 / fan 1 ranges | 1260–9000 / 1890–13500 RPM |

Submit messages start with command byte `0x01`, then operation, input length,
output length, and any input bytes. Poll uses command byte `0x02`. The secure
relay maps this to EC packet family 7. Poll responses begin with a state byte:
0 complete, 1 EC error, 2 pending; output follows at byte 1.

In SoC 2.155.11, that poll reads a **cached secure-partition flag and response**.
It does not read the physical EC mailbox. Offline replay found completion
ordering and stale-response defects; see the
[firmware analysis and recovery assessment](firmware-pending-analysis.md).

The driver serializes its transactions with a mutex, checks for pending work
before submission, and polls at most 100 times with 10 ms between pending reads
in each preflight and completion phase. Preflight drains a delayed completion.
Version 0.1.2 can recover a timeout only after independently establishing that
the physical mailbox is idle, as described below. It never bypasses physical
busy detection or blindly resends a setter.
It does not coordinate arbitrary third-party clients: only one driver/client
should own this relay. FF-A call duration itself is not bounded by the poll loop.

| Implemented operation | Input bytes | Output bytes | Purpose |
| ---: | ---: | ---: | --- |
| 1 | 0 | 10 | Capabilities |
| 4 | 0 | 2 | Read common lower clamp |
| 5 | 2 | 0 | Write common lower clamp |
| 7 | 0 | 64 | Read telemetry |

Capabilities contain the discriminator and unit mode, followed by four
little-endian `u16` values: fan 0 minimum, fan 0 maximum, fan 1 minimum, fan 1
maximum. Telemetry offsets 4 and 6 contain the two little-endian `u16` RPM
estimates. The wire request remains 64 bytes, but only its first eight response
bytes are consumed, fitting even the legacy five-word direct-message reply.
No unreturned telemetry suffix is exposed. Estimates are cached for one second;
zero, large, or delayed values are not speed-based service-health checks.
The recovered EC derives these numbers from duty, gates zero on stopped/zero
tach, and republishes only on >5% change; it does not publish measured tach RPM.

The observed operation table and lengths agree across official EC firmware
2.4.11, 3.3.2, and 3.5.8. Live validation covered 3.5.8. Unknown responses fail
the local operation rather than selecting another transport.

## Additive control and ownership

The recovered firmware computes the equivalent of:

```text
effective_request = max(min(automatic_request, upper_clamp), lower_clamp)
```

`0xffff` denotes an unset clamp. This driver only writes the lower clamp,
through a fixed 13-entry state table. It never writes the upper clamp. Its
additive guarantee is relative to the firmware policy and any existing upper
clamp; it does not audit or repair changes made by other software.

Probe accepts an unset lower clamp, or a clamp of exactly 0. Any other
existing value is refused. State changes authenticate the floor and read back
the result. Before submitting a write, the driver records its attempted floor
as uncertain. A later completed read may reconcile the last confirmed floor,
that exact attempted floor, or an unset clamp. This allows recovery when a
write was applied but its acknowledgement or readback failed. An unrelated
non-zero value returns `ESTALE` and is never overwritten. A second writer
using the same value cannot be distinguished, so exclusive relay ownership
remains required.

A clamp of 0 is not an owner. The fan minima are 1260 and 1890 RPM, and 0
leaves both fans stopped. The EC retains it across a warm reboot. Probe,
`automatic`, and every other restoration write the unset value over it, so a
loaded module whose performance service is not running is NVIDIA's curve.
Registration of the cooling device can itself apply a state (the thermal core's
untargeted update uses state 0, and a bound governor can step upward). Probe
restores the unset clamp after registration, before the service is running.

Restoration uses the same reconciliation and has up to three attempts on
lifecycle paths, with 100 ms between attempts. An ownership mismatch stops
those retries immediately. Failures are logged; restoration is not guaranteed
after a broken transport or hard crash. The in-memory attempted value does not
survive module removal or reboot; probe still requires an unset floor or the
zero clamp it is about to clear.
There is no EC-side expiry timer for a manual floor.

## Bounded stale-pending recovery (0.1.2)

Recovery stays under the existing transaction mutex. The driver locates OEM
service UUID `884a63a0-3285-4120-83aa-eec008a0a546` on the same FF-A bus,
retains its device reference, and holds its device lock throughout recovery.
It requires the same >= 1.1/64-bit service transport checks, choosing the API
from that peer's reported version; endpoint/properties are discovery. A service
already bound to another driver is refused. No persistent OEM binding or public
memory-access interface is added. This is the firmware-inventory/OEM service,
not the packet-service identity: its command-12 version reads at `0x06000760`
and boot traffic to `0x8003` alone do not establish fan support.

Only OEM command 12 reads of these fixed addresses/lengths are permitted:

| Address | Bytes | Purpose |
| --- | ---: | --- |
| `0x06000504` | 1 | Physical mailbox status |
| `0x06000800` | 5 | Response family, operation, status, and lower floor |
| `0x06000788` | 6 | BCD clock canary |

The driver requires two identical physical status observations, 100 ms apart,
with busy bits 0–1 clear, a plausible clock, a recognized response family,
and valid packet-poll states. OEM failures can appear as zeroed data, so an
idle-looking status alone is insufficient. These reads may drain a late
completion; a transition to complete during observation is allowed. The
packet sender independently rejects a mailbox that becomes busy afterward.

At most one operation-4 read is submitted by a recovery attempt. It has the
normal bounded completion wait, with no recursive recovery. Its cached floor
must match a physical family-7/operation-4/success reply, with mailbox idle,
and pass the ordinary ownership reconciliation. Shared-mailbox overwrite or
inconsistent data is a refusal. A recovered floor read returns that result;
capabilities or telemetry are resubmitted once for their own payload. A
timed-out setter succeeds only if the recovered floor is its exact target;
the setter itself is never replayed by recovery.

Every recovery attempt invalidates telemetry and starts a **30-second cooldown**
shared by all callers, including lifecycle cleanup. Both successful and failed
attempts are paced. After successful recovery, ordinary healthy transactions
can continue during cooldown. A failed recovery fences all subsequent
transactions until another verified recovery succeeds, even if the cached
pending flag clears in the meantime. This prevents cleanup or another sysfs
caller from accepting a reply whose physical cross-check failed. No further
OEM diagnosis or recovery submission starts until cooldown expires.
Failure retains an error and cannot claim restoration. Successful
recovery logs the verified floor, reconciled state, and recovery count.

This tolerates the demonstrated idle-mailbox condition without modifying
firmware. It cannot recover a genuinely busy/unreadable mailbox or prove that
all ordinary firmware replies are fresh. Physical-response cross-checking is
specific to recovery; this version does not add OEM reads to healthy polling.
Version 0.1.3 adds [incident evidence](incident-logging.md) around the same
recovery path, including the triggering transaction history and the existing
physical observations, without additional EC requests.

## Related primary documentation

- [Linux thermal cooling-device interface](https://www.kernel.org/doc/html/latest/driver-api/thermal/sysfs-api.html)
- [NVIDIA DGX Spark OS and component updates](https://docs.nvidia.com/dgx/dgx-spark/os-and-component-update.html)
- [Open Device Partnership secure EC service overview](https://github.com/OpenDevicePartnership/documentation/blob/main/guide_book/src/specs/ec_interface/secure-ec-services-overview.md)

The ODP service overview is background for the standardized service model; it
does not specify the vendor packet relay described above.
