#!/usr/bin/env python3
"""Smooth userland policy for the DGX Spark additive fan-floor device."""

from __future__ import annotations

import argparse
import logging
import math
import signal
import sys
import time
from pathlib import Path

COOLING_DEVICE_TYPE = "dgx_ec_fan_floor"
MAX_STATE = 12
POLL_SECONDS = 2.0
FILTER_ALPHA = 0.35
DOWN_HYSTERESIS_C = 4.0
UP_STATES_PER_POLL = 2
DOWN_STATES_PER_POLL = 1

# The curve becomes deliberately aggressive well below NVIDIA's stock 80 C
# second step. Firmware still takes the maximum of its own demand and this
# additive request.
PERFORMANCE_CURVE = (
    (50.0, 3),   # 4,500 common RPM floor
    (55.0, 5),   # 6,300
    (60.0, 8),   # 9,000
    (65.0, 10),  # 11,250 (fan 0 saturates at 9,000)
    (70.0, 12),  # 13,500 (both fans at their maxima)
)

LOG = logging.getLogger("dgx-fan-control")


def find_cooling_device(thermal_root: Path) -> Path:
    matches = []
    for candidate in sorted(thermal_root.glob("cooling_device*")):
        try:
            device_type = (candidate / "type").read_text(encoding="ascii").strip()
        except (OSError, UnicodeError):
            continue
        if device_type == COOLING_DEVICE_TYPE:
            matches.append(candidate)
    if len(matches) != 1:
        raise RuntimeError(
            f"expected one {COOLING_DEVICE_TYPE!r} cooling device, found {len(matches)}"
        )
    return matches[0]


def read_hottest_temperature_c(thermal_root: Path) -> float:
    readings = []
    for path in sorted(thermal_root.glob("thermal_zone*/temp")):
        try:
            millidegrees = int(path.read_text(encoding="ascii").strip())
        except (OSError, UnicodeError, ValueError):
            continue
        if 0 < millidegrees <= 200_000:
            readings.append(millidegrees / 1000.0)
    if not readings:
        raise RuntimeError("no valid kernel thermal-zone readings are available")
    return max(readings)


def curve_state(temperature_c: float) -> int:
    if not math.isfinite(temperature_c) or temperature_c <= 0:
        raise ValueError("temperature must be finite and positive")
    state = 0
    for threshold_c, threshold_state in PERFORMANCE_CURVE:
        if temperature_c < threshold_c:
            break
        state = threshold_state
    return state


def next_state(current: int, filtered_temperature_c: float) -> int:
    """Rate-limit the curve, with prompt increases and slow hysteretic falls."""
    if not 0 <= current <= MAX_STATE:
        raise ValueError("current cooling state is outside the driver contract")
    rising_target = curve_state(filtered_temperature_c)
    if rising_target > current:
        return min(rising_target, current + UP_STATES_PER_POLL)
    falling_target = curve_state(filtered_temperature_c + DOWN_HYSTERESIS_C)
    if falling_target < current:
        return max(falling_target, current - DOWN_STATES_PER_POLL)
    return current


def read_state(cooling_device: Path) -> int:
    state = int((cooling_device / "cur_state").read_text(encoding="ascii").strip())
    maximum = int((cooling_device / "max_state").read_text(encoding="ascii").strip())
    if maximum != MAX_STATE or not 0 <= state <= maximum:
        raise RuntimeError(
            f"unexpected cooling-device contract: state={state} max_state={maximum}"
        )
    return state


def write_state(cooling_device: Path, state: int) -> None:
    if not 0 <= state <= MAX_STATE:
        raise ValueError(f"state must be between 0 and {MAX_STATE}")
    (cooling_device / "cur_state").write_text(f"{state}\n", encoding="ascii")
    observed = read_state(cooling_device)
    if observed != state:
        raise RuntimeError(f"cooling-state readback mismatch: wrote {state}, read {observed}")


def run_daemon(thermal_root: Path, poll_seconds: float) -> int:
    if not math.isfinite(poll_seconds) or poll_seconds <= 0:
        raise ValueError("poll interval must be finite and positive")
    cooling_device = find_cooling_device(thermal_root)
    current = read_state(cooling_device)
    filtered_temperature_c: float | None = None
    stop_requested = False

    def request_stop(signum: int, frame: object) -> None:
        nonlocal stop_requested
        stop_requested = True

    signal.signal(signal.SIGTERM, request_stop)
    signal.signal(signal.SIGINT, request_stop)
    LOG.info("controller started at cooling state %d", current)

    try:
        while not stop_requested:
            try:
                hottest = read_hottest_temperature_c(thermal_root)
                if filtered_temperature_c is None:
                    filtered_temperature_c = hottest
                else:
                    filtered_temperature_c += FILTER_ALPHA * (
                        hottest - filtered_temperature_c
                    )
                desired = next_state(current, filtered_temperature_c)
            except Exception:
                LOG.exception("temperature observation failed; requesting maximum cooling")
                hottest = math.nan
                filtered_temperature_c = None
                desired = MAX_STATE

            if desired != current:
                write_state(cooling_device, desired)
                LOG.info(
                    "cooling state %d -> %d (hottest=%.1fC filtered=%s)",
                    current,
                    desired,
                    hottest,
                    "unavailable"
                    if filtered_temperature_c is None
                    else f"{filtered_temperature_c:.1f}C",
                )
                current = desired
            time.sleep(poll_seconds)
    finally:
        write_state(cooling_device, 0)
        LOG.info("automatic NVIDIA fan policy restored")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--thermal-root",
        type=Path,
        default=Path("/sys/class/thermal"),
        help=argparse.SUPPRESS,
    )
    subparsers = parser.add_subparsers(dest="command", required=True)
    daemon = subparsers.add_parser("daemon", help="run the smooth performance curve")
    daemon.add_argument("--poll-seconds", type=float, default=POLL_SECONDS)
    set_state = subparsers.add_parser("set-state", help="set one explicit cooling state")
    set_state.add_argument("state", type=int, choices=range(MAX_STATE + 1))
    subparsers.add_parser("automatic", help="restore firmware-automatic policy")
    subparsers.add_parser("status", help="show the cooling state and hottest sensor")
    return parser


def main(argv: list[str] | None = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(levelname)s %(message)s")
    args = build_parser().parse_args(argv)
    try:
        if args.command == "daemon":
            return run_daemon(args.thermal_root, args.poll_seconds)
        cooling_device = find_cooling_device(args.thermal_root)
        if args.command == "set-state":
            write_state(cooling_device, args.state)
            return 0
        if args.command == "automatic":
            write_state(cooling_device, 0)
            return 0
        if args.command == "status":
            print(
                f"state={read_state(cooling_device)}/{MAX_STATE} "
                f"hottest={read_hottest_temperature_c(args.thermal_root):.1f}C"
            )
            return 0
    except (OSError, RuntimeError, ValueError) as exc:
        LOG.error("%s", exc)
        return 1
    raise AssertionError(f"unhandled command: {args.command}")


if __name__ == "__main__":
    sys.exit(main())
