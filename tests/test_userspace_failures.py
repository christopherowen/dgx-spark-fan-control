# SPDX-License-Identifier: GPL-2.0-only
"""Failure-path checks without touching the host's thermal interfaces."""

import math
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from test_fan_control_contract import CONTROL


class UserlandFailureTests(unittest.TestCase):
    def test_invalid_temperatures_are_rejected(self):
        for temperature in (0, -1, math.nan, math.inf, -math.inf):
            with self.subTest(temperature=temperature):
                with self.assertRaises(ValueError):
                    CONTROL.curve_state(temperature)

    def test_missing_and_duplicate_devices_are_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(RuntimeError):
                CONTROL.find_cooling_device(root)
            for index in range(2):
                device = root / f"cooling_device{index}"
                device.mkdir()
                (device / "type").write_text(CONTROL.COOLING_DEVICE_TYPE)
            with self.assertRaises(RuntimeError):
                CONTROL.find_cooling_device(root)

    def test_bad_sensor_values_do_not_hide_valid_hottest_sensor(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for index, value in enumerate(("bad", "0", "200001", "61000", "72000")):
                zone = root / f"thermal_zone{index}"
                zone.mkdir()
                (zone / "temp").write_text(value)
            self.assertEqual(CONTROL.read_hottest_temperature_c(root), 72)
            for path in root.glob("thermal_zone*/temp"):
                path.write_text("bad")
            with self.assertRaises(RuntimeError):
                CONTROL.read_hottest_temperature_c(root)

    def test_write_detects_readback_mismatch(self):
        with tempfile.TemporaryDirectory() as directory:
            device = Path(directory)
            with patch.object(CONTROL, "read_state", return_value=0):
                with self.assertRaisesRegex(RuntimeError, "readback mismatch"):
                    CONTROL.write_state(device, 3)

    def test_sensor_loss_requests_maximum_then_signal_restores_automatic(self):
        # Deliver a real daemon stop callback at its first sleep boundary.
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            device = root / "cooling_device0"
            device.mkdir()
            for name, value in (("type", CONTROL.COOLING_DEVICE_TYPE),
                                ("cur_state", "0"), ("max_state", "12")):
                (device / name).write_text(value)
            handlers = {}
            observed = []

            def register(signum, callback):
                handlers[signum] = callback

            def interrupt_sleep(seconds):
                observed.append(CONTROL.read_state(device))
                handlers[CONTROL.signal.SIGTERM](CONTROL.signal.SIGTERM, None)

            with patch.object(CONTROL.signal, "signal", side_effect=register), \
                 patch.object(CONTROL.time, "sleep", side_effect=interrupt_sleep), \
                 self.assertLogs(CONTROL.LOG, level="ERROR"):
                self.assertEqual(CONTROL.run_daemon(root, 2), 0)
            self.assertEqual(observed, [12])
            self.assertEqual(CONTROL.read_state(device), 0)


if __name__ == "__main__":
    unittest.main()
