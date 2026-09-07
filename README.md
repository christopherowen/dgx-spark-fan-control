# DGX Spark Fan Control

**Pre-cool your Spark, run your job, return to NVIDIA's fan curve.**

Control a shared minimum fan RPM from Linux userland, or run a temperature-based
performance curve. A small kernel driver talks to the existing firmware; a
Python command controls it. NVIDIA's embedded controller (EC) continues to
apply its own cooling policy throughout.

## Start here

| I want to… | Guide |
| --- | --- |
| Install with Secure Boot | [Installation, signing, and enrollment](docs/installation.md) |
| Rebuild automatically for kernel updates | [DKMS installation](docs/installation.md#3a-dkms-builds-for-kernel-updates) |
| Build and sign a module myself | [Manual installation](docs/installation.md#3b-manual-build-and-sign) |
| Choose what runs at boot | [Startup modes](docs/installation.md#5-choose-your-startup-mode) |
| Turn the fans up before a big job | [Pre-cooling](docs/usage.md#pre-cool-before-a-big-job) |
| Restore the factory fan curve afterward | [NVIDIA automatic control](docs/usage.md#return-to-nvidia-automatic-control) |
| Upgrade or remove the software | [Updates and removal](docs/maintenance.md) |

## A typical session

After [installation](docs/installation.md), stop the optional performance
service before selecting a manual floor:

```sh
sudo systemctl stop dgx-fan-control.service
sudo dgx-fan-control set-state 12   # maximum floor: up to 9000 / 13500 RPM
dgx-fan-control status
```

Allow time for the fans to accelerate and temperatures to fall, then run your
workload normally. When finished, remove the added floor:

```sh
sudo dgx-fan-control automatic
dgx-fan-control status             # state=0/12 means NVIDIA automatic control
```

Leave the service stopped to keep NVIDIA's curve active. Starting the service
selects this project's more aggressive curve. The [usage guide](docs/usage.md)
includes a Bash job example that restores automatic control on ordinary exit
or interruption.

## What you control

The driver exposes two read-only RPM sensors through Linux `hwmon`, and a
thermal cooling device with state 0 (automatic) plus 12 common RPM floors.
These are **minimums, not exact per-fan targets**. Each fan saturates at its own
maximum, and firmware can ask for more cooling than your floor.

There is no firmware flashing, upper-clamp control, arbitrary EC memory access,
or raw PWM interface. This cannot make the fans quieter than NVIDIA's own
policy. See the [state table and performance curve](docs/usage.md#choose-a-fan-floor).

## Compatibility

Originally tested on two **NVIDIA DGX Sparks, board P4242**, with DGX OS 7.5.0,
kernel **`6.17.0-1029-nvidia`**, and EC firmware **3.5.8**. Other GB10 systems,
OEM variants, generic kernels, and future firmware are unverified. The driver
rejects platform or protocol responses outside its observed contract.

You need Python 3.10+, matching NVIDIA kernel headers and build tools, and an
enrolled local signing certificate when Secure Boot is enabled. DKMS is optional;
systemd provides boot loading and the optional performance service.

This is an independent, experimental project, unaffiliated with NVIDIA.
Version 0.1.1 fixes lost-write ownership recovery and resume synchronization,
and stops persistent communication failures from causing endless service
restarts. Both affected 0.1.0 systems were recovered without rebooting on
2026-09-07 and upgraded to 0.1.1. The underlying firmware ordering defect remains;
long-term stability is not yet qualified. See
[recovery instructions](docs/troubleshooting.md#persistent-pending-and-service-exit-69).
Orderly stop, suspend, reboot, and module removal request and verify automatic
control. A hard crash or transport failure can prevent restoration; manual
floors have no expiry timer. Read the [validation and limitations](docs/validation.md).

## Development

Run `./scripts/check` (Python 3.10+ and a C compiler) for hardware-free policy,
failure-path, compiled C transaction, signing-wrapper, and source-contract tests
plus shell syntax checks. GitHub Actions runs the same checks. Kernel
compilation and real EC behavior need a compatible Spark.

- [Firmware protocol](docs/protocol.md)
- [Firmware pending-state investigation](docs/firmware-pending-analysis.md)
- [Validation record](docs/validation.md)
- [Troubleshooting](docs/troubleshooting.md)
- [Contributing](CONTRIBUTING.md)

## License

[GPL-2.0-only](LICENSE), matching the original kernel driver's SPDX identifier.
Copyright 2026 Christopher Owen.
