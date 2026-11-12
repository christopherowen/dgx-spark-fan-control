# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the driver's real discovery/ACPI/FF-A helpers without firmware.

These peers check local contracts, not real FF-A binding or AML execution.
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
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdarg.h>
typedef uint8_t u8;
typedef uint32_t u32;
typedef unsigned long long u64;
typedef unsigned int acpi_status;
typedef unsigned int uuid_t;
#define BIT(n) (1U << (n))
#define FFA_VERSION_1_1 0x10001U
#define FFA_VERSION_1_2 0x10002U
#define FFA_MAJOR_VERSION(v) (((v) >> 16) & 0x7fff)
#define FFA_MINOR_VERSION(v) ((v) & 0xffff)
#define DGX_EC_PACKET_POLL_COMMAND 2
#define DGX_EC_PACKET_COMPLETE 0
#define ACPI_ALLOCATE_BUFFER (~0UL)
#define ACPI_TYPE_INTEGER 1
#define ACPI_TYPE_PACKAGE 4
#define CONFIG_ACPI 1
#define IS_ENABLED(x) (x)
#define ACPI_FAILURE(x) ((x) != 0)
#define ACPI_SUCCESS(x) ((x) == 0)
static char logs[16384];
static void log_line(const char *fmt, ...) {
 va_list args; va_start(args, fmt);
 size_t n = strlen(logs);
 vsnprintf(logs + n, sizeof(logs) - n, fmt, args); va_end(args);
}
#define dev_info(dev, ...) ((void)(dev), log_line(__VA_ARGS__))
#define dev_err(dev, ...) ((void)(dev), log_line(__VA_ARGS__))
#define pr_info(...) log_line(__VA_ARGS__)
#define pr_warn(...) log_line(__VA_ARGS__)
struct driver { const char *name; };
struct device { struct driver *driver; };
struct ffa_device;
struct ffa_send_direct_data { unsigned long data0, data1, data2, data3, data4; };
struct ffa_send_direct_data2 { unsigned long data[14]; };
struct msg_ops {
 int (*sync_send_receive)(struct ffa_device *, struct ffa_send_direct_data *);
 int (*sync_send_receive2)(struct ffa_device *, struct ffa_send_direct_data2 *);
};
struct info_ops { u32 (*api_version_get)(void); };
struct ffa_ops { struct msg_ops *msg_ops; struct info_ops *info_ops; };
struct ffa_device { struct device dev; struct ffa_ops *ops; uuid_t uuid; u32 vm_id, properties; bool mode_32bit; };
static int uuid_equal(const uuid_t *a, const uuid_t *b) { return *a == *b; }
static u32 version = FFA_VERSION_1_2;
static u32 get_version(void) { return version; }
static int legacy_calls, v2_calls, exchange_error;
static int legacy(struct ffa_device *dev, struct ffa_send_direct_data *msg) {
 (void)dev; legacy_calls++; memset(msg, 0x5a, sizeof(*msg)); return exchange_error;
}
static int v2(struct ffa_device *dev, struct ffa_send_direct_data2 *msg) {
 (void)dev; v2_calls++; memset(msg, 0x7c, sizeof(*msg)); return exchange_error;
}
union acpi_object {
 unsigned int type;
 struct { unsigned int type; u64 value; } integer;
 struct { unsigned int type, count; union acpi_object *elements; } package;
};
struct acpi_buffer { unsigned long length; void *pointer; };
struct acpi_device { struct device dev; void *handle; };
static struct acpi_device fans[2];
static int fan_count, native_refs, locked, frees, method_missing, eval_error;
static int no_native, no_mapping, evaluated;
static struct driver native_driver = {"acpi-fan"};
static struct device native_device = {&native_driver};
static int platform_bus_type;
static unsigned int fif_type = ACPI_TYPE_PACKAGE, fif_count = 4;
static u64 fif_revision, fif_fine = 1, fif_step = 1;
static int acpi_has_method(void *handle, const char *name) {
 (void)handle; assert(strcmp(name, "WFCF")); return !method_missing;
}
static acpi_status acpi_evaluate_object(void *handle, const char *name, void *args, struct acpi_buffer *buf) {
 (void)handle; (void)args; assert(!strcmp(name, "_FIF")); evaluated++;
 if (eval_error) return 1;
 union acpi_object *obj = calloc(5, sizeof(*obj)); assert(obj);
 obj->package.type = fif_type; obj->package.count = fif_count; obj->package.elements = obj + 1;
 for (int i = 0; i < 4; i++) obj[i + 1].integer.type = ACPI_TYPE_INTEGER;
 obj[1].integer.value = fif_revision; obj[2].integer.value = fif_fine; obj[3].integer.value = fif_step;
 buf->pointer = obj; return 0;
}
static acpi_status acpi_evaluate_integer(void *h, const char *name, void *args, unsigned long long *v) {
 (void)h; (void)args; assert(!strcmp(name, "FMIN") || !strcmp(name, "FMAX"));
 *v = !strcmp(name, "FMIN") ? 975 : 4350; return no_mapping;
}
static const char *acpi_format_exception(acpi_status status) { (void)status; return "AE_TEST"; }
static void kfree(void *ptr) { if (ptr) frees++; free(ptr); }
static struct device *bus_find_device_by_acpi_dev(int *bus, struct acpi_device *adev) {
 assert(bus == &platform_bus_type); (void)adev;
 if (no_native) return NULL;
 native_refs++; return &native_device;
}
static void device_lock(struct device *dev) { assert(dev == &native_device && !locked); locked++; }
static void device_unlock(struct device *dev) { assert(dev == &native_device && locked); locked--; }
static int device_is_bound(struct device *dev) { return dev->driver != NULL; }
static void put_device(struct device *dev) { assert(dev == &native_device); native_refs--; }
#define for_each_acpi_dev_match(adev, hid, uid, hrv) \
 for (int index = 0; index < fan_count && ((adev) = &fans[index]); index++)
static const uuid_t dgx_ec_packet_uuid = 7;
'''

MAIN = r'''
int main(int argc, char **argv) {
 assert(argc == 2);
 struct msg_ops msg = {.sync_send_receive = legacy, .sync_send_receive2 = v2};
 struct info_ops info = {.api_version_get = get_version};
 struct ffa_ops ops = {.msg_ops = &msg, .info_ops = &info};
 struct ffa_device dev = {.ops = &ops, .uuid = 7, .vm_id = 0x9234, .properties = 0xfedc};
 struct ffa_send_direct_data2 message;
 if (!strcmp(argv[1], "versions")) {
  u32 versions[] = {0x10001, 0x10002, 0x10003, 0x20000};
  for (unsigned int i = 0; i < sizeof(versions)/sizeof(*versions); i++) {
   version = versions[i]; assert(dgx_ec_validate_transport(&dev, &dgx_ec_packet_uuid) == 0);
   memset(&message, 0x11, sizeof(message));
   assert(dgx_ec_exchange(&dev, &message) == 0);
   unsigned char *p = (void *)&message;
   for (unsigned int j = 0; j < sizeof(message); j++)
    assert(p[j] == (i ? 0x7c : j < sizeof(struct ffa_send_direct_data) ? 0x5a : 0x11));
  }
  assert(legacy_calls == 1 && v2_calls == 3);
  u32 invalid[] = {0, 0x10000, 0xffff, 0xffffffff, 0x80010002};
  for (unsigned int i = 0; i < sizeof(invalid)/sizeof(*invalid); i++) {
   version = invalid[i]; assert(dgx_ec_validate_transport(&dev, &dgx_ec_packet_uuid) == -EOPNOTSUPP);
  }
  assert(strstr(logs, "UUID=") && strstr(logs, "version=") && strstr(logs, "vm_id=0x9234 properties=0xfedc"));
  assert(strstr(logs, "requires >= 1.1"));
 } else if (!strcmp(argv[1], "missing_operations")) {
  info.api_version_get = NULL; assert(dgx_ec_validate_transport(&dev, &dgx_ec_packet_uuid) < 0);
  assert(strstr(logs, "version=unavailable/")); info.api_version_get = get_version;
  ops.msg_ops = NULL; assert(dgx_ec_validate_transport(&dev, &dgx_ec_packet_uuid) < 0); ops.msg_ops = &msg;
  msg.sync_send_receive2 = NULL; assert(dgx_ec_validate_transport(&dev, &dgx_ec_packet_uuid) < 0);
  version = FFA_VERSION_1_1; assert(dgx_ec_validate_transport(&dev, &dgx_ec_packet_uuid) == 0);
  msg.sync_send_receive = NULL; assert(dgx_ec_validate_transport(&dev, &dgx_ec_packet_uuid) < 0);
  assert(strstr(logs, "missing sync_send_receive2") && strstr(logs, "missing sync_send_receive"));
 } else if (!strcmp(argv[1], "identity_and_mode")) {
  dev.uuid = 8; assert(dgx_ec_validate_transport(&dev, &dgx_ec_packet_uuid) < 0);
  dev.uuid = 7; dev.mode_32bit = true; assert(dgx_ec_validate_transport(&dev, &dgx_ec_packet_uuid) < 0);
  dev.mode_32bit = false; assert(dgx_ec_validate_transport(&dev, &dgx_ec_packet_uuid) == 0);
 } else if (!strcmp(argv[1], "no_downgrade_and_bounds")) {
  exchange_error = -EIO; assert(dgx_ec_exchange(&dev, &message) == -EIO);
  assert(v2_calls == 1 && legacy_calls == 0);
  u8 state = 0, output[112];
  assert(dgx_ec_packet_poll(&dev, &state, output, sizeof(output)) == -EMSGSIZE);
  version = FFA_VERSION_1_1;
  assert(dgx_ec_packet_poll(&dev, &state, output, 40) == -EMSGSIZE);
  assert(v2_calls == 1 && legacy_calls == 0);
 } else if (!strncmp(argv[1], "acpi_", 5)) {
  int expected = 0;
  if (!strcmp(argv[1], "acpi_bad_type")) { fif_type = ACPI_TYPE_INTEGER; expected = -EINVAL; }
  if (!strcmp(argv[1], "acpi_bad_count")) { fif_count = 3; expected = -EINVAL; }
  if (!strcmp(argv[1], "acpi_bad_revision")) { fif_revision = 1; expected = -EINVAL; }
  if (!strcmp(argv[1], "acpi_not_fine")) { fif_fine = 0; expected = -EINVAL; }
  if (!strcmp(argv[1], "acpi_zero_step")) { fif_step = 0; expected = -EINVAL; }
  if (!strcmp(argv[1], "acpi_huge_step")) { fif_step = ~0ULL; expected = -EINVAL; }
  if (!strcmp(argv[1], "acpi_missing_method")) { method_missing = 1; expected = -EOPNOTSUPP; }
  if (!strcmp(argv[1], "acpi_eval_error")) { eval_error = 1; expected = -EIO; }
  if (!strcmp(argv[1], "acpi_unbound")) { native_device.driver = NULL; expected = -ENODEV; }
  if (!strcmp(argv[1], "acpi_wrong_owner")) { native_driver.name = "other"; expected = -ENODEV; }
  if (!strcmp(argv[1], "acpi_no_native")) { no_native = 1; expected = -ENODEV; }
  if (!strcmp(argv[1], "acpi_no_mapping")) no_mapping = 1;
  if (!strcmp(argv[1], "acpi_absent")) {
   assert(dgx_ec_select_acpi() == 0); assert(evaluated == 0);
  } else {
   fan_count = 2;
   assert(dgx_ec_select_acpi() == (expected ? expected : 1));
   assert(frees == evaluated - (eval_error ? evaluated : 0));
   assert(strstr(logs, "FF-A packet candidate UUID=") && strstr(logs, "not attempted"));
   assert(strstr(logs, "level 0 is minimum, NOT automatic"));
   if (expected) assert(strstr(logs, "ACPI candidate rejected:"));
   else assert(strstr(logs, "transport=ACPI") && strstr(logs, "no EC-RPM conversion"));
  }
  assert(native_refs == 0 && locked == 0 && legacy_calls == 0 && v2_calls == 0);
 } else assert(!"unknown case");
 return 0;
}
'''


class FirmwareTransportTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="dgx-transport-tests-")
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        names = ("dgx_ec_validate_transport", "dgx_ec_exchange", "dgx_ec_packet_poll",
                 "dgx_ec_acpi_candidate", "dgx_ec_select_acpi")
        source = root / "transport.c"
        source.write_text(SHIM + "\n".join(c_function(name) for name in names) + MAIN)
        cls.binary = root / "transport"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(cls.binary),
        ], check=True, capture_output=True, text=True)

    def test_reported_versions_and_discovered_endpoints(self):
        for case in ("versions", "missing_operations", "identity_and_mode", "no_downgrade_and_bounds"):
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary), case], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_native_acpi_selection_never_writes_or_leaks(self):
        for case in ("acpi_absent", "acpi_selected", "acpi_bad_type", "acpi_bad_count",
                     "acpi_bad_revision", "acpi_not_fine", "acpi_zero_step", "acpi_huge_step",
                     "acpi_missing_method", "acpi_eval_error", "acpi_unbound", "acpi_wrong_owner",
                     "acpi_no_native", "acpi_no_mapping"):
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary), case], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_lifecycle_unwind_stops_writers_before_restoring(self):
        probe = c_function("dgx_ec_fan_control_probe")
        unwind = probe.split("unregister:", 1)[1].split("unlock:", 1)[0]
        self.assertLess(unwind.index("thermal_cooling_device_unregister"),
                        unwind.index("dgx_ec_restore_automatic"))
        self.assertIn("data->cooling_dev = NULL", unwind)
        self.assertIn('stage = "reboot-notifier registration";\n\t\tgoto unregister;', probe)
        remove = c_function("dgx_ec_fan_control_remove")
        self.assertLess(remove.index("thermal_cooling_device_unregister"),
                        remove.index("dgx_ec_restore_automatic"))

    def test_init_selects_acpi_before_registering_packet_driver(self):
        init = SOURCE.split("static int __init dgx_ec_fan_control_init(void)", 1)[1]
        init = init.split("static void __exit", 1)[0]
        self.assertLess(init.index("dgx_ec_select_acpi()"), init.index("ffa_register("))
        self.assertRegex(init, r"if \(ret > 0\) \{[^}]*return 0;\s*\}")
        self.assertIn("if (!discovery.bound)", init)
        self.assertIn("dgx_ec_report_ffa_candidate", init)
        self.assertNotRegex(SOURCE, r"(?:vm_id|properties)\s*!=")
        self.assertNotRegex(SOURCE, r"version\s*(?:==|!=)\s*FFA_VERSION_1_[12]")
