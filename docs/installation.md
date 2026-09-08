# Install on a DGX Spark

[← README](../README.md) · [Usage](usage.md) · [Updates and removal](maintenance.md)

Run commands on the Spark itself. Plan console access for the one-time signing
key enrollment: it includes a firmware-screen step during reboot.

The order is: **get the source → enroll a signing key → choose DKMS or manual
build → load and verify → choose startup behavior**.

## 1. Get the source and tools

```sh
sudo apt-get update
sudo apt-get install git build-essential linux-headers-"$(uname -r)" python3 openssl mokutil
git clone https://github.com/christopherowen/dgx-spark-fan-control.git
cd dgx-spark-fan-control
./scripts/check
uname -r
mokutil --sb-state
```

Keep this checkout for builds and updates. Commands below assume you are in its
root. If matching headers are unavailable, resolve that first through your
DGX OS package repositories. Another kernel's headers are not a substitute.

## 2. Generate and enroll your signing key

### Create a key, once

```sh
./scripts/generate-signing-key
```

The script creates these files outside the checkout:

| File | Purpose |
| --- | --- |
| `$HOME/.local/share/dgx-spark-fan-control/keys/MOK.priv` | Private signing key, mode `0600`; keep private |
| `$HOME/.local/share/dgx-spark-fan-control/keys/MOK.der` | Public certificate to enroll on the Spark |

It refuses to overwrite an existing pair. Reuse the enrolled pair for future
builds; do not generate a new key for every kernel update.

If you already maintain an enrolled pair named `MOK.priv` and `MOK.der`, skip
generation and select its directory:

```sh
export DGX_MOK_DIR="/absolute/path/to/your/keys"
```

The remaining commands use that override when set, otherwise the default path.

### Request enrollment

```sh
sudo mokutil --import "${DGX_MOK_DIR:-$HOME/.local/share/dgx-spark-fan-control/keys}/MOK.der"
```

Choose a temporary enrollment password. When ready to interrupt work:

```sh
sudo systemctl reboot
```

At the firmware MOK manager, select **Enroll MOK → Continue → Yes**, enter the
password, and complete the reboot. This needs the preboot console; SSH alone
does not complete enrollment.

### Verify after reboot

Return to the checkout. If using a custom `DGX_MOK_DIR`, export it again in the
new shell. Verify enrollment before building:

```sh
mokutil --test-key "${DGX_MOK_DIR:-$HOME/.local/share/dgx-spark-fan-control/keys}/MOK.der"
```

Expect a message saying the certificate is already enrolled. A generated key
or pending import alone is insufficient. See
[Ubuntu's Secure Boot documentation](https://documentation.ubuntu.com/security/security-features/platform-protections/secure-boot/)
for background.

If your machine intentionally runs without signature enforcement, enrollment
can be skipped. The manual unsigned build is below; changing Secure Boot
settings is not required by this project.

## 3A. DKMS: builds for kernel updates

Choose **this route or the manual route**, not both. DKMS registers module
source and uses the distribution's kernel-install hooks to rebuild it for new
kernels. Matching headers and successful compilation are still required.

```sh
sudo apt-get install dkms
dkms --version
```

These instructions target DKMS 3.x on Ubuntu-derived DGX OS. The included
[`dkms.conf`](../dkms.conf) builds only the control module and permits aarch64
targets. DKMS handles signing itself; it does not run `scripts/build-sign`.

### Set DKMS's persistent signing identity

First inspect existing DKMS signing settings:

```sh
sudo grep -H -E '^[[:space:]]*(mok_signing_key|mok_certificate)=' \
  /etc/dkms/framework.conf /etc/dkms/framework.conf.d/*.conf
```

No matches or an absent drop-in directory are normal on a fresh installation.
If DKMS already uses an enrolled key, retain it and verify that certificate
instead of changing the signing identity for your other modules.

For a new setup, copy the pair from step 2 to a persistent root-owned directory
so unattended builds do not depend on your home being mounted:

```sh
sudo install -d -m 0700 /root/.local/share/dgx-spark-fan-control/keys
sudo install -m 0600 \
  "${DGX_MOK_DIR:-$HOME/.local/share/dgx-spark-fan-control/keys}/MOK.priv" \
  /root/.local/share/dgx-spark-fan-control/keys/MOK.priv
sudo install -m 0644 \
  "${DGX_MOK_DIR:-$HOME/.local/share/dgx-spark-fan-control/keys}/MOK.der" \
  /root/.local/share/dgx-spark-fan-control/keys/MOK.der
sudo mokutil --test-key /root/.local/share/dgx-spark-fan-control/keys/MOK.der
sudoedit /etc/dkms/framework.conf
```

Set these entries, preserving unrelated settings:

```sh
mok_signing_key="/root/.local/share/dgx-spark-fan-control/keys/MOK.priv"
mok_certificate="/root/.local/share/dgx-spark-fan-control/keys/MOK.der"
```

These are **system-wide DKMS settings**. Check that no existing drop-in overrides
them. Verify the key and certificate paths printed during the build are the
intended, enrolled pair. See the
[DKMS signing documentation](https://github.com/dell/dkms/tree/v3.0.11#module-signing).

### Register, build, and install

From the checkout root, register only the files needed to build the module:

```sh
sudo install -d /usr/src/dgx-spark-fan-control-0.1.2/kernel
sudo install -m 0644 dkms.conf /usr/src/dgx-spark-fan-control-0.1.2/dkms.conf
sudo install -m 0644 kernel/Makefile kernel/dgx_ec_fan_control.c \
  /usr/src/dgx-spark-fan-control-0.1.2/kernel/
sudo dkms add -m dgx-spark-fan-control -v 0.1.2
sudo dkms build -m dgx-spark-fan-control -v 0.1.2 -k "$(uname -r)"
sudo dkms install -m dgx-spark-fan-control -v 0.1.2 -k "$(uname -r)"
dkms status -m dgx-spark-fan-control
modinfo -F signer dgx_ec_fan_control
```

Expect `installed` for the running kernel and the intended certificate subject
in `signer`. Continue with step 4. If previously installed manually, use the
[migration instructions](maintenance.md#switch-from-manual-installation-to-dkms)
first so an old module does not mask the DKMS version.

## 3B. Manual build and sign

This route requires repeating the build/install steps for each new kernel:

```sh
./scripts/build-sign
sudo install -D -m 0644 kernel/dgx_ec_fan_control.ko \
  "/lib/modules/$(uname -r)/updates/dgx_ec_fan_control.ko"
sudo depmod -a
modinfo -F signer dgx_ec_fan_control
```

The script uses the enrolled pair from step 2 and builds for the running
kernel. Run it as the user who owns the key; use `sudo` for installation.
The script itself never installs or loads a module.

For a system without signature enforcement, replace `./scripts/build-sign`
with `make -C kernel`, then use the same installation commands. An unsigned
module will not load under enforced Secure Boot.

## 4. Load the driver and verify automatic mode

Both build routes join here:

```sh
sudo install -D -m 0755 userspace/dgx_fan_control.py /usr/local/sbin/dgx-fan-control
sudo modprobe dgx_ec_fan_control
dgx-fan-control status
```

Status must start with **`state=0/12`**: the device was found and its added floor
is unset. Successful `modprobe` alone is insufficient; device probing can still
reject the firmware contract. See [troubleshooting](troubleshooting.md) if needed.

## 5. Choose your startup mode

Install the boot-load entry and optional service definition:

```sh
sudo install -D -m 0644 systemd/dgx_ec_fan_control.conf \
  /etc/modules-load.d/dgx_ec_fan_control.conf
sudo install -D -m 0644 systemd/dgx-fan-control.service \
  /etc/systemd/system/dgx-fan-control.service
sudo systemctl daemon-reload
```

Then choose one mode:

| Startup mode | Best for | Command |
| --- | --- | --- |
| NVIDIA factory curve; manual boosts when needed | Pre-cooling individual jobs | `sudo systemctl disable --now dgx-fan-control.service` |
| This project's performance curve | Continual temperature-based extra cooling | `sudo systemctl enable --now dgx-fan-control.service` |

For factory-curve mode, also clear any manual floor:

```sh
sudo dgx-fan-control automatic
dgx-fan-control status
```

The module loads at boot in state 0 in either mode. Enabling the service adds
our performance curve. `start`/`stop` change the current session;
`enable`/`disable` change future boots. Disabling the service alone does not
clear a manually set floor, which is why `automatic` is explicit above.

For the performance service, verify startup:

```sh
systemctl is-enabled dgx-fan-control.service
systemctl is-active dgx-fan-control.service
dgx-fan-control status
sudo journalctl -u dgx-fan-control.service -n 30
```

The service runs as root to write thermal sysfs, with an empty capability set,
no network access, and a read-only system filesystem. Module loading is separate.
Continue to [pre-cooling and everyday usage](usage.md).
