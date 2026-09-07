# One-shot mailbox diagnosis and recovery

This operator tool recovered two DGX Sparks without rebooting on 2026-09-07.
It is separate from the installed driver, DKMS package, and performance daemon.
Read the [firmware evidence and live results](../../docs/firmware-pending-analysis.md)
first. An idle physical mailbox with a stale secure pending flag is the
**demonstrated case**. This is not a general EC reset or a firmware fix.

## Prerequisites

- NVIDIA DGX Spark, board P4242; kernel `6.17.0-1029-nvidia`; SoC 2.155.11;
  EC 3.5.8. Verify the firmware using `fwupdmgr get-devices`; the helper does
  not independently authenticate the running firmware image.
- Keep the existing fan driver loaded. Stop its performance service and other
  fan-policy writers, retain the logs, and confirm no automatic restart loop.
- The helper recognizes the audited driver pairs only:

  | Version | Original installed build | Rebuild with normalized author metadata |
  | --- | --- | --- |
  | 0.1.0 | `FFB84C2FB9D4979B29876AD` | `7BEC2E2D9DF9895B99E207A` |
  | 0.1.1 | `81A2F2BDE37A05AD960065A` | `340672C7AB99E30FB20D4A3` |

  Author metadata changes the source fingerprint without changing behavior or
  structure layout. Both rebuilt fingerprints were verified against the same
  target headers; retaining the original pairs permits recovery of existing
  installations.

- Build with the running kernel's exact headers. The helper uses the driver's
  private structure prefix to acquire its mutex; this is **not a stable kernel
  ABI**. The live investigation independently checked installed-module DWARF
  offsets: `ffa_dev=0`, `cooling_dev=8`, `reboot_notifier=16`, `lock=40`,
  `last_updated=72`, `current_state=80`. A changed build/layout requires a new
  audit, not removal of the version guards.
- Secure Boot requires signing with an already-enrolled local key. See
  [key generation and enrollment](../../docs/installation.md).

## Build and observe

From this repository's root:

```sh
sudo systemctl stop dgx-fan-control.service
cat /sys/module/dgx_ec_fan_control/version
cat /sys/module/dgx_ec_fan_control/srcversion
cat /proc/sys/kernel/random/boot_id
make -C research/mailbox
```

Sign locally; adjust these two key paths to the enrolled pair on this machine.
Do not copy private keys between machines or commit them:

```sh
sudo /lib/modules/"$(uname -r)"/build/scripts/sign-file sha256 \
  /path/to/MOK.priv /path/to/MOK.der \
  research/mailbox/dgx_ec_mailbox_probe.ko
sudo insmod research/mailbox/dgx_ec_mailbox_probe.ko
sudo journalctl -k --since '2 minutes ago' | grep dgx_ec_mailbox_probe
sudo rmmod dgx_ec_mailbox_probe
```

A successful insertion runs once and leaves an inert helper module to remove.
A failed insertion leaves no helper loaded; inspect the reported errno and
kernel log. Always remove a successfully loaded helper before another invocation.

Default observation sends cached fan polls and four fixed OEM reads: the
six-byte BCD clock at `0x06000788`, status at `0x06000504` twice, and five bytes
of response at `0x06000800`. It sends no fan request or setter. Secure read
wrappers can drain queued events, so a read can advance firmware completion.
There is no arbitrary address parameter, acknowledgement-register read, direct
MMIO, or firmware-memory modification.

The affected machines reported `poll=2->2 mailbox=0x8,0x8`, plausible clock
bytes, and a shared-mailbox response from family `0x10` or `0x11`. Bits 0–1 of
`0x08` are clear. Zero-filled OEM output alone is not proof of an idle mailbox:
the OEM handler may hide an eSPI read error in a zeroed response.

## Recover the stale pending flag

After reviewing the observation, one explicit invocation may allow one fan
**read-lower-floor** request:

```sh
sudo insmod research/mailbox/dgx_ec_mailbox_probe.ko recover=1
sudo journalctl -k --since '2 minutes ago' | grep dgx_ec_mailbox_probe
sudo rmmod dgx_ec_mailbox_probe
```

The helper repeats all observations while holding the existing owner's mutex.
It requires pending before and after, equal idle status, a plausible BCD clock,
and a recognized response family. The secure sender independently checks the
physical mailbox at submission time. The helper submits only operation 4, then
polls at most 20 times at 50 ms. It never resubmits in a loop or changes a fan
setting. Success reports submit status 0 and poll 0. A busy, changed, unreadable,
or unrecognized result is a refusal to proceed.

A successful read restores communication, **not necessarily automatic fan
control**. Record the returned floor. Check `dgx-fan-control status` and RPMs.
Version 0.1.1 can reconcile its own uncertain write. Version 0.1.0 may still
reject the EC's actual floor because it only remembers its last confirmed floor.

## Remove an explicitly identified floor

Prefer the installed driver's `sudo dgx-fan-control automatic` when ownership
is intact. If the old driver refuses an ownership mismatch, investigate the
observed value and logs. Merely recognizing a value from the state table does
not establish who set it.

For an operator decision to remove that **specific observed floor**, the helper
has a separate mode. For example, the recovered Spark 1 had floor 12375:

```sh
# Example only: use the value actually investigated on this machine.
sudo insmod research/mailbox/dgx_ec_mailbox_probe.ko restore_floor=12375
sudo journalctl -k --since '2 minutes ago' | grep dgx_ec_mailbox_probe
sudo rmmod dgx_ec_mailbox_probe
```

This mode refuses simultaneous `recover=1`, non-idle state, an invalid clock,
and values outside the observed floor range. Before any setter it performs two
fresh operation-4 reads. Each must match the requested floor in both the secure
cache and the physical response buffer, with a successful family-7/operation-4
header and idle status. Shared-mailbox interference causes `ESTALE`; the tool
never relaxes the comparison. It then sends exactly one lower-floor **UNSET**
(`0xffff`) and verifies UNSET twice through both paths. It cannot write a new
RPM floor or alter an upper clamp. Failed readback is not a claim of restoration.

The helper never edits the loaded driver's ownership fields. After successful
UNSET, an old 0.1.0 driver's ordinary removal independently recognizes UNSET and
clears its saved state. This permitted the two machines to follow the normal
[manual module update](../../docs/maintenance.md) to 0.1.1, then verify state 0
and restart the performance service. Do not unload the old driver as a way to
skip failed recovery or erase its ownership evidence.

## Limits and testing

This tool does not prevent the firmware race recurring. It does not recover a
busy mailbox, guarantee fresh cached replies generally, or justify an automatic
retry loop. A single successful recovery is not a long-term stability result.
The application driver retains its pending preflight and bounded failure policy.

The normal `scripts/check` suite compiles the actual diagnostic functions with
strict fake FF-A boundaries. It tests refusal conditions, transport failures,
read-only recovery, bounded polling, physical/cache disagreement, and the single
UNSET restriction. Real module ownership/locking and firmware behavior require
the separate target build and hardware observations documented above.
