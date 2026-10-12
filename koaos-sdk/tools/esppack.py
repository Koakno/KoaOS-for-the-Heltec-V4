#!/usr/bin/env python3
"""esppack -- build and inspect KoaOS .esp packages (format v1, see esp_pkg.h).

  esppack.py pack <dir> [-o out.esp]   dir holds a file named 'manifest' plus the app's files (flat)
  esppack.py info <file.esp>           verify and show what's inside

manifest example:
  name=hellopkg
  version=1.0
  abi=1
  type=script
  entry=main.sh
  requires=gps,lora
"""
import hashlib, os, re, struct, sys

ABI = 2
CAPS = {"wifi", "gps", "lora", "display", "gpio", "uart", "i2c", "spi"}
NAME_RE = re.compile(r"^[A-Za-z0-9_.-]+$")


def die(msg):
    print(f"esppack: {msg}", file=sys.stderr)
    sys.exit(1)


def parse_manifest(text):
    m = {}
    for raw in text.splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        if "=" not in line:
            die(f"bad manifest line: {line!r}")
        k, v = line.split("=", 1)
        m[k.strip()] = v.strip()
    return m


def check_manifest(m, files):
    name = m.get("name", "")
    if not name or name[0] == "." or len(name) > 24 or not NAME_RE.match(name):
        die("manifest: name must be 1-24 chars of letters, digits, _ - . (not starting with '.')")
    if "abi" not in m or not m["abi"].isdigit() or int(m["abi"]) < 1:
        die("manifest: missing abi")
    if int(m["abi"]) > ABI:
        die(f"manifest: abi {m['abi']} is newer than this tool knows ({ABI})")
    if m.get("type") not in ("script", "wasm"):
        die("manifest: type must be script or wasm")
    if m.get("entry") not in files:
        die("manifest: entry file is not in the package")
    for c in filter(None, (x.strip() for x in m.get("requires", "").split(","))):
        if c not in CAPS:
            die(f"manifest: unsupported capability '{c}' (known: {', '.join(sorted(CAPS))})")


def pack(src, out):
    mp = os.path.join(src, "manifest")
    if not os.path.isfile(mp):
        die(f"{src}: no file named 'manifest'")
    manifest = open(mp, "rb").read()
    if not (0 < len(manifest) <= 512):
        die("manifest must be 1..512 bytes")
    names = sorted(f for f in os.listdir(src)
                   if f != "manifest" and not f.startswith(".") and os.path.isfile(os.path.join(src, f)))
    if not names:
        die("package has no files besides the manifest")
    if len(names) > 16:
        die("at most 16 files per package")
    for n in names:
        if len(n) > 31 or not NAME_RE.match(n):
            die(f"bad file name {n!r} (max 31 chars of letters, digits, _ - .)")
    check_manifest(parse_manifest(manifest.decode()), names)

    body = b"ESP1" + struct.pack("<HH", len(manifest), len(names)) + manifest
    data = b""
    for n in names:
        content = open(os.path.join(src, n), "rb").read()
        body += struct.pack("B", len(n)) + n.encode() + struct.pack("<I", len(content))
        data += content
    body += data
    body += hashlib.sha256(body).digest()
    if out is None:
        out = parse_manifest(manifest.decode())["name"] + ".esp"
    open(out, "wb").write(body)
    print(f"wrote {out}  ({len(body)} bytes, {len(names)} files)")


def info(path):
    b = open(path, "rb").read()
    if len(b) < 40 or b[:4] != b"ESP1":
        die("not an .esp package")
    if hashlib.sha256(b[:-32]).digest() != b[-32:]:
        die("hash mismatch: package is corrupt")
    mlen, nf = struct.unpack("<HH", b[4:8])
    manifest = b[8:8 + mlen].decode()
    print(manifest.rstrip())
    pos = 8 + mlen
    table = []
    for _ in range(nf):
        nl = b[pos]
        name = b[pos + 1:pos + 1 + nl].decode()
        size = struct.unpack("<I", b[pos + 1 + nl:pos + 5 + nl])[0]
        table.append((name, size))
        pos += 5 + nl
    print("--")
    for n, s in table:
        print(f"{s:8d}  {n}")
    print(f"sha256 ok  {b[-32:].hex()[:16]}...")


def main():
    a = sys.argv[1:]
    if len(a) >= 2 and a[0] == "pack":
        out = a[a.index("-o") + 1] if "-o" in a else None
        pack(a[1], out)
    elif len(a) == 2 and a[0] == "info":
        info(a[1])
    else:
        print(__doc__)
        sys.exit(1)


main()
