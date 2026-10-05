"""Decode a PT-P710BT raster stream back into PNGs (one per page) and print the command log.

Usage: python3 decode.py job.bin out_prefix
Pin 0 (byte 0 MSB) is drawn at the top, feed direction left to right, i.e. how the
label reads when it comes out of the printer.
"""
import sys
from PIL import Image

data = open(sys.argv[1], "rb").read()
prefix = sys.argv[2]
i, page, lines, log = 0, 0, [], []


def unpack(b):
    out, j = bytearray(), 0
    while j < len(b):
        n = b[j]; j += 1
        if n < 128:
            out += b[j:j + n + 1]; j += n + 1
        else:
            out += bytes([b[j]]) * (257 - n); j += 1
    return bytes(out)


def flush(end):
    global page, lines
    img = Image.new("1", (max(len(lines), 1), 128), 1)
    px = img.load()
    for x, line in enumerate(lines):
        for p in range(128):
            if line[p // 8] & (0x80 >> (p % 8)):
                px[x, p] = 0
    name = f"{prefix}-p{page + 1}.png"
    img.resize((img.width * 4, 128 * 4), Image.NEAREST).save(name)
    log.append(f"page {page + 1}: {len(lines)} raster lines ({len(lines) / 180 * 25.4:.1f} mm) end={end} -> {name}")
    page += 1
    lines = []


while i < len(data):
    c = data[i]
    if c == 0x00:
        n = 0
        while i < len(data) and data[i] == 0: i += 1; n += 1
        log.append(f"invalidate x{n}"); continue
    if c == 0x1B:
        cmd = data[i + 1:i + 3]
        if cmd[:1] == b"@": log.append("ESC @ init"); i += 2; continue
        if cmd == b"iS": log.append("ESC i S status request"); i += 3; continue
        if cmd == b"ia": log.append(f"ESC i a {data[i+3]:02x} (raster mode)"); i += 4; continue
        if cmd == b"iz":
            a = data[i + 3:i + 13]
            log.append(f"ESC i z flags={a[0]:02x} type={a[1]:02x} width={a[2]}mm rasters={int.from_bytes(a[4:8],'little')} page={'first' if a[8]==0 else 'next'}")
            i += 13; continue
        if cmd == b"iM": log.append(f"ESC i M {data[i+3]:02x} (auto cut={'on' if data[i+3]&0x40 else 'off'})"); i += 4; continue
        if cmd == b"iK": log.append(f"ESC i K {data[i+3]:02x} (chain={'off' if data[i+3]&0x08 else 'on'})"); i += 4; continue
        if cmd == b"id": log.append(f"ESC i d margin={data[i+3] + data[i+4]*256} dots"); i += 5; continue
        raise SystemExit(f"unknown ESC at {i}: {data[i:i+6].hex()}")
    if c == 0x4D: log.append(f"M {data[i+1]:02x} (compression)"); i += 2; continue
    if c == 0x5A: lines.append(bytes(16)); i += 1; continue
    if c == 0x47:
        n = data[i + 1] + data[i + 2] * 256
        line = unpack(data[i + 3:i + 3 + n])
        assert len(line) == 16, f"line at {i} unpacked to {len(line)} bytes"
        lines.append(line); i += 3 + n; continue
    if c == 0x0C: flush("FF"); i += 1; continue
    if c == 0x1A: flush("Ctrl-Z"); i += 1; continue
    raise SystemExit(f"unknown byte {c:02x} at {i}")

print("\n".join(log))
