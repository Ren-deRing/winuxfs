#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""Host side of the XFS Windows acceptance (M2 design 10.7).

  xfs_vm_acceptance.py prepare OUTDIR [--fixtures DIR] [--image NAME]
      Build OUTDIR/disk.img, a GPT disk for `vmctl.py attach-disk`:
        partition 1  a clean matrix image (default m00-b1024-s512-ddef-plain)
        partition 2  a copy of it with a dirty log (a synthetic log record
                     after mkfs's unmount record, as backends/xfs/tests/
                     test_logcheck.c does)
        partition 3  hole-unwritten.img
        partition 4  perf: one 512 MiB file of incompressible bytes
        partition 5  an empty mkfs image the Windows write test mounts read-write
      and OUTDIR/expected-{main,hole,perf}.json (the fixtures' manifests).
      Needs xfsprogs (mkfs.xfs, xfs_db); exit 77 when missing.
  xfs_vm_acceptance.py verify OUTDIR VMOUT
      Compare the manifests and results.json that tools/xfs-vm-acceptance.ps1
      wrote (VMOUT) with the expected ones; print PASS/FAIL per test name and
      the performance numbers; exit 1 on any FAIL.

Windows sees less than a POSIX manifest records: no mode bits or link
counts, and a symlink or FIFO is a plain file whose size is the target
length (symlink) or 0. The comparison therefore projects the expected
manifest to what is visible (kind dir/file, size, sha256).
"""

import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import uuid
import zlib

MIB = 1 << 20
SECTOR = 512
DEFAULT_IMAGE = "m00-b1024-s512-ddef-plain"
PERF_SIZE = 512 * MIB
LINUX_FS_GUID = uuid.UUID("0FC63DAF-8483-4772-8E79-3D69D8477DE4")


# ------------------------------------------------------------------ GPT ---
def _crc(b):
    return zlib.crc32(b) & 0xFFFFFFFF


def write_gpt_disk(path, images):
    """Write a GPT disk holding `images` (list of file paths), each in its own
    partition at a 1 MiB aligned offset. Zero blocks stay holes."""
    starts, off = [], MIB
    for im in images:
        starts.append(off)
        off += (os.path.getsize(im) + MIB - 1) // MIB * MIB
    total = off + MIB
    nent, esz = 128, 128
    ent = bytearray(nent * esz)
    for i, im in enumerate(images):
        s = starts[i] // SECTOR
        e = s + (os.path.getsize(im) + SECTOR - 1) // SECTOR - 1
        ent[i * esz:(i + 1) * esz] = struct.pack(
            "<16s16sQQQ72s", LINUX_FS_GUID.bytes_le,
            uuid.UUID(int=0x5846530000000000 + i + 1).bytes_le, s, e, 0,
            "xfs{}".format(i + 1).encode("utf-16-le").ljust(72, b"\0"))
    last = total // SECTOR - 1
    ent_sectors = nent * esz // SECTOR

    def header(cur, other, first_ent):
        h = struct.pack("<8sIIIIQQQQ16sQIII", b"EFI PART", 0x10000, 92, 0, 0,
                        cur, other, 34, last - ent_sectors - 1,
                        uuid.UUID(int=0x58465344495300).bytes_le, first_ent,
                        nent, esz, _crc(bytes(ent)))
        h = h[:16] + struct.pack("<I", _crc(h[:16] + b"\0\0\0\0" + h[20:92])) + h[20:]
        return h.ljust(SECTOR, b"\0")

    with open(path, "wb") as f:
        f.truncate(total)
        mbr = bytearray(SECTOR)
        mbr[446:462] = struct.pack("<B3sB3sII", 0, b"\0\2\0", 0xEE, b"\xff\xff\xff", 1,
                                   min(last, 0xFFFFFFFF))
        mbr[510:512] = b"\x55\xaa"
        f.write(mbr)
        f.write(header(1, last, 2))
        f.write(ent)
        f.seek((last - ent_sectors) * SECTOR)
        f.write(ent)
        f.write(header(last, 1, last - ent_sectors))
        for im, st in zip(images, starts):
            copy_sparse(im, f, st)
    return starts


def copy_sparse(src, dst_f, at, chunk=MIB):
    zero = bytes(chunk)
    with open(src, "rb") as s:
        pos = 0
        while True:
            b = s.read(chunk)
            if not b:
                break
            if b != zero[:len(b)]:
                dst_f.seek(at + pos)
                dst_f.write(b)
            pos += len(b)


# ------------------------------------------------------------ dirty log ---
def _xfs_db(img, *cmds):
    argv = ["xfs_db", "-r"]
    for c in cmds:
        argv += ["-c", c]
    return subprocess.run(argv + [img], capture_output=True, text=True, check=True).stdout


_TAB = []


def _crc32c(crc, data):
    if not _TAB:
        for v in range(256):
            c = v
            for _ in range(8):
                c = (c >> 1) ^ (0x82F63B78 if c & 1 else 0)
            _TAB.append(c)
    for x in data:
        crc = _TAB[(crc ^ x) & 0xFF] ^ (crc >> 8)
    return crc


def make_dirty(img):
    """Add a start-transaction record whose tail is the mkfs unmount record,
    so the log is dirty (same layout as test_logcheck.c slog_rec)."""
    f = {}
    for line in _xfs_db(img, "sb 0", "p logstart agblocks agblklog blocksize uuid").splitlines():
        k, _, v = line.partition(" = ")
        f[k.strip()] = v.strip()
    bs, agblklog, agblocks = int(f["blocksize"]), int(f["agblklog"]), int(f["agblocks"])
    fsb = int(f["logstart"])
    logoff = ((fsb >> agblklog) * agblocks + (fsb & ((1 << agblklog) - 1))) * bs
    h_len, blk, cycle = 1024, 2, 1
    nd = h_len // 512
    hdr = bytearray(512)
    pay = bytearray(nd * 512)
    struct.pack_into(">IIBB", pay, 0, 0x1234, h_len - 12, 0x69, 0x01)
    for i in range(12, h_len):
        pay[i] = (blk + i) & 0xFF
    struct.pack_into(">IIII", hdr, 0, 0xFEEDBABE, cycle, 2, h_len)
    struct.pack_into(">QQ", hdr, 16, (cycle << 32) | blk, (1 << 32) | 0)  # lsn, tail (1,0)
    struct.pack_into(">II", hdr, 36, 0xFFFFFFFF, 1)  # prev_block, num_logops
    for i in range(nd):                               # cycle stamping
        hdr[44 + 4 * i:48 + 4 * i] = pay[i * 512:i * 512 + 4]
        pay[i * 512:i * 512 + 4] = struct.pack(">I", cycle)
    struct.pack_into(">I", hdr, 300, 1)               # h_fmt
    hdr[304:320] = uuid.UUID(f["uuid"]).bytes
    struct.pack_into(">I", hdr, 320, 32768)           # h_size
    crc = _crc32c(0xFFFFFFFF, bytes(hdr[:328]).ljust(328, b"\0"))
    crc = ~_crc32c(crc, bytes(pay)) & 0xFFFFFFFF
    hdr[32:36] = struct.pack("<I", crc)
    with open(img, "r+b") as fh:
        fh.seek(logoff + blk * 512)
        fh.write(bytes(hdr) + bytes(pay))


# ------------------------------------------------------------- prepare ---
def make_perf(path, tmp):
    src = os.path.join(tmp, "perf-src")
    shutil.rmtree(src, ignore_errors=True)
    os.makedirs(src)
    h = hashlib.sha256()
    with open(os.path.join(src, "big.bin"), "wb") as f:
        for i in range(PERF_SIZE // MIB):
            b = hashlib.sha256(b"perf%d" % i).digest() * (MIB // 32)
            f.write(b)
            h.update(b)
    if os.path.exists(path):
        os.unlink(path)
    subprocess.run(["truncate", "-s", "768m", path], check=True)
    subprocess.run(["mkfs.xfs", "-q", "-f", "-m", "crc=1", "-b", "size=4096", "-p", src, path],
                   check=True, stderr=subprocess.DEVNULL)
    shutil.rmtree(src)
    return {"version": 1, "entries": {
        "/": {"kind": "dir"},
        "/big.bin": {"kind": "file", "size": PERF_SIZE, "sha256": h.hexdigest()}}}


def cmd_prepare(argv):
    import argparse
    ap = argparse.ArgumentParser(prog="prepare")
    ap.add_argument("outdir")
    ap.add_argument("--fixtures", default=os.environ.get("XFS_FIXTURES", "target/xfs-fixtures"))
    ap.add_argument("--image", default=DEFAULT_IMAGE)
    ns = ap.parse_args(argv)
    for t in ("mkfs.xfs", "xfs_db", "truncate"):
        if not shutil.which(t):
            print(f"SKIP: {t} missing (install xfsprogs)", file=sys.stderr)
            return 77
    main_img = os.path.join(ns.fixtures, ns.image + ".img")
    hole_img = os.path.join(ns.fixtures, "hole-unwritten.img")
    for p in (main_img, hole_img, main_img[:-4] + ".manifest.json"):
        if not os.path.exists(p):
            print(f"missing fixture {p}: run tools/gen-fixtures.sh --out {ns.fixtures}",
                  file=sys.stderr)
            return 1
    out = ns.outdir
    os.makedirs(out, exist_ok=True)
    dirty = os.path.join(out, "dirty.img")
    shutil.copyfile(main_img, dirty)
    make_dirty(dirty)
    perf = os.path.join(out, "perf.img")
    perf_manifest = make_perf(perf, out)
    shutil.copyfile(main_img[:-4] + ".manifest.json", os.path.join(out, "expected-main.json"))
    shutil.copyfile(hole_img[:-4] + ".manifest.json", os.path.join(out, "expected-hole.json"))
    with open(os.path.join(out, "expected-perf.json"), "w") as f:
        json.dump(perf_manifest, f, sort_keys=True, indent=1)
    wimg = os.path.join(out, "write.img")
    if os.path.exists(wimg):
        os.unlink(wimg)
    if subprocess.run(["truncate", "-s", "320m", wimg]).returncode or subprocess.run(
            ["mkfs.xfs", "-q", "-f", "-m", "crc=1,rmapbt=0,reflink=0,metadir=0", "-n", "parent=0",
             "-i", "sparse=1", wimg]).returncode:
        print("mkfs.xfs for the write image failed", file=sys.stderr)
        return 1
    starts = write_gpt_disk(os.path.join(out, "disk.img"), [main_img, dirty, hole_img, perf, wimg])
    with open(os.path.join(out, "write-part.json"), "w") as f:
        json.dump({"offset": starts[4], "size": os.path.getsize(wimg)}, f)
    os.unlink(dirty)
    os.unlink(perf)
    os.unlink(wimg)
    print(os.path.join(out, "disk.img"))
    return 0


# -------------------------------------------------------------- verify ---
def visible(expected):
    """Project a POSIX manifest to what Windows shows."""
    out = {}
    for p, e in expected["entries"].items():
        k = e["kind"]
        if k == "dir":
            out[p] = {"kind": "dir"}
        elif k == "file":
            out[p] = {"kind": "file", "size": e["size"], "sha256": e["sha256"]}
        elif k == "symlink":
            out[p] = {"kind": "file", "size": e["size"], "symlink": True}
        else:  # fifo, devices, sockets: only required to exist
            out[p] = {"kind": "file", "size": 0, "special": k}
    return out


def compare(exp, got):
    """Differences between the projected expected and the VM manifest."""
    lines = []
    ev, gv = visible(exp), got["entries"]
    for p in sorted(set(ev) | set(gv)):
        if p not in gv:
            lines.append(f"{p}: missing on Windows")
            continue
        if p not in ev:
            lines.append(f"{p}: unexpected on Windows")
            continue
        e, g = ev[p], gv[p]
        if e.get("special"):
            continue
        if e["kind"] != g["kind"]:
            lines.append(f"{p}: kind {e['kind']} != {g['kind']}")
        elif e["kind"] == "file" and e["size"] != g["size"]:
            lines.append(f"{p}: size {e['size']} != {g['size']}")
        elif (e["kind"] == "file" and not e.get("symlink")
              and e["sha256"] != g["sha256"]):
            lines.append(f"{p}: sha256 {e['sha256']} != {g['sha256']}")
    return lines


def cmd_verify(argv):
    data_disk = None
    if len(argv) == 4 and argv[2] == "--data-disk":
        data_disk = argv[3]
        argv = argv[:2]
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    exp_dir, vm = argv

    def load(p):
        with open(p, encoding="utf-8-sig") as f:
            return json.load(f)

    res = load(os.path.join(vm, "results.json"))
    exp = {k: load(os.path.join(exp_dir, f"expected-{k}.json")) for k in ("main", "hole", "perf")}
    got = {k: load(os.path.join(vm, f"manifest-{k}.json"))
           for k in ("main", "dirty", "hole", "perf")
           if os.path.exists(os.path.join(vm, f"manifest-{k}.json"))}
    failed = []

    def check(name, ok, detail=""):
        print(("PASS " if ok else "FAIL ") + name + ((": " + detail) if detail and not ok else ""))
        if not ok:
            failed.append(name)

    def diff(k, e):
        return compare(exp[e], got[k]) if k in got else [f"no {k} manifest"]

    d_main, d_dirty = diff("main", "main"), diff("dirty", "main")
    d_hole, d_perf = diff("hole", "hole"), diff("perf", "perf")
    ents = exp["main"]["entries"]
    # Reading a symlink's content is an error on Windows (documented limit):
    # only hash errors on anything else count.
    errs = res.get("main_hash_errors") or []
    errs = [errs] if isinstance(errs, str) else errs
    errs = [e for e in errs if ents.get(e.split(" : ")[0], {}).get("kind") != "symlink"]
    check("xfs_tree_matches_manifest_on_windows",
          not d_main and not d_hole and not d_perf and not errs,
          "; ".join((d_main + d_hole + d_perf + errs)[:10]))
    names = [p for p in ents if p.startswith("/names/")]
    gm = got.get("main", {"entries": {}})["entries"]
    miss = [p for p in names if p not in gm
            or (ents[p]["kind"] == "file" and gm[p].get("sha256") != ents[p].get("sha256"))]
    check("korean_and_nonascii_names_visible",
          len(names) >= 7 and not miss
          and any(re.search("[가-힣]", p) for p in gm)
          and any("\udce9" in p for p in gm),
          f"{len(names)} names, bad: {miss[:5]}")
    big = "/sparse/big_gt4g.bin"
    check("file_over_4gib_hash_matches",
          big in gm and gm[big].get("size", 0) > 4 << 30
          and gm[big].get("sha256") == ents[big]["sha256"], str(gm.get(big)))
    n = sum(1 for p in ents if re.fullmatch(r"/many/[^/]+", p))
    check("dir_3000_entries_enumerates",
          n == 3000 and res.get("many_count") == 3000 and res.get("many_unique") == 3000
          and res.get("many_get_childitem_count") == 3000,
          f"count={res.get('many_count')} unique={res.get('many_unique')}")
    sy = res.get("symlinks") or {}
    sym_ok = len(sy) >= 3
    for p, e in ents.items():
        if e["kind"] == "symlink" and p.count("/") == 1:
            s = sy.get(p[1:], {})
            if s.get("length") != e["size"] or "error" in s:
                sym_ok = False
    check("symlink_reported", sym_ok, json.dumps(sy)[:300])
    msg = res.get("dirty_message") or ""
    check("dirty_image_refused_with_message",
          res.get("dirty_mounted") is False and res.get("dirty_exit") not in (0, None)
          and "norecovery" in msg and "log" in msg.lower(), msg[:300])
    check("norecovery_option_mounts", res.get("norecovery_mounted") is True and not d_dirty,
          "; ".join(d_dirty[:10]) or str(res.get("norecovery_stderr")))
    wa = res.get("write_attempts") or {}
    check("readonly_attribute_and_write_refused",
          bool(res.get("main_mounted")) and bool(wa)
          and all(v != "ALLOWED" for v in wa.values())
          and res.get("tiny_unchanged") is True
          and bool(res.get("volume_read_only") or res.get("attr_readonly_file_is_readonly")),
          json.dumps(wa)[:300])
    def flat(x):                # PowerShell wraps arrays: "", [], [[]], "msg", ["msg"]
        if isinstance(x, (list, tuple)):
            return [y for e in x for y in flat(e)]
        return [str(x)] if x else []

    wl, wr_ = flat(res.get("write_live_errors")), flat(res.get("write_remount_errors"))
    check("windows_write_roundtrip",
          res.get("write_mounted") is True and res.get("write_remounted") is True
          and not res.get("write_exception") and len(res.get("write_stages") or []) == 6
          and res.get("write_volume_read_only") is False and not wl and not wr_,
          "; ".join(wl[:5] + wr_[:5]) or str(res.get("write_exception")) or str(res.get("write_stages")))
    if data_disk:
        wp = load(os.path.join(exp_dir, "write-part.json"))
        part = os.path.join(exp_dir, "write-after.img")
        with open(data_disk, "rb") as src, open(part, "wb") as dst:
            src.seek(wp["offset"])
            dst.truncate(wp["size"])
            left, pos = wp["size"], 0
            while left:
                b = src.read(min(MIB, left))
                if not b:
                    break
                if b.count(0) != len(b):
                    dst.seek(pos)
                    dst.write(b)
                pos += len(b)
                left -= len(b)
        r = subprocess.run(["xfs_repair", "-n", part], capture_output=True, text=True)
        check("xfs_repair_clean_after_windows_writes", r.returncode == 0,
              (r.stdout + r.stderr)[-400:])
        os.unlink(part)
    keys = ("first_mount_ms", "mount_ms", "first_root_list_ms", "many_enum_ms", "many_get_childitem_ms",
            "many_enum_warm_ms", "seqread_mib_s", "write_mib_s", "main_walk_ms", "main_entries")
    print("PERF " + " ".join(f"{k}={res.get(k)}" for k in keys))
    if failed:
        print("FAILED: " + ", ".join(failed), file=sys.stderr)
        return 1
    return 0


def main(argv):
    if len(argv) >= 2 and argv[1] == "prepare":
        return cmd_prepare(argv[2:])
    if len(argv) >= 2 and argv[1] == "verify":
        return cmd_verify(argv[2:])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
