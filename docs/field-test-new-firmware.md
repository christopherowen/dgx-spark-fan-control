# How to test: a unit carrying the newest firmware

This is the procedure for someone standing at a Spark, testing
`firmware-agnostic-transport` (version 0.2.0) on firmware that has never been
exercised with this code. It exists because the people who wrote the change do
not currently have a machine with SoC `2.158.15` or `4.1.0` installed.

Read the whole page before running anything. Loading the module is **not** a
purely observational act (probing can restore a stopped clamp), and §8's write
test has a one-way effect on native ACPI.

## 0. What is proven, what this test finds out, and what to send back

| firmware | known before this test | what this test finds out |
| --- | --- | --- |
| `ACUM027` / SoC 2.155.11 | the EC packet service answers; floor writes moved the fans; a 13500 rpm request read back 8820/12420 | whether this revised binding still works on the firmware it was measured against |
| `ACUM028` / SoC 2.158.15 | only its ACPI: `FAN0` with `_FSL`/`_FST`/`WFCF`, and `_FSL` itself issues `SMC 0xC400008D` to endpoint `0x8003` | what service the firmware actually publishes, and whether native ACPI fans are left to their own driver |
| `0ACUM10` / SoC 4.1.0 (ASUS) | its DSDT is byte-identical to `ACUM027`'s: no fan device | whether the packet fan service is present at all |

**The deliverable is the kernel-log block for the load, not one line** — the
messages differ by transport:

- **native ACPI selected** — the ACPI candidate and `transport=ACPI` messages,
  plus the fact that no packet device was claimed. FF-A discovery fields
  (reported version, `vm_id`, `properties`) are **not collected on this path**;
  report them as not applicable rather than missing.
- **packet transport selected** — the transport-validation line (UUID, reported
  and raw FF-A version, `vm_id`, `properties`, mode) and the packet-selection
  line that follows it.
- **nothing bound** — the rejection messages naming the stage, which are a
  result in themselves.

## 1. Prerequisites — before anything else

1. **Make the GRUB menu visible.** `/etc/default/grub` is overridden by
   `/etc/default/grub.d/*.cfg` sourced alphabetically, and `no-grubmenu.cfg`
   (`GRUB_TIMEOUT=0`, hidden) wins. Create `/etc/default/grub.d/zz-timeout.cfg`:

   ```sh
   GRUB_TIMEOUT=10
   GRUB_TIMEOUT_STYLE=menu
   ```

   Then regenerate the bootloader configuration **on every kernel**, not only
   for the workaround below:

   ```sh
   sudo update-grub
   ```

2. **`kho=off` on a 7.0.0-1019 kernel.** Add `kho=off` to
   `GRUB_CMDLINE_LINUX_DEFAULT`, `sudo update-grub`, reboot, then confirm from
   the kernel's own command line:

   ```sh
   grep -o 'kho=off' /proc/cmdline     # must print kho=off
   ```

   `sudo ls /sys/kernel/debug/kho` failing is *not* sufficient evidence: with
   debugfs unmounted it fails whether or not the argument applied. Keep
   `console=tty0 console=ttyS0,921600` if it is already there.

3. **Do not install `7.0.0-1024`.** It has reboot-loop and Oops reports. Hold the
   kernel packages before any update that could pull it.

4. **Toolchain, headers and inventory tools** (reuse `docs/installation.md` §1):

   ```sh
   sudo apt-get update
   sudo apt-get install git build-essential linux-headers-"$(uname -r)" \
        python3 openssl mokutil dkms fwupd dmidecode
   command -v fwupdmgr dmidecode mokutil uname    # all four must resolve
   ```

   Another kernel's headers are not a substitute. If matching headers are
   unavailable, resolve that through the DGX OS package repositories before
   continuing — do not substitute another release.

5. **Record what you are testing**, before interpreting anything:

   ```sh
   uname -a
   sudo fwupdmgr get-devices      # SoC/system and EC entries, with versions
   mokutil --sb-state
   ```

   The SoC and EC versions come from the firmware inventory, not from `uname`
   and not from this driver. An inventory *entry that is not exposed* is
   reported as unavailable; a *missing tool* is a different thing and must be
   installed, not reported as absent firmware.

## 2. Get the branch

```sh
git clone https://github.com/othexmr/dgx-spark-fan-control.git
cd dgx-spark-fan-control
git checkout firmware-agnostic-transport
./scripts/check          # 27 hardware-free tests, about one second
```

If `./scripts/check` fails, read its actual error — it needs `python3` and a C
compiler for the test shims, so a missing toolchain looks like a damaged
checkout. **Resolve it and rerun until it passes before building or loading
anything**: a checkout whose own checks fail is not a starting point for a
hardware test.

## 3. Make sure the branch's module is the one that runs

Installing a new `.ko` and running `depmod` **does not replace a module already
in memory**, and `modprobe` will then succeed while the old implementation keeps
running — which would make a "successful" test meaningless. `modinfo` reads the
file on disk, so it cannot answer what is loaded.

Check immediately **before** the final `modprobe` in §4, and again after any
reboot this procedure asks you to perform (the MOK enrolment reboot is one), so
the check cannot go stale:

```sh
lsmod | grep dgx_ec_fan_control                 # empty means nothing loaded
cat /sys/module/dgx_ec_fan_control/version 2>/dev/null
cat /sys/module/dgx_ec_fan_control/srcversion 2>/dev/null
```

Compare `srcversion` with the candidate you just built:

```sh
modinfo -F srcversion kernel/dgx_ec_fan_control.ko
```

If the loaded `srcversion` differs, an older module is running: follow
`docs/maintenance.md` (restore, verify, then unload) and repeat the install.
**Do not** unload an owner whose floor state you have not verified, and do not
unload merely to "start clean". If restoration fails, keep it loaded, preserve
the logs, and follow `docs/troubleshooting.md`.

## 4. Build and load

### Secure Boot off — fewer moving parts

```sh
make -C kernel
sudo install -D -m 0644 kernel/dgx_ec_fan_control.ko \
     /lib/modules/$(uname -r)/updates/dgx_ec_fan_control.ko
sudo depmod -a
```

### Secure Boot on — sign first

Follow `docs/installation.md` §2: generate a key, `mokutil --import`, then
**Enroll MOK at the firmware screen** (physical console), then:

```sh
./scripts/build-sign
sudo install -D -m 0644 kernel/dgx_ec_fan_control.ko \
     /lib/modules/$(uname -r)/updates/dgx_ec_fan_control.ko
sudo depmod -a
modinfo -F signer dgx_ec_fan_control   # expect your key's signer
```

### Establish independent temperature observation *before* loading

Probing can restore a stopped clamp, so loading is not observation-only. Start a
read-only watcher in a second terminal and leave it running:

```sh
while :; do
    printf '%s ' "$(date +%T)"
    for z in /sys/class/thermal/thermal_zone*/temp; do
        printf '%s=%s ' "$(basename "$(dirname "$z")")" "$(cat "$z")"
    done
    echo
    sleep 5
done
```

If you cannot observe temperature independently (no zone files, no `sensors`),
**stop before loading** and say so in the report.

### Load

```sh
sudo modprobe dgx_ec_fan_control
```

## 5. Capture

```sh
sudo journalctl -k -b --no-pager > /tmp/kernel-this-boot.log
```

Keep the whole block. The messages you need are emitted by the driver's
hyphenated device name and, on the ACPI path, by the native fan device — a
narrow `grep dgx_ec` misses both, which is exactly how a real result gets lost.
If `journalctl` is unavailable, use `sudo dmesg`.

Also capture:

```sh
cat /sys/class/thermal/cooling_device*/type
for d in /sys/class/thermal/cooling_device*; do
    printf '%s %s\n' "$(cat $d/type)" "$(cat $d/cur_state)"
done
grep -H . /sys/class/hwmon/hwmon*/name /sys/class/hwmon/hwmon*/fan*_input 2>/dev/null
ls -l /sys/bus/platform/devices/PNP0C0B:*/thermal_cooling 2>/dev/null   # native ownership links;
                                                       # no matches is expected on packet-only firmware
```

Every `fan*_input` value is a **duty-derived estimate**, not a tachometer
measurement. Report it as an estimate.

## 6. How to read the result

| observation | meaning |
| --- | --- |
| ACPI candidate selected, `transport=ACPI` logged, no packet floor cooling device registered | **correct on `ACUM028`**: native ACPI fans stay with their own driver and nothing duplicates them |
| packet transport selected *and* a floor cooling device registered while ACPI fan devices exist | **stop, write nothing**, send the log; the duplicate-writer guard failed |
| packet transport selected, floor cooling device registered, initial state readable | successful FF-A bind — this is the row that authorises §8 on a packet unit |
| `modprobe` fails with a message naming a stage (uuid, version, missing operation, unsupported platform) | the diagnostics worked; send the log — this is a result, not a failure |
| `modprobe` fails with a signature or vermagic message | Secure Boot or kernel/ABI mismatch, not a firmware finding |
| binds, then any thermal excursion, unexpected reboot, or fan stopping | **stop, reboot, report**; do not retry |

**The two transports are not primary and fallback; they belong to different
firmware generations.** `ACUM028` declares fan devices, so ACPI is its route;
`ACUM027` and `0ACUM10` declare none, so the packet path is *their* route. Both
are the only route on the firmware that has them.

On the newest firmware, ACPI is expected to work and the packet service may
simply no longer be published: its `_FSL` drives the fans through
`SMC 0xC400008D` to endpoint `0x8003`, but that endpoint carries more than one
service, and the packet fan service is a different one that nobody has measured
there.

- packet transport selected in `auto` on a firmware that declares ACPI fans →
  **record it as a finding**; ACPI selection failed for a reason worth having;
- a packet refusal in `auto` is only evidence about the packet service if the
  log shows it was asked — with declared ACPI fans it is not asked at all.

**Platform applicability matters before you interpret a refusal.** The module
accepts only the platform identities it declares (NVIDIA / `NVIDIA_DGX_Spark` /
`P4242`). A refusal on an ASUS or Dell unit may therefore mean "outside the
declared platform contract", not "the firmware changed" — record the unit's DMI
identity (`sudo dmidecode -s system-manufacturer; sudo dmidecode -s system-product-name`)
alongside the log so the two cannot be confused.

If the transport is `ACPI`, do **not** run the userspace floor daemon or the
`automatic` command: ACPI level 0 is not "automatic" on this firmware.

## 7. Asking whether the packet service exists at all

`auto` selection never asks for the packet service where fan devices are
declared, so it cannot answer whether that service still exists on the newest
firmware. The module therefore takes `transport=`:

| value | behaviour |
| --- | --- |
| `auto` (default) | ACPI where fan devices are declared, packet otherwise — the supported mode |
| `acpi` | ACPI only; refuses rather than falling through to FF-A |
| `probe` | **report only**: enumerates the FF-A bus for the packet service, registers nothing and writes no register |

`probe` is the experiment. It is deliberately not a forced packet mode: binding
the packet driver on a firmware whose ACPI already owns the clamps would make
this module a concurrent writer of both, and its restore path clears only the
lower clamp — that is how a machine ends up with a modified clamp nobody asked
for. Discovery costs nothing and answers the same question.

**The module must not already be loaded.** A module parameter is read once at
load: `modprobe` on a module that is already present succeeds without rerunning
init, so the experiment would silently run the loaded `auto` instance and the
cleanup below would then remove that instance — which on a packet unit can strand
a floor.

```sh
lsmod | grep dgx_ec_fan_control       # must print nothing
sudo modprobe --first-time dgx_ec_fan_control transport=probe
cat /sys/module/dgx_ec_fan_control/parameters/transport   # must read: probe
sudo journalctl -k -b --no-pager | tail -20
sudo modprobe -r dgx_ec_fan_control   # safe only because probe registered nothing
```

If the module is already loaded, do not remove it to make room: follow
`docs/maintenance.md` to restore and verify automatic state first, or skip this
section. `--first-time` makes a loaded module an error instead of a silent no-op.

Read the `transport=probe discovery only:` line:

| result | meaning |
| --- | --- |
| `1` or more packet service(s) matched, with its version/`vm_id`/`properties` | the packet service **still exists** on this firmware — a result nobody has |
| `0 … matched` | the service is **absent or published under a changed UUID**; the count is the finding |

`probe` itself writes nothing and registers nothing, so it can run on a serving
unit — but only when the module is not already loaded, because the load is what
runs it (§7 above). If the DMI check rejects the unit first, the experiment says
nothing about the service: that refusal is about the platform identity, not the
firmware.

## 8. Optional: the write test## 8. Optional: the write test

Only on an idle unit, only with the owner's explicit agreement, only with the
temperature watcher from §4 still running, and only if §6 shows a clean result
for that transport. Stop the test on any unexpected temperature rise, any reboot
you did not ask for, or any change in fan noise you cannot explain.

### Packet transport — use the project's own command

Install the userspace command once (the installation guide does this in §4), then
drive the floor through it rather than writing sysfs by hand. `set-state` writes
one state and asserts the readback; `automatic` writes and **verifies** the unset
clamp.

```sh
sudo install -D -m 0755 userspace/dgx_fan_control.py /usr/local/sbin/dgx-fan-control
dgx-fan-control status                 # e.g. state=0/12: found, no floor set

sudo dgx-fan-control set-state 1        # one step up; the command verifies the readback
dgx-fan-control status

sudo dgx-fan-control automatic          # restores and verifies the unset clamp
dgx-fan-control status
```

The tool refuses if the floor device is absent, which is the correct outcome on a
host where native ACPI owns the fans — that refusal is a result to report, not an
error to work around. If `automatic` fails, **do not unload**: the driver still
owns the floor. Keep it loaded, preserve the log, and follow
`docs/maintenance.md`.

### Native ACPI transport — not recommended

Every `_FSL` write pins **both** clamps, not just a floor. Raising the level by
one therefore installs an *upper* clamp at that level, which can cap the
firmware's own cooling demand, and neither unloading this module nor the packet
path's automatic restoration clears it: only a reboot does. `docs/usage.md`
warns about exactly this.

For that reason this section is optional and read-only by default. Observing the
native levels, and stopping there, is the recommended result for the ACPI
transport:

```sh
ls -l /sys/bus/platform/devices/PNP0C0B:*/thermal_cooling   # native fan -> its cooling device
cdev=$(readlink -f /sys/bus/platform/devices/PNP0C0B:00/thermal_cooling)
cat "$cdev/cur_state" "$cdev/max_state"      # record; stop if either read fails
```

Continue only if the owner has explicitly accepted a **temporary cap on
firmware cooling until the machine is rebooted**:

```sh
cur=$(cat "$cdev/cur_state")
printf '%s\n' "$((cur + 1))" | sudo tee "$cdev/cur_state"
cat "$cdev/cur_state"        # must read cur+1
sudo systemctl reboot        # the only release: no ACPI restore, no software clear
```

After the reboot confirm the level is back and temperatures are as before. If any
step refuses, abort and report the log; do not retry with a different value.

## 9. Report back

```
unit:            Founders / ASUS Ascent GX10 / Dell FCM1253 / other
DMI:             system-manufacturer / system-product-name
kernel:          uname -a
firmware:        SoC and EC versions from fwupdmgr (or "not exposed")
secure boot:     on/off, MOK state if on
transport:       ACPI / packet / none bound
stage reached:   5 / 6 / 7
kernel log:      (paste the load block from journalctl -k -b)
cooling devices: (paste type + cur_state + thermal_cooling symlinks)
hwmon values:    (paste, labelled estimates)
temperatures:    (paste before, during, after)
result:          bind / refused / wrote / aborted
notes:           fan noise, behaviour, anything unexpected
```

## 10. What not to do

- Do not write on a serving unit.
- Do not unload to "start clean": removal can strand a floor if restoration
  fails, and for native ACPI, unloading neither releases nor restores the clamp.
- Do not treat a successful `modprobe` as proof that the branch's code is what
  loaded — verify the loaded version (§3).
- Do not test on a machine whose cooling you cannot physically hear or feel.
- Do not treat a successful bind as proof that fan control works: it proves
  discovery.
- Do not run the floor daemon alongside native ACPI fans.
