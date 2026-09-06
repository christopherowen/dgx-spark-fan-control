# SPDX-License-Identifier: GPL-2.0-only
"""Check enrollment decisions without building a module or reading real keys."""
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class SigningEnrollmentTests(unittest.TestCase):
    def test_requires_affirmative_enrollment_on_both_mokutil_exit_conventions(self):
        # Substitute the build-tree path in an isolated script copy: this tests
        # enrollment on CI too, where running-kernel headers may be absent.
        with tempfile.TemporaryDirectory(prefix="dgx-sign-tests-") as directory:
            root = Path(directory)
            (root / "scripts").mkdir()
            build = root / "headers"
            (build / "scripts").mkdir(parents=True)
            (build / "scripts/sign-file").touch()
            script = root / "scripts/build-sign"
            text = (ROOT / "scripts/build-sign").read_text()
            text = text.replace('kernel_build="/lib/modules/${kernel_release}/build"',
                                'kernel_build="${TEST_KERNEL_BUILD}"')
            script.write_text(text)
            script.chmod(0o755)
            keys = root / "keys"
            keys.mkdir()
            (keys / "MOK.priv").touch(mode=0o600)
            (keys / "MOK.der").touch()
            binary = root / "bin"
            binary.mkdir()
            for name, contents in {
                "mokutil": '#!/bin/sh\nprintf "%s\\n" "$ENROLLMENT_LINE"\nexit "$ENROLLMENT_EXIT"\n',
                "make": '#!/bin/sh\nexit 77\n',
            }.items():
                path = binary / name
                path.write_text(contents)
                path.chmod(0o755)
            for message, status, expected in (
                (f"{keys}/MOK.der is already enrolled", 0, 77),
                (f"{keys}/MOK.der is already enrolled", 1, 77),
                (f"{keys}/MOK.der is not enrolled", 0, 1),
                ("Failed to read enrollment", 1, 1),
            ):
                with self.subTest(message=message, status=status):
                    env = dict(os.environ, PATH=f"{binary}:{os.environ['PATH']}",
                               DGX_MOK_DIR=str(keys), TEST_KERNEL_BUILD=str(build),
                               ENROLLMENT_LINE=message, ENROLLMENT_EXIT=str(status))
                    result = subprocess.run([str(script)], env=env, capture_output=True, text=True)
                    self.assertEqual(result.returncode, expected, result.stderr)
