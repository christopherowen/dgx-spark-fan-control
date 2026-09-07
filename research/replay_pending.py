#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Offline replay of DGX Spark SoC 2.155.11 packet completion ordering.

Requires unicorn. Reads an independently downloaded socfw.cap, verifies its
hash, and executes selected original AArch64 routines in emulated memory.
No firmware bytes are distributed, and no devices are accessed. The ordering
experiment redirects execution only inside the emulator; it is not a flashable
patch or a complete firmware fix.
"""
import argparse
import hashlib
import json
from pathlib import Path

from unicorn import Uc, UC_ARCH_ARM64, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm64_const import (
    UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2,
    UC_ARM64_REG_LR, UC_ARM64_REG_PC, UC_ARM64_REG_SP,
)

CAPSULE_SHA256 = "0985b848b1708421a399935b7f9b1afb5469588db6f26bd2953c6013f3db70ff"
IMAGE_OFFSET = 0xA08F0F
IMAGE_SIZE = 0x41100  # Includes initialized data; BSS begins at 0x93981100.
BASE = 0x93940000
PENDING = 0x939A20AC
PACKET_RESPONSE = 0x939A2068
EVENT_STATUS = 0x9399F0F4
SCRATCH = 0x1000000
STOP = SCRATCH + 0xF000


class Replay:
    def __init__(self, image, early=False, error=None, metadata_first=False):
        self.uc = Uc(UC_ARCH_ARM64, UC_MODE_ARM)
        self.uc.mem_map(BASE, 0x70000)
        self.uc.mem_write(BASE, image)
        # Startup registers this callback at 0x9396ee8c..0x9396ee94.
        self.uc.mem_write(0x9399FD88, (0x93970664).to_bytes(8, "little"))
        self.uc.mem_map(SCRATCH, 0x10000)
        self.early = early
        self.error = error
        self.metadata_first = metadata_first
        self.metadata_phase = "before"
        self.mailbox = 0
        self.packet = bytes((7, 5, 0, 0x9C, 0x18))
        self.floor = 4500
        self.trace = []
        self.io_calls = 0
        self.uc.hook_add(UC_HOOK_CODE, self.instruction)
        self.uc.hook_add(UC_HOOK_MEM_WRITE, self.write)

    def reg(self, register):
        return self.uc.reg_read(register)

    def done(self, value=0):
        self.uc.reg_write(UC_ARM64_REG_X0, value & 0xFFFFFFFFFFFFFFFF)
        self.uc.reg_write(UC_ARM64_REG_PC, self.reg(UC_ARM64_REG_LR))

    def write(self, uc, access, address, size, value, user_data):
        if address == PENDING:
            self.trace.append({"pc": hex(self.reg(UC_ARM64_REG_PC)), "pending": value})

    def instruction(self, uc, address, size, user_data):
        # Causal intervention: execute the ORIGINAL metadata instructions before
        # setting up the sender arguments, then omit their post-send execution.
        # No EC timing, callback, data, or polling behavior changes between pairs.
        # This only tests successful submission ordering, not error unwinding or
        # arbitration of an old outstanding response in a production patch.
        if self.metadata_first:
            if address == 0x93974540 and self.metadata_phase == "before":
                self.metadata_phase = "initializing"
                uc.reg_write(UC_ARM64_REG_PC, 0x93974560)
                return
            if address == 0x93974580 and self.metadata_phase == "initializing":
                self.metadata_phase = "sent"
                uc.reg_write(UC_ARM64_REG_PC, 0x93974540)
                return
            if address == 0x93974560 and self.metadata_phase == "sent":
                uc.reg_write(UC_ARM64_REG_PC, 0x93974588)
                return
        if address == 0x9396792C:  # peripheral write boundary below real wrapper
            self.io_calls += 1
            target, length, source = (self.reg(r) for r in
                                     (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
            data = bytes(uc.mem_read(source, length))
            if target == 0x06000800:
                self.packet = data
            elif target == 0x06000504:
                assert data == b"\x07"
                if self.packet[1] == 5:
                    self.floor = int.from_bytes(self.packet[3:5], "little")
                else:
                    assert self.packet[1] == 4
                self.mailbox = 1  # Model completed EC response (OBF).
                if self.early:
                    uc.mem_write(EVENT_STATUS, (0x40).to_bytes(4, "little"))
            else:
                raise AssertionError(f"unexpected write {target:#x}")
            self.done()
        elif address == 0x9396A80C:  # EC reads, timing and failures simulated
            self.io_calls += 1
            target, length, destination = (self.reg(r) for r in
                                          (UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2))
            if target == 0x06000504:
                if self.error == "status_read" and self.mailbox:
                    self.done(-1)
                    return
                data = bytes((self.mailbox,))
            elif target == 0x06000800:
                if self.error == "family_read" and length == 1:
                    self.done(-1)
                    return
                if self.error == "response_read" and length > 1:
                    self.done(-1)
                    return
                data = bytes((7, self.packet[1], 0)) + self.floor.to_bytes(2, "little")
            elif target == 0x06000500:
                self.mailbox = 0  # Completion acknowledgement read.
                data = b"\0"
            else:
                raise AssertionError(f"unexpected read {target:#x}")
            uc.mem_write(destination, data[:length].ljust(length, b"\0"))
            self.done()
        elif address == 0x93968EE4:  # Deliver one modelled virtual-wire event.
            uc.mem_write(self.reg(UC_ARM64_REG_X0), (1).to_bytes(4, "little"))
            uc.mem_write(self.reg(UC_ARM64_REG_X1), b"\x00\x85")
            uc.mem_write(EVENT_STATUS, b"\0" * 4)
            self.done()
        elif address == 0x93944A78:
            self.done()  # Diagnostic logging.
        elif address == 0x93945DC0:
            raise AssertionError("firmware stack-canary failure")

    def call(self, address, *args):
        self.uc.reg_write(UC_ARM64_REG_SP, SCRATCH + 0xE000)
        self.uc.reg_write(UC_ARM64_REG_LR, STOP)
        for register, value in zip((UC_ARM64_REG_X0, UC_ARM64_REG_X1, UC_ARM64_REG_X2), args):
            self.uc.reg_write(register, value)
        self.uc.emu_start(address, STOP, count=100000)
        assert self.reg(UC_ARM64_REG_PC) == STOP, "instruction budget exhausted"
        return self.reg(UC_ARM64_REG_X0)

    def submit(self, operation=5):
        self.metadata_phase = "before"
        # Real outer packet handler takes an FF-A message with payload at +24.
        payload = bytes((1, 5, 2, 0, 0x9C, 0x18)) if operation == 5 else bytes((1, 4, 0, 2))
        self.uc.mem_write(SCRATCH + 24, payload)
        return self.call(0x9397476C, SCRATCH, SCRATCH + 0x100)

    def complete(self):
        self.uc.mem_write(EVENT_STATUS, (0x40).to_bytes(4, "little"))
        return self.call(0x9396A0A0)

    def poll(self):
        result = self.call(0x939745E4, SCRATCH + 0x100)
        assert result == 0
        return self.uc.mem_read(SCRATCH + 0x100, 1)[0]


def replay(image):
    results = []
    for case in ("normal", "early_completion", "lost_notification", "status_read", "family_read", "response_read"):
        model = Replay(image, early=case == "early_completion",
                       error=case if case.endswith("read") else None)
        if case == "response_read":
            model.uc.mem_write(PACKET_RESPONSE, bytes((7, 4, 0, 0x8C, 0x0A)))
        assert model.submit(4 if case == "response_read" else 5) == 0
        if case not in ("early_completion", "lost_notification"):
            model.complete()
        before = model.io_calls
        observed = [model.poll() for _ in range(100)]
        expected = 0 if case in ("normal", "response_read") else 2
        assert observed == [expected] * 100, (case, observed)
        expected_writes = ([0, 1] if case == "early_completion" else
                           [1, 0] if case in ("normal", "response_read") else [1])
        assert [event["pending"] for event in model.trace] == expected_writes
        assert model.io_calls == before, "poll unexpectedly contacted the EC"
        assert model.floor == (4500 if case == "response_read" else 6300)
        if case == "response_read":
            stale = int.from_bytes(model.uc.mem_read(SCRATCH + 0x101, 2), "little")
            assert stale == 2700 and stale != model.floor
        results.append({"case": case, "poll_state": expected,
                        "ec_floor": model.floor, "mailbox_status": model.mailbox,
                        "poll_ec_accesses": model.io_calls - before,
                        "pending_writes": model.trace})
        if case == "response_read":
            results[-1]["stale_observed_floor"] = stale
    model = Replay(image, early=True)
    assert model.submit() == 0 and model.poll() == 2 and model.mailbox == 0
    model.early = False
    assert model.submit(4) == 0  # Hypothesis: resubmit only with mailbox idle.
    model.complete()
    assert model.poll() == 0
    observed = int.from_bytes(model.uc.mem_read(SCRATCH + 0x101, 2), "little")
    assert observed == model.floor == 6300
    results.append({"case": "idle_mailbox_read_resubmit", "poll_state": 0,
                    "ec_floor": model.floor, "observed_floor": observed})
    model = Replay(image)
    assert model.submit() == 0 and model.poll() == 2 and model.mailbox == 1
    assert model.submit(4) == 0x0A and model.poll() == 2
    results.append({"case": "busy_mailbox_read_resubmit", "submit_status": 0x0A,
                    "poll_state": 2, "ec_floor": model.floor})

    # Same isolated request, same EC response, only metadata ordering changes.
    # Prime with a completed read or write to exercise both old output lengths.
    # There are no other clients, overlapping requests, or injected I/O errors.
    for previous_operation in (4, 5):
        for operation in (4, 5):
            for early in (False, True):
                for metadata_first in (False, True):
                    model = Replay(image)
                    assert model.submit(previous_operation) == 0
                    model.complete()
                    assert model.poll() == 0 and model.mailbox == 0
                    model.trace.clear()
                    model.early = early
                    model.metadata_first = metadata_first
                    assert model.submit(operation) == 0
                    if not early:
                        model.complete()
                    before = model.io_calls
                    expected = 2 if early and not metadata_first else 0
                    assert [model.poll() for _ in range(100)] == [expected] * 100
                    assert model.io_calls == before
                    assert model.mailbox == 0
                    writes = [event["pending"] for event in model.trace]
                    assert writes == ([0, 1] if expected == 2 else [1, 0])
                    result = {
                        "case": "isolated_request_ordering",
                        "previous_operation": previous_operation,
                        "operation": operation,
                        "early_completion": early,
                        "metadata_first": metadata_first,
                        "poll_state": expected,
                        "mailbox_status": model.mailbox,
                        "ec_floor": model.floor,
                        "poll_ec_accesses": model.io_calls - before,
                        "pending_writes": model.trace,
                    }
                    if operation == 4 and expected == 0:
                        observed = int.from_bytes(model.uc.mem_read(SCRATCH + 0x101, 2), "little")
                        assert observed == model.floor
                        result["observed_floor"] = observed
                    results.append(result)
    return results


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capsule", type=Path)
    args = parser.parse_args()
    capsule = args.capsule.read_bytes()
    if hashlib.sha256(capsule).hexdigest() != CAPSULE_SHA256:
        parser.error("capsule hash does not match SoC 2.155.11")
    image = capsule[IMAGE_OFFSET:IMAGE_OFFSET + IMAGE_SIZE]
    print(json.dumps(replay(image), indent=2))


if __name__ == "__main__":
    main()
