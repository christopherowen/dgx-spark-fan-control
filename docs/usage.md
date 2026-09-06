# Fan control for everyday work

[← README](../README.md) · [Installation](installation.md) · [Maintenance](maintenance.md)

There are three modes:

| Mode | What controls the added floor | How to select it |
| --- | --- | --- |
| NVIDIA automatic | No added floor | Stop the service, then run `automatic` |
| Manual boost | Your selected state, until you change it | Stop the service, then run `set-state` |
| Performance curve | This project's temperature-based daemon | Start the service |

NVIDIA's thermal policy remains active in all three modes. Use one policy
writer at a time; stop the service before a manual boost.

## Pre-cool before a big job

### 1. Request maximum cooling

```sh
sudo systemctl stop dgx-fan-control.service
sudo dgx-fan-control set-state 12
dgx-fan-control status
```

State 12 requests the maximum common floor. The two fans can reach approximately
**9000 and 13500 RPM** respectively. They accelerate under firmware control;
setting a state does not instantly change their measured speed.

### 2. Let the machine cool, then run your workload

Watch the hottest reported temperature:

```sh
watch -n 2 dgx-fan-control status
```

Press Ctrl+C to stop watching. Allow tens of seconds for pre-cooling and check
actual temperatures and [RPM readings](#read-the-fan-rpms); the time needed
varies with ambient temperature and existing load. There is no universal
pre-cooling temperature or guaranteed performance improvement.

Run your training, inference, rendering, or other job as your normal user.
The manual floor remains active throughout, even if the command that set it
has already exited. A moderate boost is also available, for example
`sudo dgx-fan-control set-state 3` for a 4500 RPM common floor.

### 3. Return to NVIDIA automatic control

When the job finishes:

```sh
sudo dgx-fan-control automatic
dgx-fan-control status
```

Expect **`state=0/12`**. Leave the service stopped to retain the factory curve.
Firmware hysteresis can keep fans fast for a while after the floor is removed.

## Return to NVIDIA automatic control

From either manual or performance mode:

```sh
sudo systemctl stop dgx-fan-control.service
sudo dgx-fan-control automatic
dgx-fan-control status
```

To make that the default on future boots too:

```sh
sudo systemctl disable dgx-fan-control.service
```

The module may stay loaded for RPM monitoring and later manual boosts. There
is no need to unload it or reboot to return to the factory curve.

## Wrap a job with ordinary-exit cleanup

For terminal-driven jobs, this **Bash subshell** sets the floor, allows 30 seconds
of pre-cooling, runs a job, and attempts to restore the factory curve on exit,
including job failure and handled Ctrl+C/SIGTERM. Replace the marked job command:

```bash
(
  set -e
  sudo -v

  restore_automatic() {
    job_status=$?
    trap - EXIT
    if ! sudo dgx-fan-control automatic; then
      echo "Automatic restoration failed; check dgx-fan-control status and the kernel log." >&2
      exit 1
    fi
    exit "$job_status"
  }
  trap restore_automatic EXIT
  trap 'exit 130' INT
  trap 'exit 143' TERM

  sudo systemctl stop dgx-fan-control.service
  sudo dgx-fan-control set-state 12
  sleep 30
  ./your-job --your-options  # replace this line; run the workload without sudo
)
```

Keep the terminal available: `sudo` may request your password again after a
long job. This is not an unattended scheduler or a crash watchdog. Bash can
defer a signal trap while waiting for a foreground child; SIGKILL, loss of the
shell, or transport failure may prevent cleanup. A manual floor has no timer.
Always verify `dgx-fan-control status` after interruption. This example leaves
the performance service stopped, even if it was running before the job.

## Use the performance curve continuously

For automatic extra cooling as temperature rises:

```sh
sudo systemctl start dgx-fan-control.service
dgx-fan-control status
sudo journalctl -u dgx-fan-control.service -n 30
```

Use `sudo systemctl enable dgx-fan-control.service` if you also want this at
boot. This is **our performance curve**, not NVIDIA's factory curve:

| Filtered hottest temperature | Target state | Common floor (RPM) |
| ---: | ---: | ---: |
| Below 50°C | 0 | Automatic / unset |
| 50°C | 3 | 4,500 |
| 55°C | 5 | 6,300 |
| 60°C | 8 | 9,000 |
| 65°C | 10 | 11,250 |
| 70°C or above | 12 | 13,500 |

It samples every two seconds, smooths temperature with an exponential filter
(`alpha=0.35`), rises by at most two states per sample, and falls by at most
one with 4°C hysteresis. If no valid kernel thermal-zone readings remain, it
requests state 12 immediately. Individual invalid sensors are skipped.

Constants live in [`userspace/dgx_fan_control.py`](../userspace/dgx_fan_control.py).
After suspend/resume, restart the service to resynchronize its cached state:
`sudo systemctl restart dgx-fan-control.service`.

## Choose a fan floor

| State | Common floor (RPM) | State | Common floor (RPM) |
| ---: | ---: | ---: | ---: |
| 0 | Automatic / unset | 7 | 8,100 |
| 1 | 2,700 | 8 | 9,000 |
| 2 | 3,600 | 9 | 10,125 |
| 3 | 4,500 | 10 | 11,250 |
| 4 | 5,400 | 11 | 12,375 |
| 5 | 6,300 | 12 | 13,500 |
| 6 | 7,200 | | |

The same minimum is requested for both channels; each fan saturates at its
own maximum. Firmware can keep a fan above your chosen floor. Setting a lower
state does not force RPM below firmware demand. `set-state 0` and `automatic`
both remove the added floor.

## Read the fan RPMs

Find the hwmon device by name because numeric indexes change across boots:

```sh
for device in /sys/class/hwmon/hwmon*; do
  if [ "$(cat "$device/name" 2>/dev/null)" = dgx_ec_fan ]; then
    printf 'Fan 1: %s RPM\n' "$(cat "$device/fan1_input")"
    printf 'Fan 2: %s RPM\n' "$(cat "$device/fan2_input")"
  fi
done
```

`status` shows the requested state and hottest thermal zone; the hwmon files
show measured RPM. If a command fails, see [troubleshooting](troubleshooting.md).
