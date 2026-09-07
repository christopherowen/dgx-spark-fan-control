# Passive FF-A request recorder

`ffa-summary.bt` observes existing Linux calls to the Spark EC's secure
partition. It sends no requests, reads no EC registers, changes no fan setting,
and performs no recovery. It is an investigation tool, not a startup service.

It requires root, bpftrace, kernel BTF, and the two FF-A probe symbols present
on the tested `6.17.0-1029-nvidia` kernel. It is pinned to partition `0x8003`
and packet device **18** on the two investigated Sparks. Verify their identity
before using it on another installation:

```sh
cat /sys/bus/arm_ffa/devices/arm-ffa-18/partition_id
cat /sys/bus/arm_ffa/devices/arm-ffa-18/uuid
# Expected: 0x8003 and 78b04d80-d21d-4986-8acb-467b60247ac5
sudo env TZ=UTC bpftrace research/trace/ffa-summary.bt
```

The default run lasts approximately 60 seconds; a positive first argument
after the script path overrides the duration in seconds. It counts calls and response
statuses by device and the first two request bytes, and reports call-duration
histograms in microseconds. On device 18, request `1,4` means submit a floor
read; request `2,0` is a cached packet poll. Other services have different
payload formats: their first bytes must not be interpreted as fan operations.
Durations are Linux FF-A call times, not measured EC execution times.

The recorder retains the last 128 completed calls. If packet polls return 2
for at least one second without an intervening successful-transport poll with
a different response, it prints the ring and exits. This threshold captures
the driver's first timeout/retry boundary; it does not independently diagnose
the physical mailbox. Healthy expiry prints aggregate statistics only.

Each `@recent[ring_slot]` value contains:

```text
(sequence, monotonic_start_ns, tid, device, request_byte_0, request_byte_1,
 transport_return, response_byte_0, duration_us)
```

Ring slots wrap; sort by sequence/start time rather than slot. Sequence
allocation is not atomic across CPUs, so concurrent completions may overwrite
a slot or share a sequence. Use timestamps and TIDs as supporting evidence;
this is a bounded diagnostic, not a lossless or secure-world trace. Tracing
adds host overhead and cannot observe the internal completion callback.

For a bounded 24-hour capture that survives SSH disconnection, use an absolute
script path and a **new** output path. The transient unit is not enabled at
boot and exits on the pending trigger or duration limit:

```sh
sudo systemd-run --unit=dgx-ec-ffa-capture --collect \
  --property=RuntimeMaxSec=86500 --property=TimeoutStopSec=10 \
  --property=StandardOutput=file:/absolute/path/to/new-capture.log \
  --property=StandardError=inherit \
  /usr/bin/env TZ=UTC /usr/bin/bpftrace \
  /absolute/path/to/ffa-summary.bt 86400
systemctl status dgx-ec-ffa-capture.service
```

To stop early, stop **the recorder**:

```sh
sudo systemctl stop dgx-ec-ffa-capture.service
```

If it triggers, preserve its output together with UTC kernel and fan-service
journals before attempting recovery. A failed Linux call alone cannot tell
whether the firmware consumed an early reply, lost a notification, or failed
an eSPI read. The [firmware analysis](../../docs/firmware-pending-analysis.md)
describes the distinct signatures and the limitations of later active probes.
