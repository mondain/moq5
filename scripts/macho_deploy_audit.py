#!/usr/bin/env python3
"""Read-only Mach-O deployment-target audit.

  macho_deploy_audit.py archives --max-macos 11.0 LIB.a [LIB.a ...]
      Every member occurrence of every static archive (each architecture
      of a universal archive) is read in place: duplicate member names are
      kept as separate occurrences, nothing is extracted. Each member is
      classified as
        meta     the archive symbol table (__.SYMDEF*, or a GNU "/" "//")
        object   a Mach-O object with LC_BUILD_VERSION or
                 LC_VERSION_MIN_MACOSX
        noversion  a Mach-O object with neither command
        other    anything that is not Mach-O (LLVM bitcode, text, ...)
      An object passes when its platform is macOS and its minimum OS is at
      or below --max-macos. noversion and other members fail. The member
      count is cross-checked against `ar -t`.

  macho_deploy_audit.py binaries PATH=VERSION [PATH=VERSION ...]
      Each linked Mach-O (executable, bundle binary, dylib) must declare
      macOS with exactly that minimum OS, in every architecture.

Prints one tab-separated row per member or binary, then a summary.
Exit status 0 only when every row passes.
"""
import struct
import subprocess
import sys

LC_VERSION_MIN_MACOSX = 0x24
LC_BUILD_VERSION = 0x32
PLATFORM_MACOS = 1
PLATFORMS = {1: "macos", 2: "ios", 3: "tvos", 4: "watchos", 5: "bridgeos",
             6: "maccatalyst", 7: "ios-simulator", 8: "tvos-simulator",
             9: "watchos-simulator", 10: "driverkit", 11: "visionos",
             12: "visionos-simulator"}
CPU = {0x01000007: "x86_64", 0x0100000C: "arm64", 0x0200000C: "arm64_32",
       7: "i386", 12: "arm"}

MH_MAGIC = 0xFEEDFACE
MH_MAGIC_64 = 0xFEEDFACF
FAT_MAGIC = 0xCAFEBABE
FAT_MAGIC_64 = 0xCAFEBABF


def fmt_version(v):
    return "%d.%d.%d" % (v >> 16, (v >> 8) & 0xFF, v & 0xFF)


def parse_version(text):
    parts = [int(p) for p in text.split(".")] + [0, 0]
    return (parts[0] << 16) | (parts[1] << 8) | parts[2]


def macho_version(data):
    """(arch, platform, minos) for a thin Mach-O image, or None if not
    Mach-O. platform/minos are None when no version command exists."""
    if len(data) < 28:
        return None
    for endian in ("<", ">"):
        magic = struct.unpack(endian + "I", data[:4])[0]
        if magic in (MH_MAGIC, MH_MAGIC_64):
            break
    else:
        return None
    is64 = magic == MH_MAGIC_64
    cputype, _, _, ncmds, _sizeofcmds, _ = struct.unpack(
        endian + "iiIIII", data[4:28])
    off = 32 if is64 else 28
    arch = CPU.get(cputype & 0xFFFFFFFF, "cpu%#x" % (cputype & 0xFFFFFFFF))
    for _ in range(ncmds):
        if off + 8 > len(data):
            break
        cmd, cmdsize = struct.unpack(endian + "II", data[off:off + 8])
        if cmd == LC_BUILD_VERSION:
            platform, minos = struct.unpack(endian + "II",
                                            data[off + 8:off + 16])
            return arch, platform, minos
        if cmd == LC_VERSION_MIN_MACOSX:
            (minos,) = struct.unpack(endian + "I", data[off + 8:off + 12])
            return arch, PLATFORM_MACOS, minos
        if cmdsize == 0:
            break
        off += cmdsize
    return arch, None, None


def fat_slices(data):
    """[(offset, size)] for a universal file, or None if thin."""
    if len(data) < 8:
        return None
    magic, nfat = struct.unpack(">II", data[:8])
    if magic == FAT_MAGIC:
        step, fmt = 20, ">iiIII"
    elif magic == FAT_MAGIC_64:
        step, fmt = 32, ">iiQQII"
    else:
        return None
    out = []
    for i in range(nfat):
        fields = struct.unpack(fmt, data[8 + i * step:8 + (i + 1) * step])
        out.append((fields[2], fields[3]))
    return out


def ar_members(data):
    """Every member occurrence of a BSD/GNU ar image: [(name, bytes)]."""
    if not data.startswith(b"!<arch>\n"):
        raise ValueError("not an ar archive")
    off, out = 8, []
    while off + 60 <= len(data):
        hdr = data[off:off + 60]
        if hdr[58:60] != b"`\n":
            raise ValueError("bad member header at %d" % off)
        name = hdr[:16].decode("ascii", "replace").rstrip()
        size = int(hdr[48:58].decode("ascii").strip())
        body = data[off + 60:off + 60 + size]
        if name.startswith("#1/"):
            n = int(name[3:])
            name = body[:n].rstrip(b"\0").decode("utf-8", "replace")
            body = body[n:]
        elif name.endswith("/") and name not in ("/", "//"):
            name = name[:-1]
        out.append((name, body))
        off += 60 + size + (size & 1)
    return out


def is_meta(name):
    return name.startswith("__.SYMDEF") or name in ("/", "//")


def ar_t_count(path):
    res = subprocess.run(["ar", "-t", path], capture_output=True, text=True)
    print("ar -t %s: status=%d" % (path, res.returncode), file=sys.stderr)
    print(res.stdout, end="", file=sys.stderr)
    print(res.stderr, end="", file=sys.stderr)
    # ar -t has no expected progress diagnostics. Keep its entire output,
    # and refuse even a successful listing accompanied by a diagnostic.
    if res.returncode != 0 or res.stderr:
        return None
    return len([l for l in res.stdout.splitlines() if l.strip()])


def audit_archives(max_text, paths):
    limit = parse_version(max_text)
    rows, failures = [], 0
    counts = {"meta": 0, "object": 0, "noversion": 0, "other": 0}
    for path in paths:
        with open(path, "rb") as f:
            data = f.read()
        slices = fat_slices(data)
        images = [data[o:o + s] for o, s in slices] if slices else [data]
        parsed = 0
        for img in images:
            members = ar_members(img)
            parsed += len(members)
            for idx, (name, body) in enumerate(members):
                if is_meta(name):
                    counts["meta"] += 1
                    rows.append((path, idx, name, "-", "meta", "-", "-", "ok"))
                    continue
                v = macho_version(body)
                if v is None:
                    counts["other"] += 1
                    failures += 1
                    rows.append((path, idx, name, "-", "other", "-", "-",
                                 "FAIL not Mach-O"))
                    continue
                arch, platform, minos = v
                if platform is None:
                    counts["noversion"] += 1
                    failures += 1
                    rows.append((path, idx, name, arch, "noversion", "-", "-",
                                 "FAIL no version command"))
                    continue
                counts["object"] += 1
                pname = PLATFORMS.get(platform, str(platform))
                ok = platform == PLATFORM_MACOS and minos <= limit
                if not ok:
                    failures += 1
                rows.append((path, idx, name, arch, "object", pname,
                             fmt_version(minos),
                             "ok" if ok else "FAIL above %s" % max_text
                             if platform == PLATFORM_MACOS else
                             "FAIL platform"))
        if not slices:
            listed = ar_t_count(path)
            if listed is None or listed != parsed:
                failures += 1
                rows.append((path, "-", "-", "-", "count", "-", "-",
                             "FAIL parsed %d members, ar -t lists %s"
                             % (parsed, listed)))
    for r in rows:
        print("\t".join(str(x) for x in r))
    print("summary: archives=%d meta=%d object=%d noversion=%d other=%d "
          "failures=%d (max macOS %s)"
          % (len(paths), counts["meta"], counts["object"],
             counts["noversion"], counts["other"], failures, max_text))
    return failures == 0


def audit_binaries(specs):
    failures = 0
    for spec in specs:
        path, _, want = spec.rpartition("=")
        with open(path, "rb") as f:
            data = f.read()
        slices = fat_slices(data)
        images = [data[o:o + s] for o, s in slices] if slices else [data]
        for img in images:
            v = macho_version(img)
            if v is None or v[1] is None:
                failures += 1
                print("%s\t-\t-\t-\tFAIL no version" % path)
                continue
            arch, platform, minos = v
            ok = platform == PLATFORM_MACOS and minos == parse_version(want)
            if not ok:
                failures += 1
            print("%s\t%s\t%s\t%s\t%s" % (
                path, arch, PLATFORMS.get(platform, platform),
                fmt_version(minos),
                "ok" if ok else "FAIL expected %s" % want))
    print("summary: binaries=%d failures=%d" % (len(specs), failures))
    return failures == 0


def main(argv):
    if len(argv) >= 4 and argv[1] == "archives" and argv[2] == "--max-macos":
        return 0 if audit_archives(argv[3], argv[4:]) else 1
    if len(argv) >= 3 and argv[1] == "binaries":
        return 0 if audit_binaries(argv[2:]) else 1
    sys.stderr.write(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
