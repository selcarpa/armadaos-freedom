#!/usr/bin/env python3
"""Offline tests for the elish M2 minimal initramfs builder.

The builder needs the aarch64 cross compiler; skip cleanly when it is not
installed rather than turning a toolchain absence into a product failure.
"""

import gzip
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
BUILDER = os.path.join(ROOT, "devices", "xiaomi-elish", "boot",
                       "build_min_initramfs.py")
CC = shutil.which("aarch64-linux-gnu-gcc")


def parse_newc(data):
    entries = []
    offset = 0
    while offset < len(data):
        if data[offset:offset + 6] != b"070701":
            raise AssertionError("bad newc magic at %d" % offset)
        fields = []
        for index in range(13):
            start = offset + 6 + index * 8
            fields.append(int(data[start:start + 8], 16))
        namesize = fields[11]
        filesize = fields[6]
        name_start = offset + 110
        name = data[name_start:name_start + namesize - 1].decode("utf-8")
        name_pad = (4 - (110 + namesize) % 4) % 4
        data_start = name_start + namesize + name_pad
        file_data = data[data_start:data_start + filesize]
        if name == "TRAILER!!!":
            break
        entries.append((name, fields[1], file_data, fields[9], fields[10]))
        offset = data_start + ((filesize + 3) // 4) * 4
    return entries


@unittest.skipUnless(CC, "aarch64-linux-gnu-gcc not installed")
class MinInitramfsTest(unittest.TestCase):
    def build(self, path):
        proc = subprocess.run(
            [sys.executable, "-B", BUILDER, "--out", path, "--cc", CC],
            capture_output=True, text=True)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        with open(path, "rb") as handle:
            return handle.read()

    def test_entries_static_init_and_limit(self):
        with tempfile.TemporaryDirectory(prefix="elish-initrd-test.") as tmp:
            path = os.path.join(tmp, "initramfs.cpio.gz")
            blob = self.build(path)
            report_path = os.path.splitext(path)[0] + ".json"
            with open(report_path, encoding="utf-8") as handle:
                report = json.load(handle)
            self.assertTrue(report["under_limit"])
            self.assertLessEqual(len(blob), report["limit"])
            self.assertEqual(report["sha256"], hashlib.sha256(blob).hexdigest())

            entries = parse_newc(gzip.decompress(blob))
            by_name = {name: (mode, data, rdev_major, rdev_minor)
                       for name, mode, data, rdev_major, rdev_minor in entries}
            self.assertIn(".", by_name)
            self.assertIn("init", by_name)
            self.assertIn("etc/armada-m2-canary", by_name)
            self.assertIn("dev/console", by_name)
            mode, init_data, _major, _minor = by_name["init"]
            self.assertTrue(stat.S_ISREG(mode))
            self.assertEqual(mode & 0o777, 0o755)
            self.assertTrue(init_data.startswith(b"\x7fELF"))
            self.assertEqual(init_data[4], 2)  # ELFCLASS64
            self.assertEqual(init_data[18:20], b"\xb7\x00")  # EM_AARCH64
            mode, canary, _major, _minor = by_name["etc/armada-m2-canary"]
            self.assertIn(b"ARMADA-ELISH-M2-CANARY", canary)
            mode, _data, major, minor = by_name["dev/console"]
            self.assertTrue(stat.S_ISCHR(mode))
            self.assertEqual((major, minor), (5, 1))

    def test_reproducible(self):
        with tempfile.TemporaryDirectory(prefix="elish-initrd-test.") as tmp:
            first = self.build(os.path.join(tmp, "a.cpio.gz"))
            second = self.build(os.path.join(tmp, "b.cpio.gz"))
            self.assertEqual(first, second)


if __name__ == "__main__":
    unittest.main()
