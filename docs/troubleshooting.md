# Troubleshooting

Start with read-only observations:

```sh
uname -r
modinfo dgx_ec_fan_control
dgx-fan-control status
systemctl status dgx-fan-control.service
sudo journalctl -k -b | grep -E 'dgx|fan.floor|arm.ffa'
sudo journalctl -u dgx-fan-control.service -b -n 60
```

## Module rejected or cooling device absent

Check that the module was compiled for the running kernel (`modinfo` reports
`vermagic`), and that its signing certificate is enrolled. `Key was rejected
by service` usually requires checking signing/enrollment. `Invalid module
format` requires inspecting the kernel log and rebuilding for the exact kernel.

The module can load while its device probe rejects an unsupported transport or
capability response. Read the kernel log; do not remove validation checks.
This implementation matches the NVIDIA P4242 platform only. Other GB10 boards
have not been qualified.

An older experimental driver may already own the relay. Do not load multiple
fan drivers or run a direct packet client alongside the controller. Restore
automatic control with the existing owner before replacing it.

## EIO, busy, or refused floor changes

The original research found boots where both the stock ACPI time-alarm read and
the fan packet service failed. Secure-partition status `0x05` indicated failure
of an eSPI mailbox-status read, while `0x0a` indicated mailbox busy. These are
different failures. A cold power cycle recovered the transport in the recorded
cases; an ordinary warm reboot was not a deterministic trigger or cure.

There is no demonstrated safe userland command to reinitialize that secure
transport. If an orderly automatic reset fails, retain the logs and arrange a
maintenance shutdown and cold power cycle. Do not repeatedly load competing
probes or access controller MMIO: even a normal-world read of the secure-owned
eSPI controller caused a watchdog reboot during the original investigation.

An ownership mismatch (`ESTALE` in version 0.1.1) also causes failure: the driver
will not overwrite an unknown existing floor. Avoid concurrent writers. Version
0.1.1 remembers its own attempted write and can reconcile it after transport
recovery, even when acknowledgement or readback failed. Version 0.1.0 could lose
ownership in this situation. Neither version can restore through a relay that
never completes its outstanding transaction.

## Persistent pending and service exit 69

A separate failure was observed on two systems running version 0.1.0: the stock
ACPI EC time read still worked, while successful FF-A poll calls continuously
returned packet state `2` (pending). No new fan request was being submitted.
This differs from a failed FF-A call or submission status `0x0a` (mailbox busy).
The live trigger remains unconfirmed. Further offline analysis reproduced a
firmware ordering defect that can leave the pending flag set **after the EC
has completed the request**. See the [firmware analysis](firmware-pending-analysis.md)
for that distinction, other failure paths, and the subsequent live recovery.

Version 0.1.1 logs preflight timeouts separately from submission failures. The
daemon retries transient transport errors after two and four seconds; after
three consecutive failures it attempts automatic restoration and exits 69.
Non-retryable errors, including an ownership mismatch, stop immediately. Both
the initiating error and any restoration failure remain in the journal. A
failed restoration means the current floor is **unverified**, not automatic.

Systemd does not restart exit 69. Unexpected process crashes have a 30-second
restart delay and a three-start limit per five minutes. SIGTERM lets the daemon
perform its own cleanup, without a competing `ExecStop` writer.

On 2026-09-07, both machines had an **idle physical mailbox with a stale pending
flag**. A guarded, one-shot read-lower-floor retry recovered communication on
each. Their old drivers also remembered a different floor from the EC's actual
floor, so recovery required separately identifying and removing that floor,
verifying automatic control, and upgrading the driver. Neither machine rebooted.

Retain the kernel and service logs. The [operator diagnostic](../research/mailbox/README.md)
documents the version-pinned procedure and refusal conditions. It is research
tooling, not an automatic daemon recovery loop. A busy or unreadable mailbox
is outside the demonstrated recovery. Do not unload/reload to discard ownership
state, remove the driver's pending check, or submit raw reset packets. After
communication and ownership are reconciled and status verifies state 0, restart explicitly:

```sh
dgx-fan-control status                    # require state=0/12
sudo systemctl reset-failed dgx-fan-control.service
sudo systemctl start dgx-fan-control.service
sudo journalctl -u dgx-fan-control.service -n 30
```

A successful restart must be followed by state, RPM, and temperature checks
under load, automatic restoration, and a longer soak before claiming stability.

## Fans stay fast, or curve stops following temperature

`automatic` removes the common lower clamp. Firmware hysteresis may keep the
fans fast until its own policy decides to reduce them. Check RPM and temperature
over time. The daemon's sensor-failure response requests maximum cooling; its
journal explains those transitions.

Stop the daemon before using `set-state`. Version 0.1.1 rereads the kernel state
on every sample, so normal suspend/resume needs no manual restart. Version
0.1.0 needs `sudo systemctl restart dgx-fan-control.service` after resume.

For an issue report, include board model, OS/kernel and firmware versions,
module version, the commands used, and relevant log excerpts. Remove hostnames,
serial numbers, network addresses, and unrelated application output.
