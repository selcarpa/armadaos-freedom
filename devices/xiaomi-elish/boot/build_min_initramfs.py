#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Build the tiny elish M2 maintenance initramfs (no dracut, no modules).

The elish unified kernel has CONFIG_DRM_MSM, CONFIG_DRM_FBDEV_EMULATION,
CONFIG_DRM_PANEL_NOVATEK_NT36523 and CONFIG_SCSI_UFS_QCOM built in, so the
first M2 canary does not need a Fedora/dracut userspace.  This builder compiles
one static aarch64 init, packs a deterministic newc cpio and gzips it with a
zero mtime.

Output policy: <= 7.5 MiB, leaving the packed-kernel raw budget room for the
page-aligned v3 boot image.  The script never touches a device.
"""

import argparse
import gzip
import hashlib
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DEFAULT_SOURCE = os.path.join(HERE, "min_initramfs", "init.c")
DEFAULT_CC = "aarch64-linux-gnu-gcc"
DEFAULT_LIMIT = 7_500 * 1024
CANARY_TEXT = (
    "ARMADA-ELISH-M2-CANARY\n"
    "panel=CSOT-first\n"
    "stage=maintenance\n"
    "init=/init\n"
)

CPIO_MAGIC = b"070701"


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def pad4(data):
    return data + b"\x00" * ((4 - len(data) % 4) % 4)


def cpio_entry(name, mode, data=b"", rdev=(0, 0), nlink=None, ino=0):
    name_bytes = name.encode("utf-8") + b"\x00"
    if nlink is None:
        nlink = 2 if stat.S_ISDIR(mode) else 1
    fields = [
        ino, mode, 0, 0, nlink, 0, len(data),
        0, 0, rdev[0], rdev[1], len(name_bytes), 0,
    ]
    header = CPIO_MAGIC + b"".join(b"%08X" % value for value in fields)
    # newc aligns header+name (110 + namesize) and then the file data to 4
    # bytes.  Padding namesize itself to 4 is wrong because 110 % 4 == 2.
    name_pad = b"\x00" * ((4 - (len(header) + len(name_bytes)) % 4) % 4)
    return header + name_bytes + name_pad + pad4(data)


def build_cpio(root, entries):
    chunks = []
    for index, entry in enumerate(sorted(entries, key=lambda item: item[0])):
        name = entry[0]
        mode = entry[1]
        path = os.path.join(root, name) if name != "." else root
        if stat.S_ISREG(mode):
            with open(path, "rb") as handle:
                data = handle.read()
        elif stat.S_ISLNK(mode):
            data = os.readlink(path).encode("utf-8")
        else:
            data = b""
        chunks.append(cpio_entry(name, mode, data, entry[2], ino=index + 1))
    chunks.append(cpio_entry("TRAILER!!!", 0, ino=len(chunks) + 1))
    return b"".join(chunks)


def make_root(root, init_path, source):
    os.makedirs(root)
    for directory in ("dev", "proc", "sys", "tmp", "run", "etc"):
        os.makedirs(os.path.join(root, directory), mode=0o755)
    shutil.copyfile(init_path, os.path.join(root, "init"))
    os.chmod(os.path.join(root, "init"), 0o755)
    with open(os.path.join(root, "etc", "armada-m2-canary"), "w",
              encoding="utf-8") as handle:
        handle.write(CANARY_TEXT)
    os.chmod(os.path.join(root, "etc", "armada-m2-canary"), 0o644)

    entries = [
        (".", stat.S_IFDIR | 0o755, (0, 0)),
        ("dev", stat.S_IFDIR | 0o755, (0, 0)),
        ("proc", stat.S_IFDIR | 0o755, (0, 0)),
        ("sys", stat.S_IFDIR | 0o755, (0, 0)),
        ("tmp", stat.S_IFDIR | 0o755, (0, 0)),
        ("run", stat.S_IFDIR | 0o755, (0, 0)),
        ("etc", stat.S_IFDIR | 0o755, (0, 0)),
        ("init", stat.S_IFREG | 0o755, (0, 0)),
        ("etc/armada-m2-canary", stat.S_IFREG | 0o644, (0, 0)),
        # Fallbacks in case devtmpfs is unavailable; init mounts devtmpfs over
        # /dev when it can, so these nodes are harmless.
        ("dev/console", stat.S_IFCHR | 0o600, (5, 1)),
        ("dev/null", stat.S_IFCHR | 0o666, (1, 3)),
        ("dev/kmsg", stat.S_IFCHR | 0o600, (1, 11)),
        ("dev/tty0", stat.S_IFCHR | 0o600, (4, 0)),
    ]
    return entries


def compile_init(cc, source, init_out):
    env = dict(os.environ)
    env["SOURCE_DATE_EPOCH"] = "0"
    cmd = [
        cc, "-static", "-Os", "-s", "-fno-ident", "-Wl,--build-id=none",
        "-Wall", "-Wextra", "-o", init_out, source,
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if proc.returncode != 0:
        raise SystemExit("error: compile failed:\n%s\n%s"
                         % (proc.stdout, proc.stderr))
    if proc.stderr.strip():
        sys.stderr.write(proc.stderr)


def main():
    parser = argparse.ArgumentParser(
        description="Build the elish M2 minimal maintenance initramfs")
    parser.add_argument("--out", required=True)
    parser.add_argument("--source", default=DEFAULT_SOURCE)
    parser.add_argument("--cc", default=DEFAULT_CC)
    parser.add_argument("--limit", type=int, default=DEFAULT_LIMIT)
    parser.add_argument("--force", action="store_true")
    args = parser.parse_args()

    out = os.path.abspath(args.out)
    report_path = os.path.splitext(out)[0] + ".json"
    if not args.force and os.path.exists(out):
        raise SystemExit("error: refusing to overwrite existing %s" % out)
    if not os.path.isfile(args.source):
        raise SystemExit("error: source not found: %s" % args.source)
    cc = shutil.which(args.cc)
    if cc is None:
        raise SystemExit("error: compiler not found: %s" % args.cc)
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)

    with tempfile.TemporaryDirectory(prefix="elish-m2-initrd.") as tmp:
        init_path = os.path.join(tmp, "init")
        compile_init(cc, args.source, init_path)
        root = os.path.join(tmp, "root")
        entries = make_root(root, init_path, args.source)
        cpio = build_cpio(root, entries)
        # Python's gzip mtime=0 keeps the archive reproducible.
        compressed = gzip.compress(cpio, compresslevel=9, mtime=0)

    if len(compressed) > args.limit:
        raise SystemExit("error: initramfs %d exceeds limit %d"
                         % (len(compressed), args.limit))
    with open(out, "wb") as handle:
        handle.write(compressed)
    os.chmod(out, 0o644)

    report = {
        "builder": os.path.abspath(__file__),
        "source": os.path.abspath(args.source),
        "source_sha256": sha256_bytes(open(args.source, "rb").read()),
        "compiler": cc,
        "output": out,
        "length": len(compressed),
        "sha256": sha256_bytes(compressed),
        "limit": args.limit,
        "under_limit": True,
        "uncompressed_cpio_length": len(cpio),
        "format": "newc cpio + gzip (mtime=0)",
        "entries": [
            {"name": name, "mode": "%o" % mode, "rdev": list(rdev)}
            for name, mode, rdev in sorted(entries, key=lambda item: item[0])
        ],
        "canary": CANARY_TEXT.strip().splitlines(),
        "hardware_tested": False,
        "offline_only": True,
        "caveat": (
            "The initramfs proves only that the Linux kernel can reach a static "
            "userspace init. It does not provide a root filesystem, touch, "
            "networking, or a full ArmadaOS session."),
    }
    with open(report_path, "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, ensure_ascii=False)
        handle.write("\n")
    print("wrote %s (%d bytes)" % (out, len(compressed)))
    print("wrote %s" % report_path)


if __name__ == "__main__":
    main()
