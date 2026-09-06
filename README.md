# DGX Spark Fan Control

**More cooling for NVIDIA DGX Spark, controlled from Linux userland.**

A small kernel driver exposes fan RPM and a shared minimum-RPM setting through
standard Linux interfaces. A Python controller adds a temperature-based fan
curve. NVIDIA's embedded controller (EC) continues to run its own thermal policy
and can request more cooling at any time.

This is an independent, experimental project. It is not affiliated with or
supported by NVIDIA. It requires a locally built kernel module; Python alone
cannot access the firmware interface.

## What it does

- Reports both fans through `hwmon` (`fan1_input`, `fan2_input`).
- Exposes 12 common RPM floors plus an automatic state through a thermal
  cooling device (`dgx_ec_fan_floor`).
- Runs a smooth userland curve with fast increases and slower, hysteretic decreases.
- Uses the existing Arm FF-A firmware packet relay. No firmware flashing.
- Only raises the lower fan limit. No upper limit, arbitrary EC writes, or raw PWM.
- Requests and verifies automatic mode on orderly controller stop, suspend,
  reboot, and driver removal. Restoration can fail if the transport fails.

The setting is a **common minimum**, not an exact or independent target for each
fan. Each channel saturates at its own maximum; firmware can run it above the
requested floor. This cannot make the fans quieter than NVIDIA's own policy.

## Compatibility

The original implementation was built, signed, and exercised on two NVIDIA DGX
Sparks on **2026-08-15**, using **`6.17.0-1029-nvidia`**. The recorded EC version
was **3.5.8**. Earlier firmware was inspected statically, not qualified for live
control. See [validation and limitations](docs/validation.md).

The driver deliberately checks NVIDIA/P4242 platform identity, FF-A 1.2, the
packet relay contract, and the observed RPM ranges (1260–9000 and 1890–13500).
Other GB10 systems, OEM variants, generic kernels, and future firmware are
unverified. Do not bypass these checks to force a load.

You need Python 3.10+, systemd for the optional daemon, and a compiler plus
headers for the exact target NVIDIA kernel. Secure Boot also requires your own
enrolled signing certificate. There are no third-party Python dependencies.

## Build and install

Run these commands **on the Spark**, from the checkout:

```sh
git clone https://github.com/christopherowen/dgx-spark-fan-control.git
cd dgx-spark-fan-control
sudo apt-get install build-essential linux-headers-"$(uname -r)" python3 openssl mokutil
./scripts/check
```

If you already have an enrolled module-signing key, set `DGX_MOK_DIR` to its
directory (containing `MOK.priv`, mode `0600`, and `MOK.der`). Otherwise create
your own key and enroll its certificate:

```sh
./scripts/generate-signing-key
sudo mokutil --import "$HOME/.local/share/dgx-spark-fan-control/keys/MOK.der"
# Reboot when convenient and complete enrollment in the firmware MOK manager.
mokutil --test-key "$HOME/.local/share/dgx-spark-fan-control/keys/MOK.der"
./scripts/build-sign
```

Key enrollment needs preboot interaction; plan console access. The private key
stays outside the repository. See [Ubuntu's Secure Boot guide](https://documentation.ubuntu.com/security/docs/security-features/platform-protections/secure-boot/)
for the platform signing model. The script builds and signs only; it does not
install, load, or restart anything. For systems intentionally running without
signature enforcement, `make -C kernel` builds an unsigned module.

Install and load the module, then verify its interface before enabling a curve:

```sh
sudo install -D -m 0644 kernel/dgx_ec_fan_control.ko \
  "/lib/modules/$(uname -r)/updates/dgx_ec_fan_control.ko"
sudo depmod -a
sudo modprobe dgx_ec_fan_control
sudo install -D -m 0755 userspace/dgx_fan_control.py /usr/local/sbin/dgx-fan-control
dgx-fan-control status
```

Expected status starts with `state=0/12`. A successful `modprobe` alone does not
prove that the FF-A device bound; the status check must find exactly one device.
Then install the optional boot-time controller:

```sh
sudo install -D -m 0644 systemd/dgx_ec_fan_control.conf \
  /etc/modules-load.d/dgx_ec_fan_control.conf
sudo install -D -m 0644 systemd/dgx-fan-control.service \
  /etc/systemd/system/dgx-fan-control.service
sudo systemctl daemon-reload
sudo systemctl enable --now dgx-fan-control.service
systemctl status dgx-fan-control.service
```

The service runs as root to write the thermal sysfs attribute, with an empty
capability set, no network access, and a read-only system filesystem. It does
not have permission to load modules.

## Use

```sh
dgx-fan-control status
sudo journalctl -u dgx-fan-control.service -n 30
```

Find the RPM attributes by name; the numeric `hwmon` index changes across boots:

```sh
for device in /sys/class/hwmon/hwmon*; do
  if [ "$(cat "$device/name" 2>/dev/null)" = dgx_ec_fan ]; then
    cat "$device/fan1_input" "$device/fan2_input"
  fi
done
```

For a manual floor, first stop the daemon so there is only one policy writer:

```sh
sudo systemctl stop dgx-fan-control.service
sudo dgx-fan-control set-state 3   # 4500 RPM common floor
sudo dgx-fan-control status
sudo dgx-fan-control automatic   # remove the added floor
sudo systemctl start dgx-fan-control.service
```

A manual setting persists until changed; it has no timer. To leave NVIDIA
automatic control active, leave the daemon stopped after `automatic`.

| State | Common floor (RPM) | State | Common floor (RPM) |
| ---: | ---: | ---: | ---: |
| 0 | Automatic / unset | 7 | 8,100 |
| 1 | 2,700 | 8 | 9,000 |
| 2 | 3,600 | 9 | 10,125 |
| 3 | 4,500 | 10 | 11,250 |
| 4 | 5,400 | 11 | 12,375 |
| 5 | 6,300 | 12 | 13,500 |
| 6 | 7,200 | | |

The default curve uses the hottest valid kernel thermal zone:

| Filtered temperature | Target state |
| ---: | ---: |
| Below 50°C | 0 |
| 50°C | 3 |
| 55°C | 5 |
| 60°C | 8 |
| 65°C | 10 |
| 70°C or above | 12 |

It samples every two seconds, applies an exponential filter (`alpha=0.35`),
rises by at most two states per sample, and falls by at most one with 4°C
hysteresis. If no valid temperature readings remain, it requests state 12
immediately. Invalid individual sensors are skipped. Curve constants live in
[`userspace/dgx_fan_control.py`](userspace/dgx_fan_control.py).

## Kernel updates and removal

There is no automatic DKMS rebuild. Before booting a new kernel, build and sign
against its installed headers with `KERNEL_RELEASE=<new-release>
./scripts/build-sign`, install the resulting module under that release's
`updates/` directory, and run `sudo depmod -a <new-release>`. Never copy a module
built for another kernel. NVIDIA's normal OS and firmware update process remains
unchanged; compatibility with a new release still needs checking.

To uninstall, restore automatic control **before** removing the driver:

```sh
sudo systemctl disable --now dgx-fan-control.service
sudo dgx-fan-control automatic
dgx-fan-control status                    # must report state=0/12
sudo modprobe -r dgx_ec_fan_control
sudo rm -f /etc/modules-load.d/dgx_ec_fan_control.conf
sudo rm -f /etc/systemd/system/dgx-fan-control.service
sudo rm -f /usr/local/sbin/dgx-fan-control
sudo rm -f "/lib/modules/$(uname -r)/updates/dgx_ec_fan_control.ko"
sudo depmod -a
sudo systemctl daemon-reload
```

Also remove this module from any other kernel release where you installed it.
If restoration or readback fails, stop here and consult
[troubleshooting](docs/troubleshooting.md) before unloading.

## Development

`./scripts/check` runs hardware-free policy and contract tests plus shell syntax
checks. GitHub Actions runs the same checks. Kernel compilation and real EC
behavior require a compatible Spark; CI does not claim hardware validation.

Read the [protocol notes](docs/protocol.md), [validation record](docs/validation.md),
and [contribution guide](CONTRIBUTING.md) before changing the driver.

## License

[GPL-2.0-only](LICENSE), matching the original kernel driver's SPDX identifier.
Copyright 2026 Christopher Owen.
