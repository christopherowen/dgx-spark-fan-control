import importlib.util
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "kernel" / "dgx_ec_fan_control.c").read_text()
MODULE_PATH = ROOT / "userspace" / "dgx_fan_control.py"
SERVICE = (ROOT / "systemd" / "dgx-fan-control.service").read_text()
SPEC = importlib.util.spec_from_file_location("dgx_fan_control", MODULE_PATH)
assert SPEC is not None and SPEC.loader is not None
CONTROL = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(CONTROL)


class KernelContractTests(unittest.TestCase):
    def test_only_additive_lower_floor_is_writable(self):
        self.assertIn("DGX_EC_FAN_SET_LOWER_LIMIT", SOURCE)
        self.assertNotIn("SET_UPPER", SOURCE)
        self.assertNotIn("hwmon_fan_target", SOURCE)
        self.assertNotIn("hwmon_pwm", SOURCE)
        self.assertNotIn("debugfs", SOURCE)
        self.assertNotIn("ioctl", SOURCE)

    def test_state_zero_is_automatic_and_states_are_bounded(self):
        table = SOURCE.split("dgx_ec_floor_states[] = {", 1)[1].split("};", 1)[0]
        self.assertIn("DGX_EC_LIMIT_UNSET", table)
        for rpm in ("2700U", "4500U", "9000U", "13500U"):
            self.assertIn(rpm, table)
        self.assertIn("state >= ARRAY_SIZE(dgx_ec_floor_states)", SOURCE)

    def test_platform_and_capabilities_fail_closed(self):
        for requirement in (
            'DMI_SYS_VENDOR, "NVIDIA"',
            'DMI_PRODUCT_NAME, "NVIDIA_DGX_Spark"',
            'DMI_BOARD_NAME, "P4242"',
            "FFA_VERSION_1_2",
            "DGX_EC_EXPECTED_FAN0_MIN_RPM",
            "DGX_EC_EXPECTED_FAN1_MAX_RPM",
            "if (floor != DGX_EC_LIMIT_UNSET)",
        ):
            self.assertIn(requirement, SOURCE)

    def test_orderly_lifecycle_restores_automatic_policy(self):
        self.assertIn("dgx_ec_restore_automatic(data, \"suspend\")", SOURCE)
        self.assertIn("dgx_ec_restore_automatic(data, \"reboot\")", SOURCE)
        self.assertIn("dgx_ec_restore_automatic(data, \"module removal\")", SOURCE)
        self.assertIn("DGX_EC_RESTORE_ATTEMPTS", SOURCE)
        self.assertIn("thermal_cooling_device_register", SOURCE)


class UserlandPolicyTests(unittest.TestCase):
    def test_performance_curve_reaches_maximum_at_seventy(self):
        self.assertEqual(CONTROL.curve_state(49.9), 0)
        self.assertEqual(CONTROL.curve_state(50.0), 3)
        self.assertEqual(CONTROL.curve_state(60.0), 8)
        self.assertEqual(CONTROL.curve_state(70.0), 12)

    def test_rate_limit_rises_quickly_and_falls_with_hysteresis(self):
        self.assertEqual(CONTROL.next_state(0, 70.0), 2)
        self.assertEqual(CONTROL.next_state(10, 70.0), 12)
        self.assertEqual(CONTROL.next_state(12, 67.0), 12)
        self.assertEqual(CONTROL.next_state(12, 65.9), 11)

    def test_finds_exactly_one_device_and_authenticates_state(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            zone = root / "thermal_zone0"
            zone.mkdir()
            (zone / "temp").write_text("65000\n", encoding="ascii")
            cooling = root / "cooling_device0"
            cooling.mkdir()
            (cooling / "type").write_text("dgx_ec_fan_floor\n", encoding="ascii")
            (cooling / "max_state").write_text("12\n", encoding="ascii")
            (cooling / "cur_state").write_text("0\n", encoding="ascii")

            self.assertEqual(CONTROL.find_cooling_device(root), cooling)
            self.assertEqual(CONTROL.read_hottest_temperature_c(root), 65.0)
            CONTROL.write_state(cooling, 8)
            self.assertEqual(CONTROL.read_state(cooling), 8)

    def test_service_keeps_root_write_but_drops_ambient_authority(self):
        self.assertNotIn("User=", SERVICE)
        self.assertIn("CapabilityBoundingSet=\n", SERVICE)
        self.assertIn("NoNewPrivileges=yes", SERVICE)
        self.assertIn("PrivateNetwork=yes", SERVICE)
        self.assertIn("ProtectSystem=strict", SERVICE)
        self.assertNotIn("modprobe", SERVICE)


if __name__ == "__main__":
    unittest.main()
