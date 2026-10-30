#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Offline Xiaomi Pad 5 Pro (elish) v3 Linux maintenance boot packaging driver.

Drives the external MBKP packer, the supplied UnpackTool, the narrow v3 boot
repacker (repack_boot_v3.py) and the official avbtool over the fixed 5-entry
maintenance layout used by the elish M2 bring-up:

  Image-00  stock Android kernel Image  (base)
  Image-01  unified Linux ARM64 Image    (maintenance payload)
  Image-02  derived elish DTB            (CSOT first; BOE fallback)
  Image-03  minimal initramfs Blob
  Attached Shim (auto-appended by the packer)

The original boot ramdisk is preserved verbatim by the v3 repacker.  Xiaomi's
v3 boot header has no separate DTB section and kShimC's base handoff passes
only the DT, so the stock Android ramdisk must stay inside boot.img.

Output size policy: the elish GPT boot partition is 192 MiB but the stock image
is 128 MiB.  The repacker receives --max-output-size 201326592 plus
--minimal-avb, so the AVB footer sits at the real content end instead of
padding the file to the partition size.  The packed kernel itself still uses
the conservative 128 MiB loader budget until an elish raw profile is measured.

Offline verification only: no device access, no flashing and no reboot.
hardware_tested is always false and offline_only is always true.
"""

import argparse
import configparser
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

sys.dont_write_bytecode = True

RUNTIME_SIZE = 4 * 1024 * 1024
ARM64_HDR_SIZE = 0x40
ARM64_MAGIC = b"ARM\x64"
QC_ABOOT_MAGIC = b"UNCOMPRESSED_IMG"
GZIP_MAGIC = b"\x1f\x8b"
BOOT_MAGIC = b"ANDROID!"
MANIFEST_MAGIC = 0x464D504B
MANIFEST_HEADER_SIZE = 60
MANIFEST_ENTRY_SIZE = 56
MANIFEST_ENTRY_ALIGN = 4
FDT_MAGIC = 0xD00DFEED
AVB_MAGIC = b"AVBf"

MF_TYPE_LINUX = 1
MF_TYPE_SHIM = 3
MF_TYPE_BLOB = 4
MF_TYPE_DTB = 5
FLAG_BASE = 1
FLAG_EXEC = 4
FLAG_HAS_DTB = 8
FLAG_QC = 16

# The elish GPT boot partition is 192 MiB; the stock image is only 128 MiB.
BOOT_PARTITION_MAX = 192 * 1024 * 1024
# Conservative loader budget retained for elish until a raw profile is measured.
DEFAULT_RAW_BUDGET = 128 * 1024 * 1024
BL_RANGE = 0x8000000

TIMEOUT_MS = 8000

BASE_NAME = "原厂 Android"
LINUX_NAME = "Linux 维护模式"
DTB_NAME = "Elish DTB"
BLOB_NAME = "M2 initramfs"
SHIM_NAME = "Attached Shim"

# Fixed manifest entry order (config section order; the shim is auto-added last).
# index, name, type, flags, alignment exponent, device-tree reference
EXPECTED_ENTRIES = [
    (0, BASE_NAME, MF_TYPE_LINUX, FLAG_BASE | FLAG_EXEC, 21, 0),
    (1, LINUX_NAME, MF_TYPE_LINUX, FLAG_EXEC | FLAG_HAS_DTB, 21, 2),
    (2, DTB_NAME, MF_TYPE_DTB, 0, 3, 0),
    (3, BLOB_NAME, MF_TYPE_BLOB, 0, 16, 0),
    (4, SHIM_NAME, MF_TYPE_SHIM, FLAG_EXEC, 2, 0),
]

# Maintenance boot defaults to Linux; the Android base remains selectable in
# the kShim menu, and `fastboot boot` never touches the installed boot slot.
DEFAULT_LABEL = "Image-01"
DEFAULT_INDEX = 1

REQUIRED_COMPATIBLE = ("xiaomi,elish", "qcom,sm8250")
FORBIDDEN_COMPATIBLE = frozenset((
    "lenovo,elden",
    "lenovo,tb323fu",
    "qcom,sm8750",
    "qcom,sm8850",
    "qcom,kaanapali",
))
PANEL_COMPAT = {
    "csot": b"xiaomi,elish-csot-nt36523",
    "boe": b"xiaomi,elish-boe-nt36523",
}
ALLOWED_CHOSEN_PROPS = frozenset((
    "bootargs",
    "armada,boot-stage",
    "kshim,initrd-entry",
    "linux,initrd-start",
    "linux,initrd-end",
))

CMDLINE_START = 44
CMDLINE_END = 1580
CMDLINE_LEN = CMDLINE_END - CMDLINE_START


DEFAULT_REPACKER = os.path.join(
    os.path.dirname(os.path.abspath(__file__)), "repack_boot_v3.py")

STAGE = "init"
CHECKS = []
COMMANDS = []
TOOL = {}

OUT = None
OUT_OWNED = False
WORK = None
LOGS = None
TMP = None
ENV = None


class StageError(Exception):
    def __init__(self, stage, message):
        super().__init__(message)
        self.stage = stage
        self.message = message


def set_stage(stage):
    global STAGE
    STAGE = stage


def check(name, condition, detail=""):
    CHECKS.append({"check": name, "ok": bool(condition), "detail": detail})
    if not condition:
        raise StageError(STAGE, name + ((": " + detail) if detail else ""))


def le32(buf, offset):
    return struct.unpack_from("<I", buf, offset)[0]


def le64(buf, offset):
    return struct.unpack_from("<Q", buf, offset)[0]


def be32(buf, offset):
    return struct.unpack_from(">I", buf, offset)[0]


def align_up(value, alignment):
    return (value + alignment - 1) // alignment * alignment


def sha256_bytes(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_bytes(path, label):
    if not os.path.isfile(path):
        raise StageError(STAGE, "%s not found: %s" % (label, path))
    with open(path, "rb") as handle:
        return handle.read()


def run(cmd, name, stdout_path=None, expect_success=True):
    COMMANDS.append({"name": name, "argv": list(cmd)})
    err_log = os.path.join(LOGS, name + ".stderr")
    out_log = os.path.join(LOGS, name + ".stdout")
    if stdout_path is not None:
        with open(stdout_path, "wb") as fout, open(err_log, "wb") as ferr:
            proc = subprocess.run(cmd, stdout=fout, stderr=ferr, cwd=WORK, env=ENV)
    else:
        with open(out_log, "wb") as fout, open(err_log, "wb") as ferr:
            proc = subprocess.run(cmd, stdout=fout, stderr=ferr, cwd=WORK, env=ENV)
    if expect_success and proc.returncode != 0:
        raise StageError(
            STAGE, "command failed rc=%d: %s" % (proc.returncode, " ".join(cmd)))
    return proc.returncode, out_log, err_log


def run_capture(cmd, name, expect_success=True):
    COMMANDS.append({"name": name, "argv": list(cmd)})
    proc = subprocess.run(cmd, capture_output=True, cwd=WORK, env=ENV)
    with open(os.path.join(LOGS, name + ".stdout"), "wb") as handle:
        handle.write(proc.stdout)
    with open(os.path.join(LOGS, name + ".stderr"), "wb") as handle:
        handle.write(proc.stderr)
    if expect_success and proc.returncode != 0:
        raise StageError(
            STAGE,
            "command failed rc=%d: %s: %s"
            % (proc.returncode, " ".join(cmd),
               proc.stderr.decode("utf-8", "replace").strip()))
    return proc.returncode, proc.stdout


def require_tool(name):
    path = shutil.which(name)
    check("tool:%s" % name, path is not None, "not found in PATH")
    TOOL[name] = path
    return path


def stdout_text(name):
    with open(os.path.join(LOGS, name + ".stdout"), encoding="utf-8",
              errors="replace") as handle:
        return handle.read()


def parse_arm64(data, label):
    check(label + ":size>=64", len(data) >= ARM64_HDR_SIZE, "len=%d" % len(data))
    check(label + ":raw-not-qc",
          data[0:len(QC_ABOOT_MAGIC)] != QC_ABOOT_MAGIC,
          "QC aboot wrapper present")
    check(label + ":raw-not-gzip", data[0:2] != GZIP_MAGIC,
          "gzip stream present; raw Android Image required")
    check(label + ":magic", data[56:60] == ARM64_MAGIC, "bad ARM64 magic at 0x38")
    text_offset = le64(data, 8)
    image_size = le64(data, 16)
    check(label + ":image_size", image_size >= 64, "image_size=%d" % image_size)
    slot = max(len(data), image_size)
    return {"text_offset": text_offset, "image_size": image_size,
            "length": len(data), "effective_slot": slot}


def parse_manifest(buf, offset):
    magic, vmaj, vmin, count, default, timeout = struct.unpack_from(
        "<IHHIII", buf, offset)
    backup = struct.unpack_from("<QQQQQ", buf, offset + 20)
    entries = []
    cursor = offset + MANIFEST_HEADER_SIZE
    for _ in range(count):
        (e_off, e_size, e_type, e_flags, e_load, e_entry, e_dtb, e_align,
         e_copy, e_namelen) = struct.unpack_from("<QQIIQIIIQI", buf, cursor)
        name_bytes = buf[cursor + MANIFEST_ENTRY_SIZE:
                         cursor + MANIFEST_ENTRY_SIZE + e_namelen]
        record = align_up(MANIFEST_ENTRY_SIZE + e_namelen, MANIFEST_ENTRY_ALIGN)
        pad = buf[cursor + MANIFEST_ENTRY_SIZE + e_namelen: cursor + record]
        entries.append({
            "offset": e_off, "size": e_size, "type": e_type, "flags": e_flags,
            "load_address": e_load, "entry_offset": e_entry,
            "dtb_index": e_dtb, "alignment": e_align, "copy_size_max": e_copy,
            "name_len": e_namelen, "name_bytes": name_bytes, "pad": pad,
            "record_size": record,
        })
        cursor += record
    return {"magic": magic, "version_major": vmaj, "version_minor": vmin,
            "count": count, "default": default, "timeout": timeout,
            "backup": backup, "entries": entries, "end": cursor}


def parse_boot_header(data):
    return {
        "magic": data[0:8],
        "kernel_size": le32(data, 8),
        "ramdisk_size": le32(data, 12),
        "header_size": le32(data, 20),
        "version": le32(data, 40),
    }


def payload(raw, entry):
    return raw[entry["offset"]:entry["offset"] + entry["size"]]


def parse_dts(text):
    """Parse canonical dtc DTS text into {node_path: {property: raw_value}}."""
    result = {}
    stack = []
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if (not line or line.startswith("//") or line.startswith("/*")
                or line.startswith("*") or line.startswith("/memreserve/")):
            continue
        if line.startswith("/dts-v1/"):
            continue
        if line.endswith("{"):
            name = line[:-1].strip()
            if ":" in name:
                name = name.split(":", 1)[1].strip()
            if name == "":
                name = "/"
            stack.append(name)
            continue
        if line.startswith("}"):
            if stack:
                stack.pop()
            continue
        if line.endswith(";"):
            body = line[:-1].strip()
            if "=" in body:
                key, val = body.split("=", 1)
                key, val = key.strip(), val.strip()
            else:
                key, val = body.strip(), None
            path = "/" + "/".join(stack[1:]) if len(stack) > 1 else "/"
            result.setdefault(path, {})[key] = val
    return result


def dts_changes(before, after):
    changed = []
    for path in set(before) | set(after):
        props_b = before.get(path, {})
        props_a = after.get(path, {})
        for key in set(props_b) | set(props_a):
            if props_b.get(key, "<absent>") != props_a.get(key, "<absent>"):
                changed.append((path, key))
    return sorted(changed)


def fdtget_strings(dtb, node, prop, name):
    _rc, out = run_capture([TOOL["fdtget"], "-t", "s", dtb, node, prop], name)
    return [token for token in out.decode("utf-8", "replace").split() if token]


def fdtget_string(dtb, node, prop, name):
    _rc, out = run_capture([TOOL["fdtget"], "-t", "s", dtb, node, prop], name)
    text = out.decode("utf-8", "replace")
    if text.endswith("\n"):
        text = text[:-1]
    return text


def fdtget_hex(dtb, node, prop, name):
    _rc, out = run_capture([TOOL["fdtget"], "-t", "x", dtb, node, prop], name)
    return out.decode("utf-8", "replace").strip()


def detect_panel(dtb_bytes):
    found = [panel for panel, token in PANEL_COMPAT.items() if token in dtb_bytes]
    if len(found) == 1:
        return found[0]
    return None


def build_dtb(inputs, bootargs, boot_stage=None, initrd_index=3):
    """Copy the supplied DTB into WORK and set only the allowlisted /chosen
    properties. Returns (derived_path, derived_bytes, changed, dts_paths).
    """
    set_stage("dtb")

    source = read_bytes(inputs["dtb"], "source dtb")
    check("dtb:magic", len(source) >= 8 and be32(source, 0) == FDT_MAGIC,
          "bad FDT magic")
    check("dtb:totalsize", len(source) >= 8 and be32(source, 4) == len(source),
          "totalsize=%s len=%d"
          % (be32(source, 4) if len(source) >= 8 else -1, len(source)))

    compatible = fdtget_strings(inputs["dtb"], "/", "compatible",
                                "fdtget-compat-src")
    for required in REQUIRED_COMPATIBLE:
        check("dtb:compatible-required:%s" % required, required in compatible,
              "compatible=%r" % compatible)
    for forbidden in sorted(FORBIDDEN_COMPATIBLE):
        check("dtb:compatible-forbidden:%s" % forbidden,
              forbidden not in compatible, "compatible=%r" % compatible)

    panel = detect_panel(source)
    check("dtb:panel-known", panel is not None,
          "neither CSOT nor BOE panel compatible found")

    derived = os.path.join(WORK, "derived.dtb")
    with open(derived, "wb") as handle:
        handle.write(source)

    _rc, listing = run_capture([TOOL["fdtget"], "-l", derived, "/"],
                               "fdtget-list-root")
    nodes = listing.decode("utf-8", "replace").split()
    if "chosen" not in nodes:
        run_capture([TOOL["fdtput"], "-c", derived, "/chosen"],
                    "fdtput-create-chosen")
    run_capture([TOOL["fdtget"], "-l", derived, "/chosen"],
                "fdtget-list-chosen")

    before_dts = os.path.join(WORK, "source.dts")
    run_capture([TOOL["dtc"], "-I", "dtb", "-O", "dts", "-o", before_dts,
                 inputs["dtb"]], "dtc-source")
    with open(before_dts, encoding="utf-8", errors="replace") as handle:
        before_text = handle.read()

    _rc, props = run_capture([TOOL["fdtget"], "-p", derived, "/chosen"],
                             "fdtget-props-chosen")
    present = set(props.decode("utf-8", "replace").split())
    for stale in ("linux,initrd-start", "linux,initrd-end"):
        if stale in present:
            run_capture([TOOL["fdtput"], "-d", derived, "/chosen", stale],
                        "fdtput-remove-" + stale.replace(",", "_"))

    run_capture([TOOL["fdtput"], "-t", "s", derived, "/chosen", "bootargs",
                 bootargs], "fdtput-bootargs")
    # elish uses a normal manifest payload, not kShimC's Lenovo-board-gated
    # diagnostic role.  Leaving armada,boot-stage unset keeps the row on the
    # main menu as "Linux 维护模式" and stops the session default resolver from
    # turning it into an advanced-page diagnostic.  boot_stage stays available
    # for callers that explicitly want the diagnostic role on supported boards.
    if boot_stage is None:
        if "armada,boot-stage" in present:
            run_capture([TOOL["fdtput"], "-d", derived, "/chosen",
                         "armada,boot-stage"], "fdtput-remove-boot-stage")
    else:
        run_capture([TOOL["fdtput"], "-t", "s", derived, "/chosen",
                     "armada,boot-stage", boot_stage], "fdtput-boot-stage")
    run_capture([TOOL["fdtput"], "-t", "x", derived, "/chosen",
                 "kshim,initrd-entry", str(initrd_index)],
                "fdtput-initrd-entry")

    after_dts = os.path.join(WORK, "derived.dts")
    run_capture([TOOL["dtc"], "-I", "dtb", "-O", "dts", "-o", after_dts,
                 derived], "dtc-derived")
    with open(after_dts, encoding="utf-8", errors="replace") as handle:
        after_text = handle.read()

    changed = dts_changes(parse_dts(before_text), parse_dts(after_text))
    for path, prop in changed:
        check("dtb:change-allowed:%s:%s" % (path, prop),
              path == "/chosen" and prop in ALLOWED_CHOSEN_PROPS,
              "unexpected structural change at %s %s" % (path, prop))

    derived_bytes = read_bytes(derived, "derived dtb")
    check("dtb:derived-magic", be32(derived_bytes, 0) == FDT_MAGIC, "bad magic")
    check("dtb:derived-totalsize", be32(derived_bytes, 4) == len(derived_bytes),
          "totalsize mismatch")
    d_compat = fdtget_strings(derived, "/", "compatible",
                              "fdtget-compat-derived")
    for required in REQUIRED_COMPATIBLE:
        check("dtb:derived-compatible-required:%s" % required,
              required in d_compat, "compatible=%r" % d_compat)
    for forbidden in sorted(FORBIDDEN_COMPATIBLE):
        check("dtb:derived-compatible-forbidden:%s" % forbidden,
              forbidden not in d_compat, "compatible=%r" % d_compat)

    got_bootargs = fdtget_string(derived, "/chosen", "bootargs",
                                 "fdtget-bootargs")
    check("dtb:derived-bootargs", got_bootargs == bootargs,
          "bootargs=%r expected=%r" % (got_bootargs, bootargs))
    if boot_stage is None:
        rc, _ = run_capture([TOOL["fdtget"], "-t", "s", derived, "/chosen",
                             "armada,boot-stage"], "fdtget-boot-stage",
                            expect_success=False)
        check("dtb:derived-stage-absent", rc != 0,
              "armada,boot-stage must not mark the elish payload as a diagnostic")
    else:
        stage = "".join(fdtget_strings(derived, "/chosen", "armada,boot-stage",
                                       "fdtget-boot-stage"))
        check("dtb:derived-stage", stage == boot_stage, "stage=%r" % stage)
    entry = fdtget_hex(derived, "/chosen", "kshim,initrd-entry",
                       "fdtget-initrd-entry")
    check("dtb:derived-initrd-entry-single", entry and " " not in entry,
          "not exactly one 32-bit cell: %r" % entry)
    check("dtb:derived-initrd-entry", int(entry, 16) == initrd_index,
          "initrd-entry=%s expected=%d" % (entry, initrd_index))

    return derived, derived_bytes, changed, {
        "source_dts": before_dts, "derived_dts": after_dts, "panel": panel,
        "compatible": d_compat,
    }


def config_safe(path, label):
    check("config:safe:" + label,
          not any(ch in path for ch in "\n\r#;"),
          "unsafe character in path: %r" % path)
    return path


def write_pack_config(config_path, inputs, stock_path, padded_path,
                      derived_path, packed_kernel, linux_entry_offset):
    lines = [
        "[Pack]",
        "Shim=" + config_safe(inputs["runtime"], "shim"),
        "Output=" + config_safe(packed_kernel, "output"),
        "Default=" + DEFAULT_LABEL,
        "Timeout=%d" % TIMEOUT_MS,
        "",
        "[Image-00]",
        "Name=" + BASE_NAME,
        "Path=" + config_safe(stock_path, "image00"),
        "Type=Linux",
        "BaseImage=true",
        "Align=21",
        "",
        "[Image-01]",
        "Name=" + LINUX_NAME,
        "Path=" + config_safe(padded_path, "image01"),
        "Type=Linux",
        "BaseImage=false",
        "Align=21",
        "EntryOffset=%d" % linux_entry_offset,
        "DeviceTreeBlob=Image-02",
        "",
        "[Image-02]",
        "Name=" + DTB_NAME,
        "Path=" + config_safe(derived_path, "image02"),
        "Type=DTB",
        "BaseImage=false",
        "Align=3",
        "",
        "[Image-03]",
        "Name=" + BLOB_NAME,
        "Path=" + config_safe(inputs["initrd"], "image03"),
        "Type=Blob",
        "BaseImage=false",
        "Align=16",
        "",
        "[Manifest]",
        "Type=Manifest",
        "",
    ]
    with open(config_path, "w", encoding="utf-8") as handle:
        handle.write("\n".join(lines))


def is_branch(word):
    return (word & 0xfc000000) == 0x14000000


def main():
    global OUT, WORK, LOGS, TMP, ENV

    parser = argparse.ArgumentParser(
        description="Offline Xiaomi elish v3 Linux maintenance boot driver")
    parser.add_argument("--stock-boot", required=True)
    parser.add_argument("--linux-image", required=True)
    parser.add_argument("--dtb", required=True)
    parser.add_argument("--initrd", required=True)
    parser.add_argument("--bootargs", required=True)
    parser.add_argument("--runtime", required=True)
    parser.add_argument("--packer", required=True)
    parser.add_argument("--unpacker", required=True)
    parser.add_argument("--avbtool", required=True)
    parser.add_argument("--repacker", default=None)
    parser.add_argument("--out", required=True)
    parser.add_argument("--source-label", default="")
    parser.add_argument("--output-name", default="boot.img")
    parser.add_argument("--panel", choices=("auto", "csot", "boe"),
                        default="auto")
    parser.add_argument(
        "--raw-profile", choices=("conservative", "xbl-evidence"),
        default="conservative",
        help="packed-kernel loader budget: conservative 128 MiB default, or "
             "the evidence-only 192 MiB profile derived from the XBL "
             "0xA0000000/0x10000000 'Kernel' reservation")
    parser.add_argument("--max-output-size", type=int,
                        default=BOOT_PARTITION_MAX,
                        help="boot partition/output limit in bytes "
                             "(default: 192 MiB)")
    parser.add_argument(
        "--pad-to-max-output", action="store_true",
        help="pad the AVB footer to --max-output-size instead of using the "
             "minimal real-content footer; fallback if a device insists on a "
             "partition-sized boot image")
    args = parser.parse_args()

    if args.max_output_size <= 0 or args.max_output_size % 4096 != 0:
        parser.error("--max-output-size must be a positive multiple of 4096")
    if args.max_output_size > BOOT_PARTITION_MAX:
        parser.error("--max-output-size exceeds the 192 MiB elish boot partition")
    if args.max_output_size < 64 * 1024 * 1024:
        parser.error("--max-output-size is too small for a boot image")
    if (not args.output_name or os.path.basename(args.output_name) != args.output_name
            or args.output_name in (".", "..")):
        parser.error("--output-name must be a plain filename")

    inputs = {
        "stock_boot": os.path.abspath(args.stock_boot),
        "linux_image": os.path.abspath(args.linux_image),
        "dtb": os.path.abspath(args.dtb),
        "initrd": os.path.abspath(args.initrd),
        "runtime": os.path.abspath(args.runtime),
        "packer": os.path.abspath(args.packer),
        "unpacker": os.path.abspath(args.unpacker),
        "avbtool": os.path.abspath(args.avbtool),
        "repacker": os.path.abspath(args.repacker or DEFAULT_REPACKER),
    }
    panel = None if args.panel == "auto" else args.panel
    OUT = os.path.abspath(args.out)

    try:
        return run_driver(inputs, args.source_label, args.bootargs, panel,
                          args.max_output_size, args.pad_to_max_output,
                          args.output_name, args.raw_profile)
    except StageError as error:
        stage, message = error.stage, error.message
    except Exception as error:
        stage, message = STAGE, "%s: %s" % (type(error).__name__, error)

    failure = {
        "stage": stage,
        "error": message,
        "source_label": args.source_label,
        "offline_verified": False,
        "offline_only": True,
        "hardware_tested": False,
        "checks": CHECKS,
        "commands": COMMANDS,
    }
    if OUT_OWNED:
        try:
            with open(os.path.join(OUT, "failure.json"), "w",
                      encoding="utf-8") as handle:
                json.dump(failure, handle, indent=2, ensure_ascii=False)
                handle.write("\n")
        except OSError:
            pass
    sys.stderr.write("FAILED at stage '%s': %s\n" % (stage, message))
    return 1


def args_out_raw():
    for index, value in enumerate(sys.argv):
        if value == "--out" and index + 1 < len(sys.argv):
            return sys.argv[index + 1]
    return OUT


def prepare_environment(inputs):
    global WORK, LOGS, TMP, ENV, OUT_OWNED

    set_stage("prepare")
    for key, path in inputs.items():
        check("input:%s" % key, os.path.isfile(path), path)
    check("out:not-symlink", not os.path.islink(args_out_raw()), "output is a symlink")
    if os.path.exists(OUT):
        check("out:is-dir", os.path.isdir(OUT), "exists and is not a directory")
        check("out:empty", not os.listdir(OUT), "existing directory is not empty")
    else:
        os.makedirs(OUT)

    real_out = os.path.realpath(OUT)
    for key, path in inputs.items():
        real_in = os.path.realpath(path)
        check("out:distinct:%s" % key, real_in != real_out, "output equals input")
        check("out:not-inside:%s" % key,
              not real_in.startswith(real_out + os.sep),
              "input %s lives inside --out" % key)

    OUT_OWNED = True
    WORK = os.path.join(OUT, "work")
    LOGS = os.path.join(OUT, "logs")
    TMP = os.path.join(OUT, "tmp")
    for path in (WORK, LOGS, TMP):
        os.makedirs(path)

    ENV = dict(os.environ)
    ENV["PYTHONDONTWRITEBYTECODE"] = "1"
    ENV["TMPDIR"] = TMP
    ENV["TEMP"] = TMP
    ENV["TMP"] = TMP
    tempfile.tempdir = TMP
    os.chdir(WORK)


def run_driver(inputs, source_label, bootargs, expect_panel,
               max_output_size, pad_to_max_output, output_name, raw_profile):
    prepare_environment(inputs)

    for name in ("fdtput", "fdtget", "dtc"):
        require_tool(name)

    set_stage("validate-inputs")
    stock_boot = read_bytes(inputs["stock_boot"], "stock boot")
    linux_image = read_bytes(inputs["linux_image"], "linux image")
    runtime = read_bytes(inputs["runtime"], "runtime shim")
    initrd_bytes = read_bytes(inputs["initrd"], "initrd")

    check("stock_boot:size-min", len(stock_boot) >= 64 * 1024 * 1024,
          "len=%d expected at least 64 MiB" % len(stock_boot))
    check("stock_boot:size<=output-limit", len(stock_boot) <= max_output_size,
          "len=%d max=%d" % (len(stock_boot), max_output_size))
    boot = parse_boot_header(stock_boot)
    check("stock_boot:magic", boot["magic"] == BOOT_MAGIC, repr(boot["magic"]))
    check("stock_boot:version", boot["version"] == 3,
          "version=%d" % boot["version"])
    check("stock_boot:header_size", boot["header_size"] == 1580,
          "header_size=%d" % boot["header_size"])
    kernel_size = boot["kernel_size"]
    ramdisk_size = boot["ramdisk_size"]
    check("stock_boot:kernel_size",
          kernel_size != 0 and 4096 + kernel_size <= len(stock_boot),
          "kernel_size=%d" % kernel_size)
    check("stock_boot:ramdisk_size", ramdisk_size > 0,
          "elish stock boot must carry a boot ramdisk")
    check("stock_boot:ramdisk-bounds",
          4096 + align_up(kernel_size, 4096) + ramdisk_size <= len(stock_boot),
          "ramdisk_size=%d" % ramdisk_size)

    stock_image = stock_boot[4096:4096 + kernel_size]
    check("stock_image:len", len(stock_image) == kernel_size,
          "len=%d kernel_size=%d" % (len(stock_image), kernel_size))
    stock_info = parse_arm64(stock_image, "stock_image")

    ramdisk_off = 4096 + align_up(kernel_size, 4096)
    stock_ramdisk = stock_boot[ramdisk_off:ramdisk_off + ramdisk_size]
    check("stock_ramdisk:len", len(stock_ramdisk) == ramdisk_size,
          "len=%d ramdisk_size=%d" % (len(stock_ramdisk), ramdisk_size))

    check("runtime:size", len(runtime) == RUNTIME_SIZE,
          "len=%d expected=%d" % (len(runtime), RUNTIME_SIZE))

    linux_info = parse_arm64(linux_image, "linux_image")
    # The packer requires the secondary Linux Image's text_offset residue to
    # match the Android base so it can place it at a 2 MiB boundary.  elish's
    # stock Android Image uses text_offset=0x80000 while the mainline vmlinuz
    # uses 0x0.  kShimC boots the selected entry at linux_base+entry.offset and
    # does not consume this header field, so relocate only the metadata field
    # in the packed copy; the source vmlinuz is never modified.
    target = max(len(linux_image), linux_info["image_size"])
    relocated = bytearray(linux_image + b"\x00" * (target - len(linux_image)))
    struct.pack_into("<Q", relocated, 8, stock_info["text_offset"])
    padded = bytes(relocated)
    check("linux:relocated-text-offset",
          le64(padded, 8) == stock_info["text_offset"],
          "text_offset=%#x expected=%#x"
          % (le64(padded, 8), stock_info["text_offset"]))
    check("linux:padded", len(padded) == target, "len=%d" % len(padded))
    linux_reserved = align_up(len(padded), 4096)
    bss_pad = linux_reserved - len(padded)

    check("initrd:nonempty", len(initrd_bytes) > 0, "initrd is empty")
    initrd_format = "unknown"
    if initrd_bytes[:2] == GZIP_MAGIC:
        initrd_format = "gzip"
    elif initrd_bytes[:6] in (b"070701", b"070702"):
        initrd_format = "cpio-newc"

    stock_has_footer = len(stock_boot) >= 64 and stock_boot[-64:-60] == AVB_MAGIC
    check("stock_boot:avb-footer", stock_has_footer,
          "stock boot image has no AVB footer")

    for key in ("packer", "unpacker", "avbtool", "repacker"):
        check("tool:%s" % key, os.path.isfile(inputs[key]), inputs[key])

    source_digests = {key: sha256_file(path) for key, path in inputs.items()}
    source_sizes = {key: os.path.getsize(path) for key, path in inputs.items()}

    if raw_profile == "xbl-evidence":
        raw_budget = {
            "profile": "elish-xbl-256m-evidence",
            "max_raw_bytes": 192 * 1024 * 1024,
            "evidence": (
                "Not a measured elish raw profile. The elish XBL image "
                "contains the memory-map line '0xA0000000, 0x10000000, "
                "\"Kernel\", AddMem, SYS_MEM, SYS_MEM_CAP, Reserv, "
                "WRITE_BACK_XN' (256 MiB).  192 MiB is used here only as a "
                "conservative cap below that reservation; the ABL fallback "
                "PCD RamdiskEndAddress=0x05600000 still means a device that "
                "fails QueryBootParams would be much smaller.  A successful "
                "on-device raw trial is still required before treating this "
                "as a hardware profile."),
        }
    else:
        raw_budget = {
            "profile": "elish-conservative-default",
            "max_raw_bytes": DEFAULT_RAW_BUDGET,
            "evidence": (
                "No elish raw profile has been measured. The elish XBL image "
                "contains a 0xA0000000/0x10000000 'Kernel' reservation, but the "
                "ABL fallback PCD RamdiskEndAddress=0x05600000 is much smaller. "
                "Keep the conservative 128 MiB packed-kernel budget until an "
                "on-device raw trial is recorded."),
        }

    stock_path = os.path.join(WORK, "StockImage")
    with open(stock_path, "wb") as handle:
        handle.write(stock_image)
    padded_path = os.path.join(WORK, "padded-Image")
    with open(padded_path, "wb") as handle:
        handle.write(padded)

    boot_stage = None
    derived_path, derived_bytes, dtb_changed, dtb_info = build_dtb(
        inputs, bootargs, boot_stage=boot_stage, initrd_index=3)
    panel = dtb_info["panel"]
    if expect_panel is not None:
        check("dtb:panel-expected", panel == expect_panel,
              "detected=%s expected=%s" % (panel, expect_panel))

    set_stage("config")
    packed_kernel = os.path.join(OUT, "PackedKernel")
    config_path = os.path.join(WORK, "build.cfg")
    linux_entry_offset = 0
    write_pack_config(config_path, inputs, stock_path, padded_path,
                      derived_path, packed_kernel, linux_entry_offset)

    set_stage("pack")
    run([inputs["packer"], config_path], "packer")
    raw = read_bytes(packed_kernel, "PackedKernel")

    set_stage("assert-pack")
    check("raw:size<=loader-budget", len(raw) <= raw_budget["max_raw_bytes"],
          "len=%d max=%d profile=%s"
          % (len(raw), raw_budget["max_raw_bytes"], raw_budget["profile"]))
    check("raw:arm64-magic", raw[56:60] == ARM64_MAGIC, "bad magic")
    check("raw:image_size==len", le64(raw, 16) == len(raw),
          "image_size=%d len=%d" % (le64(raw, 16), len(raw)))
    check("raw:text_offset-intact", le64(raw, 8) == stock_info["text_offset"],
          "text_offset changed")
    check("raw:flags-intact", le64(raw, 0x18) == le64(stock_image, 0x18),
          "flags changed")
    manifest_offset = le64(raw, 32)
    manifest_size = le64(raw, 40)
    shim_offset = le64(raw, 48)
    check("raw:manifest-bounds",
          manifest_size >= MANIFEST_HEADER_SIZE
          and manifest_offset + manifest_size <= len(raw),
          "off=%d size=%d len=%d" % (manifest_offset, manifest_size, len(raw)))

    manifest = parse_manifest(raw, manifest_offset)
    check("manifest:magic", manifest["magic"] == MANIFEST_MAGIC,
          hex(manifest["magic"]))
    check("manifest:version",
          manifest["version_major"] == 0 and manifest["version_minor"] == 1,
          "%d.%d" % (manifest["version_major"], manifest["version_minor"]))
    check("manifest:count", manifest["count"] == len(EXPECTED_ENTRIES),
          "%d" % manifest["count"])
    check("manifest:default", manifest["default"] == DEFAULT_INDEX,
          "%d" % manifest["default"])
    check("manifest:timeout", manifest["timeout"] == TIMEOUT_MS,
          "%d" % manifest["timeout"])
    check("manifest:end", manifest["end"] == manifest_offset + manifest_size,
          "entry table does not fill manifest")

    expected_backup = (
        le64(stock_image, 0),
        le64(stock_image, 16),
        le64(stock_image, 32),
        le64(stock_image, 40),
        le64(stock_image, 48),
    )
    check("manifest:backup", manifest["backup"] == expected_backup,
          "backup=%r expected=%r" % (manifest["backup"], expected_backup))

    entries = manifest["entries"]
    for index, name, e_type, e_flags, e_align, e_dtb in EXPECTED_ENTRIES:
        entry = entries[index]
        check("entry%d:name" % index,
              entry["name_bytes"].decode("utf-8", "replace") == name,
              entry["name_bytes"].decode("utf-8", "replace"))
        check("entry%d:name_len" % index,
              entry["name_len"] == len(name.encode("utf-8")),
              "%d" % entry["name_len"])
        check("entry%d:padding" % index,
              entry["pad"] == b"\x00" * len(entry["pad"]),
              "non-zero record padding")
        check("entry%d:type" % index, entry["type"] == e_type,
              "%d" % entry["type"])
        check("entry%d:flags" % index, entry["flags"] == e_flags,
              "%d" % entry["flags"])
        check("entry%d:align" % index, entry["alignment"] == e_align,
              "%d" % entry["alignment"])
        check("entry%d:load" % index, entry["load_address"] == 0,
              "%d" % entry["load_address"])
        expected_entry_offset = linux_entry_offset if index == 1 else 0
        check("entry%d:entry_offset" % index,
              entry["entry_offset"] == expected_entry_offset,
              "%d" % entry["entry_offset"])
        check("entry%d:copy_size_max" % index, entry["copy_size_max"] == 0,
              "%d" % entry["copy_size_max"])
        check("entry%d:dtb_index" % index, entry["dtb_index"] == e_dtb,
              "%d" % entry["dtb_index"])

    base = entries[0]
    check("base:offset", base["offset"] == 0, "%d" % base["offset"])
    check("base:size", base["size"] == len(stock_image),
          "%d expected=%d" % (base["size"], len(stock_image)))
    check("base:no-has-dtb", (base["flags"] & FLAG_HAS_DTB) == 0, "HAS_DTB set")
    check("base:no-qc", (base["flags"] & FLAG_QC) == 0, "QC flag set")

    linux = entries[1]
    check("linux:type", linux["type"] == MF_TYPE_LINUX, "%d" % linux["type"])
    check("linux:size==padded", linux["size"] == len(padded),
          "%d expected=%d" % (linux["size"], len(padded)))
    check("linux:has-dtb", (linux["flags"] & FLAG_HAS_DTB) != 0,
          "Linux entry lacks HAS_DTB")

    dtb_entry = entries[2]
    check("dtb:entry-type", dtb_entry["type"] == MF_TYPE_DTB,
          "%d" % dtb_entry["type"])
    check("dtb:entry-size==derived",
          dtb_entry["size"] == len(derived_bytes),
          "%d expected=%d" % (dtb_entry["size"], len(derived_bytes)))

    blob_entry = entries[3]
    check("blob:type", blob_entry["type"] == MF_TYPE_BLOB,
          "%d" % blob_entry["type"])
    check("blob:size==initrd", blob_entry["size"] == len(initrd_bytes),
          "%d expected=%d" % (blob_entry["size"], len(initrd_bytes)))

    shim = entries[4]
    check("shim:type", shim["type"] == MF_TYPE_SHIM, "%d" % shim["type"])
    check("shim:offset==header", shim["offset"] == shim_offset,
          "entry=%d header=%d" % (shim["offset"], shim_offset))
    check("shim:size", shim["size"] == RUNTIME_SIZE, "%d" % shim["size"])
    check("shim:payload==runtime", payload(raw, shim) == runtime,
          "shim payload differs from runtime input")

    restored = bytearray(payload(raw, base))
    for slot, offset in enumerate((0, 16, 32, 40, 48)):
        struct.pack_into("<Q", restored, offset, manifest["backup"][slot])
    check("base:payload==stock_image", bytes(restored) == stock_image,
          "base payload differs from stock image")

    linux_payload = payload(raw, linux)
    check("linux:payload==padded", linux_payload == padded,
          "linux payload differs from padded Image")
    check("linux:prefix==source",
          linux_payload[:8] == linux_image[:8]
          and linux_payload[16:len(linux_image)] == linux_image[16:],
          "linux payload differs from --linux-image outside text_offset")
    check("linux:text-offset-relocated",
          linux_payload[8:16] == stock_image[8:16],
          "linux payload text_offset was not relocated")
    check("linux:zero-pad",
          linux_payload[len(linux_image):]
          == b"\x00" * (len(padded) - len(linux_image)),
          "non-zero Linux tail padding")
    check("dtb:payload==derived", payload(raw, dtb_entry) == derived_bytes,
          "DTB payload differs from derived DTB")
    check("blob:payload==initrd", payload(raw, blob_entry) == initrd_bytes,
          "Blob payload differs from --initrd")

    order = [
        ("base", base),
        ("shim", shim),
        ("linux", linux),
        ("dtb", dtb_entry),
        ("blob", blob_entry),
    ]
    regions = []
    for name, entry in order:
        start = entry["offset"]
        end = start + entry["size"]
        check("layout:%s:align" % name,
              start % (1 << entry["alignment"]) == 0,
              "offset=%d align=2^%d" % (start, entry["alignment"]))
        regions.append((name, start, end, entry["size"]))
    previous_end = 0
    for name, start, end, used in regions:
        check("layout:%s:ordered" % name, start >= previous_end,
              "start=%d previous_end=%d" % (start, previous_end))
        check("layout:%s:in-file" % name, end <= len(raw),
              "end=%d len=%d" % (end, len(raw)))
        if start > previous_end:
            check("layout:%s:gap-zero" % name,
                  raw[previous_end:start] == b"\x00" * (start - previous_end),
                  "non-zero gap before %s" % name)
        previous_end = end
    check("layout:linux:base-text-residue",
          (linux["offset"] % 4096) == (base["offset"] % 4096),
          "linux offset residue differs from base text residue")
    check("layout:blob-after-linux-reserved",
          blob_entry["offset"] >= linux["offset"] + linux_reserved,
          "initrd starts before Linux reserved runtime end")
    check("layout:dtb-after-linux-reserved",
          dtb_entry["offset"] >= linux["offset"] + linux_reserved,
          "DTB starts before Linux reserved runtime end")
    check("layout:linux:bss-pad-zero",
          raw[linux["offset"] + len(padded):
              linux["offset"] + linux_reserved] == b"\x00" * bss_pad,
          "non-zero Linux BSS padding")
    check("layout:manifest-after-payloads", manifest_offset >= previous_end,
          "manifest=%d previous_end=%d" % (manifest_offset, previous_end))
    check("layout:manifest-gap-zero",
          raw[previous_end:manifest_offset]
          == b"\x00" * (manifest_offset - previous_end),
          "non-zero gap before manifest")
    check("layout:ends-at-manifest", manifest_offset + manifest_size == len(raw),
          "trailing bytes after manifest")

    w0 = le32(raw, 0)
    w1 = le32(raw, 4)
    check("patch:w0-target-positive", shim_offset > 0, "%d" % shim_offset)
    check("patch:w0-target-in-range", shim_offset < BL_RANGE, "%d" % shim_offset)
    expected_w0 = 0x94000000 | ((shim_offset >> 2) & 0x03ffffff)
    check("patch:w0", w0 == expected_w0,
          "w0=%08x expected=%08x" % (w0, expected_w0))
    imm = w0 & 0x03ffffff
    if imm & 0x02000000:
        imm -= 0x04000000
    check("patch:w0-target", imm * 4 == shim_offset,
          "target=%d shim=%d" % (imm * 4, shim_offset))
    orig_w0 = le32(stock_image, 0)
    orig_w1 = le32(stock_image, 4)
    if is_branch(orig_w0) and not is_branch(orig_w1):
        expected_w1 = 0x14000000 | ((orig_w0 - 1) & 0x03ffffff)
    else:
        check("patch:orig-w1-is-branch", is_branch(orig_w1), "%08x" % orig_w1)
        expected_w1 = orig_w1
    check("patch:w1", w1 == expected_w1, "w1=%08x expected=%08x"
          % (w1, expected_w1))

    set_stage("unpack")
    unpacked_dir = os.path.join(OUT, "unpacked")
    run([inputs["unpacker"], packed_kernel, unpacked_dir], "unpacker")

    set_stage("assert-unpack")
    restored_base = read_bytes(os.path.join(unpacked_dir, "BaseImage"),
                               "BaseImage")
    check("unpack:base-full", restored_base == stock_image,
          "restored BaseImage differs from stock image")
    restored_shim = read_bytes(os.path.join(unpacked_dir, "Shim.bin"),
                               "Shim.bin")
    check("unpack:shim", restored_shim == runtime, "restored Shim differs")

    emitted = configparser.ConfigParser()
    emitted.optionxform = str
    emitted.read(os.path.join(unpacked_dir, "config.cfg"), encoding="utf-8")
    repacked_name = "RepackedKernel"
    if emitted.has_section("Pack") and emitted["Pack"].get("Output"):
        repacked_name = os.path.basename(emitted["Pack"]["Output"])
    run([inputs["packer"], os.path.join(unpacked_dir, "config.cfg")],
        "repacker-packer")
    repacked = read_bytes(os.path.join(unpacked_dir, repacked_name),
                          "repacked kernel")
    check("repack:byte-identical", repacked == raw,
          "repacked output differs from original packed kernel")

    set_stage("repack")
    final_boot = os.path.join(OUT, output_name)
    repack_report = os.path.join(OUT, "repack_report.json")
    repack_cmd = [
        sys.executable, "-B", inputs["repacker"],
        "--original", inputs["stock_boot"],
        "--kernel", packed_kernel,
        "--output", final_boot,
        "--report", repack_report,
        "--avbtool", inputs["avbtool"],
        "--max-output-size", str(max_output_size),
    ]
    if pad_to_max_output:
        repack_cmd += ["--avb-partition-size", str(max_output_size)]
    else:
        repack_cmd += ["--minimal-avb"]
    run(repack_cmd, "repacker")

    set_stage("assert-boot")
    final = read_bytes(final_boot, "boot.img")
    final_boot_header = parse_boot_header(final)
    check("boot:size<=output-limit", len(final) <= max_output_size,
          "len=%d max=%d" % (len(final), max_output_size))
    if pad_to_max_output:
        check("boot:padded-size", len(final) == max_output_size,
              "len=%d expected=%d" % (len(final), max_output_size))
    check("boot:magic", final[0:8] == BOOT_MAGIC, repr(final[0:8]))
    check("boot:version", final_boot_header["version"] == 3,
          "version=%d" % final_boot_header["version"])
    check("boot:header_size", final_boot_header["header_size"] == 1580,
          "header_size=%d" % final_boot_header["header_size"])
    check("boot:ramdisk_size",
          final_boot_header["ramdisk_size"] == ramdisk_size,
          "ramdisk_size=%d expected=%d"
          % (final_boot_header["ramdisk_size"], ramdisk_size))
    check("boot:header-prefix", final[0:8] == stock_boot[0:8],
          "first 8 bytes changed")
    check("boot:header-body", final[12:4096] == stock_boot[12:4096],
          "header differs beyond kernel_size field")
    final_kernel_size = final_boot_header["kernel_size"]
    check("boot:kernel_size", final_kernel_size == len(raw),
          "kernel_size=%d raw len=%d" % (final_kernel_size, len(raw)))
    check("boot:kernel-bytes",
          final[4096:4096 + final_kernel_size] == raw,
          "embedded kernel is not the raw packed kernel")

    final_ram_off = 4096 + align_up(len(raw), 4096)
    check("boot:ramdisk-bytes",
          final[final_ram_off:final_ram_off + ramdisk_size] == stock_ramdisk,
          "embedded ramdisk differs from the stock ramdisk")
    check("boot:ramdisk-padding",
          final[4096 + len(raw):final_ram_off]
          == b"\x00" * (final_ram_off - 4096 - len(raw)),
          "non-zero kernel padding")

    footer = final[-64:]
    avb_magic, avb_major, avb_minor, ois, vbo, vbs, _reserved = struct.unpack(
        ">4sIIQQQ28s", footer)
    check("avb:footer-magic", avb_magic == AVB_MAGIC, repr(avb_magic))
    check("avb:footer-major", avb_major == 1, "%d" % avb_major)
    expected_ois = align_up(final_ram_off + ramdisk_size, 4096)
    check("avb:original_image_size", ois == expected_ois,
          "ois=%d expected=%d" % (ois, expected_ois))
    check("avb:vbmeta-bounds",
          vbs > 0 and vbo >= ois and vbo + vbs <= len(final) - 64,
          "vbo=%d vbs=%d len=%d" % (vbo, vbs, len(final)))
    check("boot:ramdisk-tail-padding",
          final[final_ram_off + ramdisk_size:ois]
          == b"\x00" * (ois - final_ram_off - ramdisk_size),
          "non-zero ramdisk padding")
    check("avb:minimal-footer",
          pad_to_max_output or len(final) >= ois + 64 * 1024 + 4096,
          "final length %d is below the avbtool metadata reservation"
          % len(final))

    _sf = struct.unpack(">4sIIQQQ28s", stock_boot[-64:])
    stock_orig_size, stock_vbo, stock_vbs = _sf[3], _sf[4], _sf[5]
    check("avb:stock-vbmeta-size", stock_vbs > 0,
          "stock footer vbmeta_size is zero")
    check("avb:stock-vbmeta-bounds",
          stock_vbo >= stock_orig_size
          and stock_vbo + stock_vbs <= len(stock_boot) - 64,
          "stock footer vbmeta region out of bounds")
    check("avb:fresh-metadata",
          final[vbo:vbo + vbs]
          != stock_boot[stock_vbo:stock_vbo + stock_vbs],
          "vbmeta block matches stale stock value")

    set_stage("avb")
    verify_log = os.path.join(LOGS, "avbtool_verify_image.stdout")
    # avbtool's verify_image follows the hash descriptor's partition_name and
    # opens <image_dir>/boot.img when the file is named differently.  Provide a
    # temporary sibling alias so an artifact named boot-csot.img can still be
    # verified by the official tool without renaming the deliverable.
    verify_target = final_boot
    verify_alias = None
    if os.path.basename(final_boot) != "boot.img":
        verify_alias = os.path.join(OUT, "boot.img")
        check("avb:verify-alias-free", not os.path.lexists(verify_alias),
              "refusing to replace %s" % verify_alias)
        os.symlink(os.path.basename(final_boot), verify_alias)
        verify_target = verify_alias
    try:
        run([sys.executable, "-B", inputs["avbtool"], "verify_image",
             "--image", verify_target], "avbtool_verify_image")
    finally:
        if verify_alias is not None and os.path.lexists(verify_alias):
            os.unlink(verify_alias)
    run([sys.executable, "-B", inputs["avbtool"], "info_image",
         "--image", final_boot], "avbtool_info_image")
    info_log = os.path.join(LOGS, "avbtool_info_image.stdout")
    algorithm_match = re.search(r"^\s*Algorithm:\s*(\S+)",
                                stdout_text("avbtool_info_image"), re.M)
    check("avb:algorithm-line", algorithm_match is not None,
          "no anchored Algorithm line in info_image output")
    check("avb:algorithm-none", algorithm_match.group(1) == "NONE",
          "algorithm=%s" % algorithm_match.group(1))

    set_stage("assert-repack-report")
    repack_data = json.loads(read_bytes(repack_report, "repack report")
                             .decode("utf-8"))
    check("repack-report:output-length",
          repack_data.get("output_length") == len(final),
          "%r expected=%d" % (repack_data.get("output_length"), len(final)))
    check("repack-report:partition-limit",
          repack_data.get("partition_limit") == max_output_size,
          "%r expected=%d"
          % (repack_data.get("partition_limit"), max_output_size))
    check("repack-report:ramdisk-preserved",
          repack_data.get("ramdisk_preserved") is True,
          "%r" % repack_data.get("ramdisk_preserved"))
    avb_report = repack_data.get("avb", {})
    check("repack-report:avb-partition-size",
          avb_report.get("partition_size") == len(final),
          "%r expected=%d"
          % (avb_report.get("partition_size"), len(final)))
    check("repack-report:avb-algorithm",
          avb_report.get("metadata_algorithm") == "NONE",
          "%r" % avb_report.get("metadata_algorithm"))

    set_stage("recheck-sources")
    for key, path in inputs.items():
        check("source-unchanged:%s" % key,
              sha256_file(path) == source_digests[key],
              "input changed: %s" % path)

    set_stage("report")
    report_entries = []
    for index, name, e_type, e_flags, e_align, e_dtb in EXPECTED_ENTRIES:
        entry = entries[index]
        report_entries.append({
            "index": index,
            "name": name,
            "type": entry["type"],
            "flags": entry["flags"],
            "offset": entry["offset"],
            "size": entry["size"],
            "entry_offset": entry["entry_offset"],
            "dtb_index": entry["dtb_index"],
            "alignment": entry["alignment"],
            "sha256": sha256_bytes(payload(raw, entry)),
        })

    report = {
        "offline_verified": True,
        "offline_only": True,
        "hardware_tested": False,
        "source_label": source_label,
        "inputs": {
            key: {"path": inputs[key], "length": source_sizes[key],
                  "sha256": source_digests[key]}
            for key in sorted(inputs)
        },
        "board": {
            "device": "xiaomi-elish",
            "compatible": list(dtb_info["compatible"]),
            "panel": panel,
            "panel_expected": expect_panel,
        },
        "bootargs": bootargs,
        "dtb": {
            "source": inputs["dtb"],
            "source_sha256": source_digests["dtb"],
            "derived_length": len(derived_bytes),
            "derived_sha256": sha256_bytes(derived_bytes),
            "panel": panel,
            "boot_stage": boot_stage,
            "changed": [list(item) for item in dtb_changed],
            "dts": dtb_info,
        },
        "stock_image": {
            "length": len(stock_image),
            "sha256": sha256_bytes(stock_image),
            "text_offset": stock_info["text_offset"],
            "image_size": stock_info["image_size"],
            "effective_slot": stock_info["effective_slot"],
        },
        "stock_ramdisk": {
            "offset": ramdisk_off,
            "length": ramdisk_size,
            "sha256": sha256_bytes(stock_ramdisk),
            "preserved": True,
        },
        "linux_image": {
            "source_length": len(linux_image),
            "source_sha256": source_digests["linux_image"],
            "padded_length": len(padded),
            "reserved_length": linux_reserved,
            "bss_pad": bss_pad,
            "text_offset": linux_info["text_offset"],
            "text_offset_patched": le64(padded, 8),
            "entry_offset": linux_entry_offset,
            "image_size": linux_info["image_size"],
        },
        "initrd": {
            "path": inputs["initrd"],
            "length": len(initrd_bytes),
            "sha256": source_digests["initrd"],
            "format": initrd_format,
        },
        "runtime": {
            "length": len(runtime),
            "sha256": source_digests["runtime"],
        },
        "loader_budget": raw_budget,
        "config_path": config_path,
        "default_label": DEFAULT_LABEL,
        "default_index": DEFAULT_INDEX,
        "timeout_ms": TIMEOUT_MS,
        "uncompressed": True,
        "manifest": {
            "magic": hex(manifest["magic"]),
            "version": "%d.%d" % (manifest["version_major"],
                                  manifest["version_minor"]),
            "count": manifest["count"],
            "offset": manifest_offset,
            "size": manifest_size,
            "backup": list(manifest["backup"]),
        },
        "entries": report_entries,
        "layout": [
            {"name": name, "start": start, "end": end, "used": used}
            for name, start, end, used in regions
        ],
        "outputs": {
            "packed_kernel": {"path": packed_kernel, "length": len(raw),
                              "sha256": sha256_bytes(raw)},
            "repacked_kernel": {
                "path": os.path.join(unpacked_dir, repacked_name),
                "sha256": sha256_bytes(repacked)},
            "final_boot": {"path": final_boot, "length": len(final),
                           "sha256": sha256_bytes(final)},
            "repack_report": repack_report,
        },
        "boot": {
            "kernel_size": final_kernel_size,
            "kernel_offset": 4096,
            "ramdisk_offset": final_ram_off,
            "ramdisk_size": ramdisk_size,
            "output_limit": max_output_size,
            "padded_to_limit": pad_to_max_output,
            "avb": {
                "algorithm": algorithm_match.group(1),
                "partition_size": len(final),
                "original_image_size": ois,
                "vbmeta_offset": vbo,
                "vbmeta_size": vbs,
                "verify_image_log": verify_log,
                "info_image_log": info_log,
            },
        },
        "caveats": [
            "Offline byte-level round-trip plus official avbtool success is NOT "
            "hardware validation.",
            "The stock Android ramdisk is preserved byte-for-byte because the "
            "v3 boot header and kShimC base handoff do not carry a separate DT "
            "or initrd address for the Android base.",
            "The Linux Image header text_offset is relocated from 0x0 to the "
            "stock Android base value 0x80000 in the packed copy so the packer "
            "can keep the 2 MiB placement rule; the source vmlinuz is unchanged.",
            "CSOT is the first panel target; BOE is a fallback only after a "
            "CSOT black-screen/no-display result.",
            "The final image uses a minimal AVB footer at the real content end; "
            "the 192 MiB GPT partition size is only an upper limit. "
            "--pad-to-max-output is the fallback if a device requires a "
            "partition-sized file.",
            "Packed-kernel raw size %d bytes passes the selected %s loader "
            "budget (%d bytes); the XBL 0x10000000 'Kernel' reservation is "
            "evidence, not a measured elish raw profile."
            % (len(raw), raw_budget["profile"], raw_budget["max_raw_bytes"]),
            "Unsigned AVB metadata requires an unlocked bootloader; the original "
            "Xiaomi signature cannot be preserved after changing the kernel.",
            "Linux boot, display output, touch, UFS root and the initramfs "
            "canary are NOT validated on hardware.",
        ],
        "commands": COMMANDS,
        "checks": CHECKS,
    }

    with open(os.path.join(OUT, "report.json"), "w", encoding="utf-8") as handle:
        json.dump(report, handle, indent=2, ensure_ascii=False)
        handle.write("\n")

    sys.stdout.write("offline verification PASSED; report at %s\n"
                     % os.path.join(OUT, "report.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
