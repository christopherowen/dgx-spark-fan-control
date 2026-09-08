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
  -> Arm FF-A direct message v2
  -> NVIDIA secure partition / EC packet relay
  -> EC common lower RPM clamp
  -> EC automatic policy and fan ramp
```

RPM observations use the same relay and are exposed under the `dgx_ec_fan`
hwmon device. The driver leaves the stock NVIDIA FF-A EC driver bound to its
own services. It does not map the eSPI controller or access EC memory directly.

## Pinned transport

| Field | Accepted value |
| --- | --- |
| DMI vendor / product / board match | `NVIDIA` / `NVIDIA_DGX_Spark` / `P4242` |
| Service UUID | `78b04d80-d21d-4986-8acb-467b60247ac5` |
| FF-A API | 1.2, 64-bit service |
| Partition ID / properties | `0x8003` / `0x0109` |
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
maximum. In RPM mode, telemetry offsets 4 and 6 contain the two little-endian
`u16` RPM readings. Other telemetry fields are not exposed. RPM observations
are cached for one second and values above 30,000 are rejected.

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

Probe requires an initially unset lower clamp. State changes authenticate the
existing floor and read back the result. Before submitting a write, the driver
records its attempted floor as uncertain. A later completed read may reconcile
the last confirmed floor, that exact attempted floor, or an unset clamp. This
allows recovery when a write was applied but its acknowledgement or readback
failed. An unrelated value returns `ESTALE` and is never overwritten. A second
writer using the same value cannot be distinguished, so exclusive relay
ownership remains required.

Restoration uses the same reconciliation and has up to three attempts on
lifecycle paths, with 100 ms between attempts. An ownership mismatch stops
those retries immediately. Failures are logged; restoration is not guaranteed
after a broken transport or hard crash. The in-memory attempted value does not
survive module removal or reboot; probe still requires an unset floor.
There is no EC-side expiry timer for a manual floor.

## Bounded stale-pending recovery (0.1.2)

Recovery stays under the existing transaction mutex. The driver locates OEM
service UUID `884a63a0-3285-4120-83aa-eec008a0a546` on the same FF-A bus,
retains its device reference, and holds its device lock throughout recovery.
It requires the same pinned FF-A version/partition/properties and refuses a
service already bound to another driver. No persistent OEM binding or public
memory-access interface is added.

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

## Related primary documentation

- [Linux thermal cooling-device interface](https://www.kernel.org/doc/html/latest/driver-api/thermal/sysfs-api.html)
- [NVIDIA DGX Spark OS and component updates](https://docs.nvidia.com/dgx/dgx-spark/os-and-component-update.html)
- [Open Device Partnership secure EC service overview](https://github.com/OpenDevicePartnership/documentation/blob/main/guide_book/src/specs/ec_interface/secure-ec-services-overview.md)

The ODP service overview is background for the standardized service model; it
does not specify the vendor packet relay described above.
