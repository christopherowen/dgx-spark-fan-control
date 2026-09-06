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

An ownership mismatch also causes failure: the driver will not overwrite an
unknown existing floor. Avoid concurrent writers. A floor written successfully
but followed by failed readback can leave the driver unable to authenticate
ownership; see the [known limitations](validation.md#known-limitations).

## Fans stay fast, or curve stops following temperature

`automatic` removes the common lower clamp. Firmware hysteresis may keep the
fans fast until its own policy decides to reduce them. Check RPM and temperature
over time. The daemon's sensor-failure response requests maximum cooling; its
journal explains those transitions.

Stop the daemon before using `set-state`. After suspend/resume, restart the
daemon to synchronize its cached state with the kernel's restored state:

```sh
sudo systemctl restart dgx-fan-control.service
dgx-fan-control status
```

For an issue report, include board model, OS/kernel and firmware versions,
module version, the commands used, and relevant log excerpts. Remove hostnames,
serial numbers, network addresses, and unrelated application output.
