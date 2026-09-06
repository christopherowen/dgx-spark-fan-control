# SPDX-License-Identifier: GPL-2.0-only
"""Execute the driver's actual C transaction functions against a fake FF-A peer.

The existing source assertions cannot exercise acknowledgement loss or delayed
completion. Only Linux wrappers and the firmware boundary are replaced here;
transaction and recovery functions are extracted verbatim from the driver.
"""
import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "kernel/dgx_ec_fan_control.c").read_text()


def c_function(name):
    start = SOURCE.index(f"static int {name}(")
    end = SOURCE.index("{", start) + 1
    depth = 1
    while depth:
        depth += (SOURCE[end] == "{") - (SOURCE[end] == "}")
        end += 1
    return SOURCE[start:end]


SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define U16_MAX UINT16_MAX
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define dev_info(...) ((void)0)
#define dev_emerg(dev, ...) ((void)snprintf(log_text, sizeof(log_text), __VA_ARGS__))
#define dev_warn_ratelimited(dev, ...) ((void)snprintf(log_text, sizeof(log_text), __VA_ARGS__))
static char log_text[256];
struct mutex { int unused; };
struct notifier_block { int unused; };
struct ffa_device;
struct ffa_send_direct_data2 { unsigned long data[14]; };
struct msg_ops { int (*sync_send_receive2)(struct ffa_device *, struct ffa_send_direct_data2 *); };
struct ffa_ops { struct msg_ops *msg_ops; };
struct ffa_device { int dev; struct ffa_ops *ops; };
struct thermal_cooling_device { struct dgx_ec_fan_control_data *devdata; };
static int mutex_lock_interruptible(struct mutex *m) { (void)m; return 0; }
static void mutex_lock(struct mutex *m) { (void)m; }
static void mutex_unlock(struct mutex *m) { (void)m; }
static void msleep(unsigned int ms) { (void)ms; }
static u16 get_unaligned_le16(const void *ptr) {
 const u8 *p = ptr; return p[0] | ((u16)p[1] << 8);
}
static u32 get_unaligned_le32(const void *ptr) {
 const u8 *p = ptr; return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void put_unaligned_le16(u16 value, void *ptr) {
 u8 *p = ptr; p[0] = value; p[1] = value >> 8;
}
static u16 physical_floor, reply_floor;
static int pending_polls, stuck, submit_count, get_count, write_count;
static int write_timeout_once, readback_timeout_once, auto_timeout_once;
static int reject_auto_count, transport_error_once, foreign_after_write;
static int poll_state_override = -1;
static int exchange(struct ffa_device *dev, struct ffa_send_direct_data2 *message) {
 (void)dev;
 u8 *raw = (u8 *)message->data;
 if (transport_error_once) { int ret = transport_error_once; transport_error_once = 0; return ret; }
 if (raw[0] == 2) {
  memset(raw, 0, sizeof(*message));
  if (poll_state_override >= 0) { raw[0] = poll_state_override; return 0; }
  if (stuck || pending_polls > 0) { if (pending_polls > 0) pending_polls--; raw[0] = 2; return 0; }
  put_unaligned_le16(reply_floor, raw + 1);
  return 0;
 }
 assert(raw[0] == 1);
 submit_count++;
 if (raw[1] == 4) {
  assert(raw[2] == 0 && raw[3] == 2);
  get_count++;
  reply_floor = physical_floor;
  if (readback_timeout_once && write_count) { readback_timeout_once = 0; pending_polls = 100; }
 } else if (raw[1] == 5) {
  assert(raw[2] == 2 && raw[3] == 0);
  u16 value = get_unaligned_le16(raw + 4);
  if (value == UINT16_MAX && reject_auto_count > 0) {
   reject_auto_count--; memset(raw, 0, sizeof(*message)); raw[0] = 0x0a; return 0;
  }
  physical_floor = foreign_after_write ? 7777 : value;
  write_count++;
  if (value != UINT16_MAX && write_timeout_once) { write_timeout_once = 0; pending_polls = 100; }
  if (value == UINT16_MAX && auto_timeout_once) { auto_timeout_once = 0; pending_polls = 100; }
 } else { assert(!"unexpected operation"); }
 memset(raw, 0, sizeof(*message));
 return 0;
}
'''

MAIN = r'''
int main(int argc, char **argv) {
 assert(argc == 2);
 struct msg_ops msg = {.sync_send_receive2 = exchange};
 struct ffa_ops ops = {.msg_ops = &msg};
 struct ffa_device ffa = {.ops = &ops};
 struct dgx_ec_fan_control_data data = {.ffa_dev = &ffa, .current_state = 3};
 struct thermal_cooling_device cdev = {.devdata = &data};
 unsigned long state = 999;
 physical_floor = reply_floor = 4500;
 if (!strcmp(argv[1], "write_timeout") || !strcmp(argv[1], "readback_timeout")) {
  write_timeout_once = !strcmp(argv[1], "write_timeout");
  readback_timeout_once = !strcmp(argv[1], "readback_timeout");
  assert(dgx_ec_set_cur_state(&cdev, 5) == -ETIMEDOUT);
  assert(physical_floor == UINT16_MAX && data.current_state == 0);
  assert(!data.floor_uncertain && write_count == 2);
  assert(dgx_ec_get_cur_state(&cdev, &state) == 0 && state == 0);
  assert(dgx_ec_set_cur_state(&cdev, 5) == 0 && physical_floor == 6300);
 } else if (!strcmp(argv[1], "uncertain_recovery")) {
  write_timeout_once = 1;
  assert(dgx_ec_write_lower_floor(&data, 6300) == -ETIMEDOUT);
  stuck = 1;
  int submitted = submit_count;
  assert(dgx_ec_get_cur_state(&cdev, &state) == -ETIMEDOUT);
  assert(data.floor_uncertain && data.attempted_floor == 6300 && data.current_state == 3);
  assert(submit_count == submitted);
  stuck = 0;
  assert(dgx_ec_get_cur_state(&cdev, &state) == 0 && state == 5);
  assert(!data.floor_uncertain);
  assert(dgx_ec_set_cur_state(&cdev, 0) == 0 && physical_floor == UINT16_MAX);
 } else if (!strcmp(argv[1], "foreign_floor")) {
  physical_floor = 7777;
  data.floor_uncertain = true; data.attempted_floor = 6300;
  assert(dgx_ec_get_cur_state(&cdev, &state) == -ESTALE);
  assert(dgx_ec_set_cur_state(&cdev, 0) == -ESTALE);
  assert(dgx_ec_restore_automatic(&data, "test") == -ESTALE);
  assert(write_count == 0 && physical_floor == 7777);
 } else if (!strcmp(argv[1], "foreign_during_write")) {
  foreign_after_write = 1;
  assert(dgx_ec_set_cur_state(&cdev, 5) == -EIO);
  assert(write_count == 1 && physical_floor == 7777);
 } else if (!strcmp(argv[1], "automatic_recovery")) {
  auto_timeout_once = 1;
  assert(dgx_ec_set_cur_state(&cdev, 0) == -ETIMEDOUT);
  assert(data.floor_uncertain && physical_floor == UINT16_MAX);
  assert(dgx_ec_get_cur_state(&cdev, &state) == 0 && state == 0);
  assert(dgx_ec_set_cur_state(&cdev, 0) == 0);
 } else if (!strcmp(argv[1], "late_completion")) {
  pending_polls = 3;
  assert(dgx_ec_get_cur_state(&cdev, &state) == 0 && state == 3);
  assert(submit_count == 1);
 } else if (!strcmp(argv[1], "preflight_stuck")) {
  stuck = 1;
  assert(dgx_ec_write_lower_floor(&data, 6300) == -ETIMEDOUT);
  assert(submit_count == 0 && !data.floor_uncertain && write_count == 0);
 } else if (!strcmp(argv[1], "bounded_restore")) {
  reject_auto_count = 2;
  assert(dgx_ec_restore_automatic(&data, "test") == 0);
  assert(reject_auto_count == 0 && write_count == 1 && physical_floor == UINT16_MAX);
 } else if (!strcmp(argv[1], "unexpected_responses")) {
  for (int value = 1; value <= 3; value += 2) {
   poll_state_override = value;
   assert(dgx_ec_set_cur_state(&cdev, 5) == (value == 1 ? -EREMOTEIO : -EBADMSG));
  }
  poll_state_override = -1; transport_error_once = -EAGAIN;
  assert(dgx_ec_set_cur_state(&cdev, 5) == -EAGAIN);
  assert(submit_count == 0 && write_count == 0);
 } else { assert(!"unknown case"); }
 return 0;
}
'''


class KernelTransactionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="dgx-fan-c-tests-")
        cls.addClassCleanup(cls.temp.cleanup)
        root = Path(cls.temp.name)
        defines = "\n".join(re.findall(r"^#define DGX_EC_.*$", SOURCE, re.M))
        table = re.search(r"static const u16 dgx_ec_floor_states\[\] = \{.*?\n\};", SOURCE, re.S)[0]
        data = re.search(r"struct dgx_ec_fan_control_data \{.*?\n\};", SOURCE, re.S)[0]
        names = (
            "dgx_ec_packet_poll", "dgx_ec_submit_status", "dgx_ec_wait_for_completion",
            "dgx_ec_preflight", "dgx_ec_read_operation", "dgx_ec_write_lower_floor",
            "dgx_ec_read_lower_floor", "dgx_ec_reconcile_floor",
            "dgx_ec_restore_automatic_locked", "dgx_ec_restore_automatic",
            "dgx_ec_get_cur_state", "dgx_ec_set_cur_state",
        )
        source = root / "transactions.c"
        source.write_text(SHIM + defines + "\n" + table + "\n" + data + "\n" +
                          "\n".join(c_function(name) for name in names) + MAIN)
        cls.binary = root / "transactions"
        subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
            "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(cls.binary),
        ], check=True, capture_output=True, text=True)

    def test_transaction_failure_and_recovery_scenarios(self):
        for case in (
            "write_timeout", "readback_timeout", "uncertain_recovery", "foreign_floor",
            "foreign_during_write", "automatic_recovery", "late_completion",
            "preflight_stuck", "bounded_restore", "unexpected_responses",
        ):
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary), case], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
