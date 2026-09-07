# Why the firmware can remain pending

Offline analysis, 2026-09-06; live recovery, 2026-09-07. This describes **SoC
firmware 2.155.11**. Original instructions reproduce an ordering defect against
an emulated EC boundary. Subsequent hardware diagnosis found stale pending flags
with idle mailboxes on both Sparks, and recovered both without rebooting.
The exact historical event ordering was not captured, so the reproduced race
remains a supported explanation rather than a proven trace of either failure.

## Finding

The packet relay's `pending` result is a cached software flag in the secure
partition. It is not an observation that the physical EC mailbox is busy.
There is a reachable call order in which a completed request leaves that flag
set forever:

1. The submit handler calls the lower-level packet sender.
2. The sender writes the request buffer and rings the EC mailbox.
3. Before returning, the memory-write wrapper processes queued eSPI events.
4. If the reply notification is already available, the completion callback
   clears the pending flag, reads the reply, and acknowledges the mailbox.
5. The sender returns success. The submit handler then records the requested
   output length and **sets pending back to one**.

No new interrupt or concurrent CPU is needed for this ordering: the callback
can run synchronously inside the write wrapper. The external timing assumption
is that the EC notification has arrived by that wrapper's event-drain step.
The emulator supplies that timing; it does not measure its frequency on hardware.

The result is an idle physical mailbox with a stale pending flag. Further
packet polls read that flag without contacting the EC, draining eSPI events,
checking a timeout, or attempting recovery. Increasing the Linux timeout or
slowing the poll cadence cannot repair a flag already stuck this way.

## Evidence locations

Addresses use the load base from the earlier research, `0x93940000`. They are
specific to the image hash below, not a public firmware ABI.

| Address | Observed behavior |
| --- | --- |
| `0x93974320` | Packet submit handler |
| `0x9397454c` | Calls packet sender before initializing pending metadata |
| `0x9397456c` | Stores requested output length at `0x939a20ab` |
| `0x9397457c` | Sets pending byte at `0x939a20ac` to one |
| `0x93977030` | Sender checks mailbox bits 0–1; returns `0x0a` if either is set |
| `0x9397713c` | Writes family byte 7 to mailbox `0x06000504` through the memory-write wrapper |
| `0x9396a7f8` | That wrapper calls event drain `0x9396a0a0` before returning |
| `0x9396a174` | Delivers one decoded virtual-wire event to the registered callback chain |
| `0x9396fd98` | Event index 0, value `0x85`, invokes the mailbox completion dispatcher |
| `0x93977384` | Reads mailbox status and response family; family 7 selects fan packet completion |
| `0x93974714` | Completion clears pending **before** reading the response buffer |
| `0x93974738` | Reads `old_output_length + 3` bytes from `0x06000800` into cached response `0x939a2068` |
| `0x93977528` | Reads acknowledgement address `0x06000500` after response dispatch |
| `0x939745e4` | Poll returns 2 if pending is set, otherwise uses cached response status/data |

The event-drain recursion guard does not prevent the ordering: the first drain
is eligible, and its callback runs before the outer submit stores pending.
The replay executes the original guard instructions as well as that call chain.

## Other failure paths

The same outward symptom has several possible causes:

| Injected condition | Physical mailbox in the model | Subsequent poll | Interpretation |
| --- | --- | --- | --- |
| Normal completion after submit returns | Idle | 0 | Expected behavior |
| Early completion inside the write wrapper | Idle | 2 indefinitely | Pending is set after its completion was consumed |
| Notification never delivered | Reply ready | 2 indefinitely | No callback clears pending |
| Completion callback cannot read mailbox status | Reply ready | 2 indefinitely | Callback exits before clearing pending; no retry scheduled there |
| Completion callback cannot read response family | Reply ready | 2 indefinitely | Same missing recovery at a later boundary |
| Response-body read fails after pending is cleared | Acknowledged | Can return 0 with stale data | Error is logged but not reliably propagated into the cached response |

The last case is a separate correctness defect. The replay seeds a previous
2700-RPM response, models a current floor of 4500, fails the new response-body
read, and observes a successful poll containing 2700. Thus a completed firmware
poll alone does not establish fresh readback. Our driver checks cannot fully
authenticate a reply if the secure firmware presents stale bytes as current.

An early callback also uses the **previous** requested output length because
the new length is assigned after the sender returns. A request with a larger
output than its predecessor can therefore receive incomplete cached data in
addition to the pending-state defect.

This is distinct from the historical startup `0x05` failure. The old startup
analysis does not establish that this runtime condition requires a power cycle.

## Recovery assessment

There is still no explicit cancel/reset command in the recovered packet relay.
However, the sender checks the **physical mailbox**, not the cached pending flag.
In the offline replay, a stale pending flag plus an idle mailbox allows a new
**read-lower-floor** request. When that request completes after submission,
pending clears and its exact floor is returned. This is a conditional recovery
path without rebooting, subsequently demonstrated on both affected Sparks.
Recovering relay communication is also separate from reconciling an uncertain
floor in an already-loaded 0.1.0 driver. Successful packet recovery alone must
not be reported as restored automatic fan control.

If the mailbox still holds a reply, the same read request returns `0x0a` and
does not recover. Blindly repeating it would not address the missing completion.
The current Linux driver deliberately prevents either submission while its
preflight reports pending. Do not remove that check globally.

The discriminating observation is a fixed, one-byte read of the physical
mailbox status at `0x06000504` through the existing secure OEM read service.
The OEM bulk-read handler (`0x93973260`, command 12) reaches the same eSPI read
wrapper used by the packet sender. Live reads returned `0x08` on both machines:
bits 0–1 were clear while packet polls remained 2. These read wrappers themselves drain eSPI events, so even a read
may advance a queued completion; it is not a passive secure-memory snapshot.

A bounded diagnostic should retain the existing packet owner, exclude competing
writers, expose no arbitrary addresses, and avoid the acknowledgement register.
Only an independently verified idle mailbox would justify evaluating one
read-only packet retry. A busy mailbox requires investigation of the notification
and acknowledgement path. Neither route justifies raw MMIO, firmware-memory
patching, or a fan-floor write over an unverified outstanding transaction.

For prevention, the firmware needs to establish pending state and output length
before a completion can run, and must publish completion only after successful
response capture. It also needs a defined error/recovery path for notification
or read failures. This project cannot repair those internal writes through its
current fan-floor interface. Host pacing is an experiment, not a demonstrated
fix for this ordering defect: the problematic order is inside one request.

## Isolating the trigger

The 2026-09-07 recurrence began in the daemon's operation-4 floor read. The
last logged fan change was state 6 → 5 at **12:29:53 UTC**; the first timeout
was **12:58:02 UTC**, about 28 minutes later. The traceback identifies
`current = read_state(cooling_device)`. The first kernel timeout has no
preflight-failure message; later attempts explicitly fail operation-4
preflight. This is consistent with a newly submitted read losing completion,
rather than a fan-setting transition failing at that moment. Historical logs
do not record every successful transaction or secure-world callback.

Version 0.1.1 authenticates the current floor on every two-second daemon pass
to handle resume/reset and uncertain writes. Version 0.1.0 retained the
daemon's last state between changes. The newer behavior adds approximately
1,800 floor-read submissions per hour while the floor is unchanged. That is
additional exposure to the same submit path, not evidence of an EC queue
overflow or a measured probability of failure.

Passive Linux FF-A tracing on both hosts, **18:15–18:16 UTC**, observed the
following independently on each host:

| Existing traffic in approximately 60 seconds | Count | Call duration |
| --- | ---: | --- |
| Operation-4 submissions | 30 | 256–512 microseconds |
| Cached packet polls, including preflight | 60 | 16–128 microseconds |
| Other traced calls to secure partition `0x8003` | 0 | — |
| Transport errors or nonzero response status | 0 | — |

Both FF-A direct-message entry points were probed. These observations show no
burst or competing Linux FF-A caller in those healthy windows. Cached polls
are not additional EC packet submissions. The trace cannot see autonomous
secure-firmware activity, EC-side scheduling, or earlier traffic at the failure.

### Controlled ordering experiment

The expanded replay adds sixteen paired cases. Each begins with one normally
completed read or write, then submits one isolated operation-4 read or
operation-5 write. The model supplies either normal or early completion. The
paired intervention changes **only** when the existing metadata instructions
execute: before sender argument setup instead of after the sender returns.
All original sender, wrapper, event-drain, and completion instructions remain
the same, as do the modeled EC response and timing.

| Completion timing | Original metadata order | Metadata initialized first |
| --- | --- | --- |
| After submit returns | Completes normally | Completes normally |
| Inside the write wrapper | Mailbox acknowledged, poll stays 2 | Mailbox acknowledged, poll returns 0 |

The result holds for both operations and both preceding response lengths
(zero or two bytes). Successful corrected reads also return the exact modeled
floor. Thus an isolated read is sufficient for this firmware defect in the
model: flooding, a concurrent writer, a recent floor change, and injected I/O
errors are not required. One hundred subsequent cached polls do not contact
the EC or change a stuck outcome. Host delay after submission cannot change
an ordering that has already occurred inside that submission.

This intervention runs existing instructions in a different order **only in
emulated memory**. It is a causal experiment, not an installable patch. A real
firmware correction must additionally handle failed submissions, old
outstanding responses, synchronization, and response-body read errors; merely
moving two stores is not a complete fix.

The production trigger remains unobserved inside secure firmware. The replay
proves a reachable defect and its ordering dependency, while the hardware's
idle-mailbox/stale-pending signature supports that explanation. It does not
exclude another path to the same state. The next useful live evidence is a
bounded passive request history at onset, not another blind recovery loop.
The [FF-A recorder](../research/trace/README.md) captures that history without
issuing requests or modifying controller behavior.

## Live recovery on both Sparks

The [one-shot diagnostic](../research/mailbox/README.md) kept each 0.1.0 driver
loaded, retained its module reference, and held its transaction mutex. Its
private prefix offsets were checked against the installed modules' DWARF;
loaded and installed source versions matched. Secure Boot signatures were
generated locally with the already-enrolled keys.

| Observation (UTC, 2026-09-07) | Spark 1 | Spark 2 |
| --- | --- | --- |
| Driver's remembered state | 12 / 13,500 RPM floor | 5 / 6,300 RPM floor |
| Two physical mailbox reads | `0x08`, `0x08` | `0x08`, `0x08` |
| Cached packet poll before retry | 2 → 2 | 2 → 2 |
| One read-only retry | 09:18:56; submit 0, poll 0 | 09:23:20; submit 0, poll 0 |
| Returned EC floor | `0x3057` / 12,375 RPM | `0x1fa4` / 8,100 RPM |
| Verified automatic restoration | 09:22:04 | 09:23:29 |

The fixed six-byte read at `0x06000788` returned a plausible BCD clock, not board
identity. The time-reader code at `0x93977954` confirms this use. Shared-mailbox
responses included family `0x10`/`0x11`; the dispatcher accepts `0x10..0x14` at
`0x93977478`. Its response buffer need not retain the last fan reply.

Each recovery submitted exactly one operation-4 read after checking idle status,
a plausible clock, and a recognized response family. No fan setting changed
during this step. Poll completed normally and telemetry resumed.

Both EC floors differed from the old driver's confirmed state. These values
match the policy's possible next step (one step down or two steps up), but that
alone does not prove ownership. An explicit operator recovery selected the
observed floor to remove. The helper required two fresh floor reads, with
matching physical response headers/payloads and cached replies, before sending
one operation-5 **UNSET**. It verified UNSET twice through both paths afterward.
It never changed the old driver's private state or wrote a new RPM target.

The first restoration attempt on Spark 1 refused with `ESTALE` before any
setter. The added comparison logging was absent on that attempt, so the exact
failed comparison is unknown; a later attempt passed every comparison. The
shared response buffer can be overwritten by another service, which is a
reason to refuse rather than accept an unverifiable floor.

Both old modules then independently confirmed automatic policy during orderly
removal. Version 0.1.1 loaded in state 0. Each passed a manual 0 → 12 → 0 test,
with measured 9,000/13,500 RPM after eight seconds at maximum, followed by
successful service startup. Boot IDs remained unchanged. This demonstrates
recovery and basic operation; it does not establish long-term stability or fix
the internal firmware race.

### Recurrence on version 0.1.1

Spark 1 failed again at **12:58 UTC on 2026-09-07**, approximately 3½ hours
after service startup. A state read timed out; the daemon exhausted its three
attempts, failed automatic restoration, and exited 69 at 12:58:12. Systemd
correctly performed no automatic restarts, but fan control remained unavailable
until the next operator check. Spark 2 remained running without this failure.

At **17:59:18 UTC**, the same guarded helper observed cached poll `2 → 2`,
physical mailbox `0x08` twice, and a plausible RTC. One read-only retry completed
with floor `0x189c` (6,300 RPM), matching the 0.1.1 driver's confirmed state 5.
The ordinary driver command then restored automatic policy and read back state
0; the performance service was restarted. No explicit floor-ownership override,
module replacement, or reboot was needed. The hottest sensor was 53.8°C during
the recovery check; fan RPM reads were unavailable while the relay was wedged.

This recurrence rules out a claim of unattended reliability for 0.1.1. Its
failure containment works, and the idle-mailbox recovery worked again, but
operator intervention was still necessary. The observations remain consistent
with the firmware race; they do not capture the exact internal event ordering.

## Reproduce offline

The input was independently downloaded from NVIDIA's LVFS release referenced
by the Spark's trusted fwupd metadata. The versions also match
[NVIDIA's release notes](https://docs.nvidia.com/dgx/dgx-spark/release-notes.html).

| Input | SHA-256 |
| --- | --- |
| [SoC 2.155.11 CAB](https://fwupd.org/downloads/35e2452645affc35365ee66259d82908d441283f61a16b8305593bcff3e505b1-socfw.cab) | `35e2452645affc35365ee66259d82908d441283f61a16b8305593bcff3e505b1` |
| Extracted `socfw.cap` | `0985b848b1708421a399935b7f9b1afb5469588db6f26bd2953c6013f3db70ff` |

The executable image begins at capsule file offset `0xa08f0f`. The replay loads
`0x41100` bytes of code and initialized data, supplies zeroed BSS and a stack,
and initializes the callback registration performed by firmware startup.

Extract the CAB into a directory outside the checkout, then run:

```sh
# Analysis tools only; do not install or flash the capsule.
uv run --no-project --with unicorn==2.1.4 python research/replay_pending.py \
  /absolute/path/to/extracted/socfw.cap
```

[`research/replay_pending.py`](../research/replay_pending.py) verifies the exact
capsule SHA-256 and runs **24 scenarios**: eight failure/recovery cases and
sixteen controlled ordering cases. It executes the original submit,
sender, write wrapper, event drain, callback chain, completion, and poll machine
code. Only peripheral writes, EC reads, virtual-wire reception, and diagnostic
logging are substituted in the original-order cases. The paired ordering
intervention additionally redirects emulator execution as described above.
The EC is a model, not an emulation of its Cortex-M
firmware; hardware scheduling and low-level controller behavior are not proven.

The test asserts that 100 repeated polls issue zero EC accesses, checks both
pending-write orders, verifies stale-data exposure, and exercises idle versus
busy mailbox retry outcomes. The normal project suite remains hardware-free
and does not download or execute proprietary firmware. Firmware images and
disassembly are not distributed in this repository.
