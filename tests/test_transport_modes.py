# SPDX-License-Identifier: GPL-2.0-only
"""Execute real module init/exit and census code against a fake driver core.

These tests establish local routing/ownership, not real firmware acceptance.
"""
import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

from test_kernel_transactions import SOURCE, c_function


SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
#define __init
#define __exit
#define CONFIG_ARM_FFA_TRANSPORT FFA_REACHABLE
#define IS_REACHABLE(config) (config)
#define DMI_SYS_VENDOR 1
#define DMI_PRODUCT_NAME 2
#define DMI_BOARD_NAME 3
static char logs[16384];
static void log_line(const char *format, ...) {
 va_list args; va_start(args, format);
 size_t n = strlen(logs);
 vsnprintf(logs + n, sizeof(logs) - n, format, args); va_end(args);
}
#define pr_info(...) log_line(__VA_ARGS__)
#define pr_warn(...) log_line(__VA_ARGS__)
#define pr_err(...) log_line(__VA_ARGS__)
#define dev_err(dev, ...) ((void)(dev), log_line(__VA_ARGS__))
typedef unsigned int uuid_t;
struct device_driver { const char *name; };
struct device { struct device_driver *driver; };
struct ffa_device { struct device dev; uuid_t uuid; };
struct ffa_driver { struct device_driver driver; };
#define to_ffa_dev(dev) ((struct ffa_device *)(dev))
static struct ffa_driver dgx_ec_fan_control_driver = {{"test-packet"}};
static const uuid_t dgx_ec_packet_uuid = 7;
static char *dgx_ec_transport;
static bool dgx_ec_ffa_registered;
static bool supported = true, bind_on_register = true, driver_live;
static int acpi_result, acpi_calls, census_calls, register_calls, unregister_calls;
static int metadata_calls, locks, register_error, peer_count = 1;
static struct ffa_device peer = {.uuid = 7};
static struct device_driver other_driver = {"other"};
int ffa_bus_type;
static bool dgx_ec_is_supported_platform(void) { return supported; }
static const char *dmi_get_system_info(int field) { (void)field; return "test"; }
static int dgx_ec_select_acpi(void) { acpi_calls++; return acpi_result; }
#if IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT)
static int uuid_equal(const uuid_t *a, const uuid_t *b) { return *a == *b; }
static void device_lock(struct device *dev) { assert(dev == &peer.dev && locks == 0); locks++; }
static void device_unlock(struct device *dev) { assert(dev == &peer.dev && locks == 1); locks--; }
static int dgx_ec_validate_transport(struct ffa_device *dev, const uuid_t *uuid) {
 assert(locks == 1 && dev == &peer && uuid == &dgx_ec_packet_uuid);
 metadata_calls++; return 0;
}
#endif
static int ffa_register(struct ffa_driver *driver) {
 assert(driver == &dgx_ec_fan_control_driver && !driver_live); register_calls++;
 if (register_error) return register_error;
 driver_live = true;
 if (bind_on_register) peer.dev.driver = &driver->driver;
 return 0;
}
static void ffa_unregister(struct ffa_driver *driver) {
 assert(driver == &dgx_ec_fan_control_driver && driver_live); unregister_calls++;
 driver_live = false; peer.dev.driver = NULL;
}
'''

CENSUS_SHIM = r'''
#if IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT)
static int bus_for_each_dev(int *bus, void *start, void *context,
                            int (*callback)(struct device *, void *)) {
 assert(bus == &ffa_bus_type && start == NULL);
 struct dgx_ec_discovery *census = context;
 /* Both scans must start fresh, rather than double-counting a service. */
 assert(census->matching == 0 && census->bound == 0);
 census_calls++;
 assert(census->probed == (register_calls != 0));
 if (peer_count) assert(callback(&peer.dev, context) == 0);
 assert(census->matching == (unsigned int)peer_count);
 assert(census->bound == (unsigned int)(peer_count && driver_live && bind_on_register));
 return 0;
}
#endif
'''

MAIN = r'''
int main(int argc, char **argv) {
 assert(argc == 2);
 const char *scenario = argv[1];
 dgx_ec_transport = "auto";
 int expected = 0;
 if (!strncmp(scenario, "acpi_", 5)) dgx_ec_transport = "acpi";
 if (!strncmp(scenario, "probe_", 6)) dgx_ec_transport = "probe";
 if (!strcmp(scenario, "acpi_present") || !strcmp(scenario, "auto_acpi")) acpi_result = 1;
 if (!strcmp(scenario, "acpi_positive")) acpi_result = 37;
 if (!strcmp(scenario, "acpi_absent")) expected = -ENODEV;
 if (!strcmp(scenario, "acpi_rejected") || !strcmp(scenario, "auto_acpi_rejected"))
  acpi_result = expected = -EIO;
 if (!strcmp(scenario, "probe_absent") || !strcmp(scenario, "auto_packet_absent")) peer_count = 0;
 if (!strcmp(scenario, "auto_packet_absent")) expected = -ENODEV;
 if (!strcmp(scenario, "probe_owned")) peer.dev.driver = &other_driver;
 if (!strcmp(scenario, "auto_unbound")) { bind_on_register = false; expected = -ENODEV; }
 if (!strcmp(scenario, "auto_register_error")) register_error = expected = -EBUSY;
 if (!strcmp(scenario, "invalid") || !strcmp(scenario, "removed_packet") || !strcmp(scenario, "empty")) {
  dgx_ec_transport = !strcmp(scenario, "invalid") ? "typo" :
                     !strcmp(scenario, "removed_packet") ? "packet" : "";
  expected = -EINVAL;
 }
 if (!strcmp(scenario, "unsupported_platform")) { supported = false; expected = -ENODEV; }
#if !IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT)
 if (!strncmp(scenario, "probe_", 6)) expected = -EOPNOTSUPP;
 if (!strncmp(scenario, "auto_", 5) && acpi_result == 0) expected = -ENODEV;
#endif
 int result = dgx_ec_fan_control_init();
 assert(result == expected && result <= 0 && locks == 0);
 if (!strcmp(dgx_ec_transport, "probe")) {
  assert(acpi_calls == 0 && register_calls == 0 && unregister_calls == 0);
  assert(!driver_live && !dgx_ec_ffa_registered);
#if IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT)
  assert(census_calls == 1 && metadata_calls == peer_count);
  assert(strstr(logs, "discovery only:") && strstr(logs, "nothing registered, nothing written"));
  if (peer_count) assert(strstr(logs, "not attempted: enumeration only; no packet driver registered"));
  else assert(strstr(logs, "0 packet service(s)"));
#else
  assert(census_calls == 0 && metadata_calls == 0);
#endif
 } else if (!strcmp(dgx_ec_transport, "acpi") || acpi_result != 0) {
  assert(acpi_calls == 1 && census_calls == 0 && register_calls == 0);
  assert(!driver_live && !dgx_ec_ffa_registered);
 } else if (expected == -EINVAL || !supported) {
  assert(acpi_calls == 0 && census_calls == 0 && register_calls == 0);
  assert(!driver_live && !dgx_ec_ffa_registered);
 } else {
  assert(acpi_calls == 1);
#if IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT)
  assert(census_calls == (peer_count && !register_error ? 2 : 1));
  assert(register_calls == peer_count);
  assert(unregister_calls == (peer_count && !register_error && !bind_on_register));
  if (peer_count && !register_error && !bind_on_register)
   assert(strstr(logs, "1 matching service(s), none bound"));
  assert(dgx_ec_ffa_registered == (peer_count && !register_error && bind_on_register));
#else
  assert(census_calls == 0 && register_calls == 0 && !dgx_ec_ffa_registered);
#endif
 }
 int before_exit = unregister_calls;
 bool registered = dgx_ec_ffa_registered;
 dgx_ec_fan_control_exit();
 assert(unregister_calls == before_exit + registered && !driver_live && locks == 0);
 return 0;
}
'''


class TransportModeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="dgx-mode-tests-")
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        discovery = re.search(r"struct dgx_ec_discovery \{.*?\n\};", SOURCE, re.S)[0]
        source = root / "modes.c"
        source.write_text(
            SHIM + discovery + CENSUS_SHIM +
            "\n#if IS_REACHABLE(CONFIG_ARM_FFA_TRANSPORT)\n" +
            c_function("dgx_ec_report_ffa_candidate") + "\n#endif\n" +
            c_function("dgx_ec_fan_control_init") + c_function("dgx_ec_fan_control_exit") + MAIN
        )
        cls.binaries = {}
        for reachable in (0, 1):
            binary = root / f"modes-{reachable}"
            result = subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c11", "-Wall", "-Wextra", "-Werror",
                f"-DFFA_REACHABLE={reachable}", str(source), "-o", str(binary),
            ], capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stderr)
            cls.binaries[reachable] = binary

    def run_scenarios(self, scenarios, reachable=1):
        for scenario in scenarios:
            with self.subTest(scenario=scenario, ffa_reachable=reachable):
                result = subprocess.run([str(self.binaries[reachable]), scenario],
                                        capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_forced_acpi_normalizes_selection_and_refuses_absence(self):
        self.run_scenarios(("acpi_present", "acpi_positive", "acpi_absent", "acpi_rejected"))

    def test_probe_only_enumerates_even_when_another_driver_owns_service(self):
        self.run_scenarios(("probe_present", "probe_absent", "probe_owned"))

    def test_auto_fallback_resets_census_and_unwinds_registration(self):
        self.run_scenarios(("auto_acpi", "auto_acpi_rejected", "auto_packet", "auto_packet_absent",
                            "auto_unbound", "auto_register_error"))

    def test_invalid_parameter_and_platform_exit_without_acquiring_resources(self):
        self.run_scenarios(("invalid", "removed_packet", "empty", "unsupported_platform"))

    def test_modes_without_reachable_ffa_transport(self):
        self.run_scenarios(("probe_present", "acpi_present", "acpi_absent", "auto_packet"), reachable=0)
