# Investigating a pending-state incident

Version **0.1.3** records a small history in the driver and prints it when a
pending timeout admits a recovery attempt. Successful polling adds no journal
messages. The history is fixed at 16 entries per driver instance; it allocates
no memory per request and adds no EC/FF-A calls. Existing state-change messages
remain unchanged.

Each incident has a monotonically increasing `incident` number within the
loaded driver instance. Join that number with the host, boot, driver version,
and journal time: numbering restarts when the module is reloaded. The existing
30-second recovery gate bounds full dumps; callers rejected during cooldown
do not print another incident. Existing rate-limited timeout/error messages
can still appear.

## What is recorded

The `begin` line identifies the requested operation and failure phase:
`preflight`, `completion`, or `blocked` by an earlier unverified recovery.
It records driver version, monotonic time, confirmed cooling state, attempted
floor, uncertain-write and recovery flags, and cached RPM with its validity
and age. Cached values are not fresh sensor observations. Temperature is not
sampled as part of diagnosis.

`totals` contains cumulative submitted capabilities reads, floor reads, floor
writes, and telemetry reads since module load. These include recovery requests
and rejected submissions. It also gives the age of the last attempted write
(meaningful only when `floor_writes` is nonzero), successful recovery count,
and cooldown duration. Counts and recent timing can help distinguish a burst
of changes from a failure during an otherwise steady floor.

The next lines print the last 16 history entries in sequence order. Each
normal transaction has a preflight entry and a submit/completion entry, so
steady two-second floor polling usually retains about eight requests. Each
`tx` line includes:

- Operation, input/output lengths, and requested floor for setters. A zero
  `floor` in a read/preflight entry is a placeholder, not a requested setting.
- Calling thread ID (`pid`) and CPU at entry; CPU migration afterward is possible.
- Kernel monotonic `start_ns`, submit-path duration in microseconds, and total
  elapsed time through completion or failure. The submit duration includes
  the small bookkeeping immediately around the FF-A call, not just EC work.
- Submit transport return and raw secure-partition status. `sp` is meaningful
  only for submit entries with `transport=0`; otherwise `-1` is a placeholder.
- Poll count, pending-response count, first and last returned poll state,
  last poll transport return, and final driver result. `0xff` is initialized
  when no state has been observed; use the transport/result fields to
  distinguish this from an unexpected returned `0xff`. Result `-110` is
  the driver's timeout.

The triggering request is printed **before recovery starts**, so a recovery
read cannot overwrite that evidence. A submitted recovery read gets its own
`tx` line afterward. `observation` and `response` lines preserve the existing
OEM reads: before/after cached poll states, two initial mailbox observations,
the final mailbox status, RTC bytes, both response headers/payloads, and the
cached recovery floor. `stage` identifies the exact boundary or guard at which
recovery returned, including early failures. `end` gives result and elapsed
recovery time; the existing success message gives reconciled state and count.

## Interpret the validity mask first

Unobserved fields are initialized placeholders. The hexadecimal `valid` mask
records which reads returned successfully, **not whether their bytes passed
validation**. OEM firmware can return zeroed data on an internal error.

| Bit | Available observation |
| ---: | --- |
| 0 | Initial cached poll |
| 1 | RTC bytes |
| 2 | First physical mailbox status |
| 3 | Initial physical response bytes |
| 4 | Second physical mailbox status |
| 5 | Cached poll after those observations |
| 6 | Completed recovery read's cached floor |
| 7 | Final physical response bytes |
| 8 | Final physical mailbox status |

For example, `stage=response-before ret=-5 valid=0x7` means the initial poll,
RTC, and first status read returned, but the response read failed. The zero
response buffer is **not** evidence of an EC reply. `valid=0x1ff` means all
nine observations are available; inspect the guard/result before accepting
them. A recovery result of zero requires the existing physical and ownership
checks to pass.

## Preserve and correlate the evidence

Capture both journals before a restart or module replacement:

```sh
sudo journalctl -k -b --utc --no-pager > dgx-fan-kernel.log
sudo journalctl -u dgx-fan-control.service -b --utc --no-pager > dgx-fan-service.log
uname -r
cat /proc/sys/kernel/random/boot_id
cat /sys/module/dgx_ec_fan_control/version
cat /sys/module/dgx_ec_fan_control/srcversion
```

Use journal time to correlate workload, thermal, suspend/resume, and other
device events. Compare the failed submit duration and spacing with neighboring
successful requests, and whether the last write was seconds or hours earlier.
Pending polls with an idle mailbox support stale firmware state; a busy
mailbox or failed OEM read points to a different boundary. The
[firmware analysis](firmware-pending-analysis.md) explains the alternatives.

This history covers this driver's requests only. It cannot see another
secure-world client, EC scheduling, or the internal completion callback, and
timing alone cannot prove the race. The optional
[passive FF-A recorder](../research/trace/README.md) supplies broader Linux
caller evidence. Logs add CPU/printing overhead during diagnosis; they do not
add hardware traffic or change the recovery admission checks.
