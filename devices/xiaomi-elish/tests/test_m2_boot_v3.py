#!/usr/bin/env python3
"""Focused offline tests for the elish v3 repacker size/AVB options.

These tests build a synthetic v3 boot image and use the vendored official
avbtool.  They do not touch a device, do not need the elish stock image and do
not exercise the MBKP packer.
"""

import gzip
import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
REPACKER = os.path.join(ROOT, "devices", "xiaomi-elish", "boot",
                        "repack_boot_v3.py")
AVBTOOL = os.path.join(ROOT, "packages", "kshim-tools", "vendor", "avbtool.py")
MIB = 1024 * 1024


def make_kernel(size):
    data = bytearray(size)
    struct.pack_into("<Q", data, 8, 0x80000)
    struct.pack_into("<Q", data, 16, size)
    data[56:60] = b"ARM\x64"
    return bytes(data)


def make_stock(path):
    kernel = make_kernel(MIB)
    ramdisk = gzip.compress(b"elish synthetic ramdisk\n" * 1024, mtime=0)
    header = bytearray(4096)
    header[0:8] = b"ANDROID!"
    struct.pack_into("<I", header, 8, len(kernel))
    struct.pack_into("<I", header, 12, len(ramdisk))
    struct.pack_into("<I", header, 20, 1580)
    struct.pack_into("<I", header, 40, 3)
    content = bytes(header) + kernel
    content += b"\x00" * ((4096 - len(content) % 4096) % 4096)
    content += ramdisk
    # Leave room for avbtool's footer/metadata; the partition is 8 MiB.
    content += b"\x00" * (4 * MIB - len(content))
    with open(path, "wb") as handle:
        handle.write(content)
    proc = subprocess.run(
        [sys.executable, "-B", AVBTOOL, "add_hash_footer",
         "--image", path, "--partition_size", str(8 * MIB),
         "--partition_name", "boot", "--algorithm", "NONE"],
        capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(proc.stdout + proc.stderr)
    return kernel, ramdisk


def run_repack(args):
    return subprocess.run(
        [sys.executable, "-B", REPACKER] + args,
        capture_output=True, text=True)


class RepackBootV3Test(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="elish-v3-test.")
        self.dir = self.tmp.name
        self.stock = os.path.join(self.dir, "stock.img")
        self.stock_kernel, self.stock_ramdisk = make_stock(self.stock)
        self.kernel = os.path.join(self.dir, "kernel")
        with open(self.kernel, "wb") as handle:
            handle.write(make_kernel(2 * MIB))

    def tearDown(self):
        self.tmp.cleanup()

    def report(self, name):
        with open(os.path.join(self.dir, name + ".json"), encoding="utf-8") as f:
            return json.load(f)

    def common(self, name):
        return [
            "--original", self.stock,
            "--kernel", self.kernel,
            "--output", os.path.join(self.dir, name + ".img"),
            "--report", os.path.join(self.dir, name + ".json"),
            "--avbtool", AVBTOOL,
        ]

    def verify_avb(self, image):
        alias = os.path.join(self.dir, "boot.img")
        if os.path.lexists(alias):
            os.unlink(alias)
        os.symlink(os.path.basename(image), alias)
        try:
            proc = subprocess.run(
                [sys.executable, "-B", AVBTOOL, "verify_image",
                 "--image", image], capture_output=True, text=True,
                cwd=self.dir)
        finally:
            os.unlink(alias)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("Successfully verified", proc.stdout)

    def test_default_keeps_128_style_length(self):
        proc = run_repack(self.common("default"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        report = self.report("default")
        self.assertEqual(report["output_length"], 8 * MIB)
        self.assertEqual(report["partition_limit"], 8 * MIB)
        self.assertTrue(report["ramdisk_preserved"])
        self.verify_avb(report["output"])

    def test_minimal_avb_uses_real_content_length(self):
        args = self.common("minimal")
        args += ["--max-output-size", str(16 * MIB), "--minimal-avb"]
        proc = run_repack(args)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        report = self.report("minimal")
        self.assertLess(report["output_length"], 16 * MIB)
        self.assertEqual(report["output_length"],
                         report["avb"]["partition_size"])
        self.assertGreaterEqual(report["output_length"],
                                report["avb"]["partition_size"])
        self.verify_avb(report["output"])

    def test_exact_192_style_partition_pads(self):
        args = self.common("exact")
        args += ["--max-output-size", str(16 * MIB),
                 "--avb-partition-size", str(16 * MIB)]
        proc = run_repack(args)
        self.assertEqual(proc.returncode, 0, proc.stderr)
        report = self.report("exact")
        self.assertEqual(report["output_length"], 16 * MIB)
        self.verify_avb(report["output"])

    def test_over_limit_fails_without_output(self):
        big = os.path.join(self.dir, "big-kernel")
        with open(big, "wb") as handle:
            handle.write(make_kernel(8 * MIB))
        args = self.common("overlimit")
        args[args.index(self.kernel)] = big
        args += ["--max-output-size", str(8 * MIB), "--minimal-avb"]
        proc = run_repack(args)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("exceeds partition limit", proc.stderr)
        self.assertFalse(os.path.exists(os.path.join(self.dir,
                                                     "overlimit.img")))

    def test_conflicting_avb_modes_fail(self):
        args = self.common("conflict")
        args += ["--minimal-avb", "--avb-partition-size", str(16 * MIB)]
        proc = run_repack(args)
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("mutually exclusive", proc.stderr)


if __name__ == "__main__":
    unittest.main()
