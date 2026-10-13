# python -I tools\pgm2png.py out.png a.pgm [b.pgm ...]  - stacks the PGMs vertically (scaled x2) into one PNG
import struct
import sys
import zlib

out, files = sys.argv[1], sys.argv[2:]
imgs = []
for name in files:
    d = open(name, 'rb').read()
    parts = d.split(b'\n', 1)
    _, w, h, _ = parts[0].split()
    imgs.append((int(w), int(h), parts[1]))
S = 2
W = max(w for w, _, _ in imgs) * S
rows = []
for w, h, px in imgs:
    for y in range(h):
        line = bytearray()
        for x in range(w):
            line += bytes([px[y * w + x]]) * S
        line += bytes([64]) * (W - len(line))
        for _ in range(S):
            rows.append(bytes(line))
    rows.append(bytes([128]) * W)
raw = b''.join(b'\0' + r for r in rows)


def chunk(t, b):
    return struct.pack('>I', len(b)) + t + b + struct.pack('>I', zlib.crc32(t + b) & 0xffffffff)


png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', W, len(rows), 8, 0, 0, 0, 0)) + \
    chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
open(out, 'wb').write(png)
