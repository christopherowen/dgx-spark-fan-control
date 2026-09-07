# SPDX-License-Identifier: GPL-2.0-only
"""Exercise the actual one-shot diagnostic against a strict fake secure peer."""
import os
import re
import shlex
import subprocess
import tempfile
import unittest
from pathlib import Path

SOURCE = (Path(__file__).resolve().parents[1] /
          "research/mailbox/dgx_ec_mailbox_probe.c").read_text()

SHIM = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
#define pr_info(...) ((void)0)
struct ffa_device;
struct ffa_send_direct_data2 { unsigned long data[14]; };
struct msg_ops { int (*sync_send_receive2)(struct ffa_device *, struct ffa_send_direct_data2 *); };
struct ffa_ops { struct msg_ops *msg_ops; };
struct ffa_device { struct ffa_ops *ops; int oem; };
static bool recover;
static unsigned int restore_floor = 0xffff;
static int submits, polls, status_reads, exchanges, fail_at, submit_status;
static int writes, stale_physical, stale_cache, refuse_unset, restore_test;
static u16 physical_floor = 12375;
static int pending_after_submit, completion_state, state_before = 2, state_after = 2;
static u8 status1 = 8, status2 = 8, family = 0x10, operation = 4;
static u8 clock_bytes[] = {0x06, 0x16, 0x09, 0x07, 0x09, 0x26};
static void msleep(unsigned int ms) { assert(ms == 50 || ms == 100); }
static u16 get_unaligned_le16(const void *ptr) {
 const u8 *p = ptr; return p[0] | ((u16)p[1] << 8);
}
static u32 get_unaligned_le32(const void *ptr) {
 const u8 *p = ptr; return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}
static void put_unaligned_le32(u32 value, void *ptr) {
 u8 *p = ptr; for (int i = 0; i < 4; i++) p[i] = value >> (8 * i);
}
static int exchange(struct ffa_device *dev, struct ffa_send_direct_data2 *msg) {
 u8 *p = (u8 *)msg->data;
 if (++exchanges == fail_at) return -EIO;
 if (dev->oem) {
  assert(p[0] == 12);
  u32 address = get_unaligned_le32(p + 1), length = get_unaligned_le32(p + 5);
  memset(p, 0, sizeof(*msg));
  if (address == 0x06000788) { assert(length == 6); memcpy(p, clock_bytes, 6); }
  else if (address == 0x06000504) { assert(length == 1); p[0] = status_reads++ ? status2 : status1; }
  else {
   assert(address == 0x06000800 && length == 5); p[0] = family; p[1] = operation;
   u16 value = stale_physical ? 7777 : physical_floor;
   p[3] = value; p[4] = value >> 8;
  }
 } else if (p[0] == 2) {
  memset(p, 0, sizeof(*msg));
  p[0] = submits ? (pending_after_submit ? 2 : completion_state) : (polls ? state_after : state_before);
  u16 value = stale_cache ? 7777 : physical_floor;
  p[1] = value; p[2] = value >> 8;
  polls++;
 } else {
  assert(p[0] == 1);
  if (p[1] == 5) {
   assert(restore_test && p[2] == 2 && p[3] == 0 && p[4] == 255 && p[5] == 255);
   assert(++writes == 1);
   if (!refuse_unset) physical_floor = 65535;
   for (unsigned int i = 6; i < sizeof(*msg); i++) assert(p[i] == 0);
  } else {
   assert(p[1] == 4 && p[2] == 0 && p[3] == 2);
   for (unsigned int i = 4; i < sizeof(*msg); i++) assert(p[i] == 0);
  }
  ++submits;
  if (restore_test) { family = 7; operation = 4; }
  memset(p, 0, sizeof(*msg)); p[0] = submit_status;
 }
 return 0;
}
'''
MAIN = r'''
int main(int argc, char **argv) {
 assert(argc == 2);
 struct msg_ops msg = {.sync_send_receive2 = exchange};
 struct ffa_ops ops = {.msg_ops = &msg};
 struct ffa_device packet = {.ops = &ops}, oem = {.ops = &ops, .oem = 1};
 if (!strncmp(argv[1], "restore_transport_", 18)) {
  restore_test = 1; restore_floor = 12375; state_before = state_after = 0;
  fail_at = atoi(argv[1] + 18);
  assert(diagnose(&packet, &oem) == -EIO);
  assert(writes == (fail_at > 17));
  assert(submits <= 5 && exchanges == fail_at);
  return 0;
 }
 recover = true;
 int expected = -EAGAIN, expected_submits = 0;
 if (!strncmp(argv[1], "restore_", 8)) {
  restore_test = 1; recover = false; restore_floor = 12375;
  state_before = state_after = 0;
  expected = 0; expected_submits = 5;
  if (!strcmp(argv[1], "restore_foreign")) { physical_floor = 7777; expected = -ESTALE; expected_submits = 1; }
  if (!strcmp(argv[1], "restore_stale_physical")) { stale_physical = 1; expected = -ESTALE; expected_submits = 1; }
  if (!strcmp(argv[1], "restore_stale_cache")) { stale_cache = 1; expected = -ESTALE; expected_submits = 1; }
  if (!strcmp(argv[1], "restore_refused")) { refuse_unset = 1; expected = -ESTALE; expected_submits = 4; }
  if (!strcmp(argv[1], "restore_busy")) { state_after = 2; expected = -EAGAIN; expected_submits = 0; }
  if (!strcmp(argv[1], "restore_conflicting_modes")) { recover = true; expected = -EAGAIN; expected_submits = 0; }
  if (!strcmp(argv[1], "restore_invalid_floor")) { restore_floor = 70000; expected = -EAGAIN; expected_submits = 0; }
 } else if (!strcmp(argv[1], "observe")) { recover = false; expected = 0; }
 else if (!strcmp(argv[1], "idle_shared") || !strcmp(argv[1], "idle_fan")) {
  if (!strcmp(argv[1], "idle_fan")) family = 7;
  expected = 0; expected_submits = 1;
 } else if (!strcmp(argv[1], "busy_reply")) status1 = status2 = 9;
 else if (!strcmp(argv[1], "busy_input")) status1 = status2 = 10;
 else if (!strcmp(argv[1], "changing_status")) status2 = 0;
 else if (!strcmp(argv[1], "empty_rtc")) memset(clock_bytes, 0, 6);
 else if (!strcmp(argv[1], "invalid_rtc")) clock_bytes[0] = 0x6a;
 else if (!strcmp(argv[1], "invalid_month")) clock_bytes[4] = 0x13;
 else if (!strcmp(argv[1], "foreign_header")) family = 0x15;
 else if (!strcmp(argv[1], "foreign_operation")) { family = 7; operation = 3; }
 else if (!strcmp(argv[1], "already_complete")) state_before = state_after = 0;
 else if (!strcmp(argv[1], "completed_during_reads")) state_after = 0;
 else if (!strcmp(argv[1], "invalid_poll")) { state_before = 3; expected = -EBADMSG; }
 else if (!strcmp(argv[1], "sender_busy")) { submit_status = 0x0a; expected = -EREMOTEIO; expected_submits = 1; }
 else if (!strcmp(argv[1], "retry_stuck")) { pending_after_submit = 1; expected = -ETIMEDOUT; expected_submits = 1; }
 else if (!strcmp(argv[1], "retry_rejected")) { completion_state = 1; expected = -EREMOTEIO; expected_submits = 1; }
 else if (!strcmp(argv[1], "retry_malformed")) { completion_state = 3; expected = -EBADMSG; expected_submits = 1; }
 else if (!strncmp(argv[1], "transport_", 10)) {
  fail_at = argv[1][10] - '0'; expected = -EIO;
  if (fail_at == 8) expected_submits = 1;
 } else if (!strcmp(argv[1], "address_guard")) {
  u8 out[6];
  assert(read_fixed(&oem, 0x06000500, 1, out) == -EPERM);
  assert(read_fixed(&oem, 0x06000504, 2, out) == -EPERM);
  assert(exchanges == 0); return 0;
 } else assert(!"unknown case");
 assert(diagnose(&packet, &oem) == expected);
 assert(submits == expected_submits);
 assert(polls <= 22 && exchanges <= 40);
 if (restore_test) {
  assert(writes == (expected_submits >= 4));
  if (!expected) assert(physical_floor == 65535);
 } else assert(writes == 0);
 return 0;
}
'''


class MailboxProbeTests(unittest.TestCase):
    def test_bounded_diagnosis_and_recovery(self):
        # Compile the actual protocol/guard functions; replace only Linux and FF-A.
        functions = SOURCE[SOURCE.index("static int poll_packet("):
                           SOURCE.index("static int __init mailbox_init(")]
        with tempfile.TemporaryDirectory(prefix="dgx-mailbox-tests-") as directory:
            path = Path(directory)
            source = path / "probe.c"
            source.write_text(SHIM + functions + MAIN)
            binary = path / "probe"
            result = subprocess.run(shlex.split(os.environ.get("CC", "cc")) + [
                "-std=c11", "-Wall", "-Wextra", "-Werror", str(source), "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            cases = re.findall(r'!strcmp\(argv\[1\], "([a-z_]+)"\)', MAIN)
            scenarios = (sorted(set(cases)) + ["restore_success"] +
                         [f"transport_{i}" for i in range(1, 9)] +
                         [f"restore_transport_{i}" for i in range(1, 29)])
            for case in scenarios:
                with self.subTest(case=case):
                    result = subprocess.run([str(binary), case], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
