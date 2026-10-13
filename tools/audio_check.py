# Audio check for VENDETTA_AUDIODUMP output (raw 16-bit stereo PCM, 48 kHz).
# Prints per 10 s window: L/R level, the lag of R behind L (cross-correlation on a decimated envelope, +-1 s) and the
# number of clicks (sample-to-sample jumps far above the window's typical slope) per channel.
# Usage: python -I audio_check.py F:\hctp-newbase\logs\auto\audio.pcm
import sys, array

RATE = 48000
path = sys.argv[1]
data = array.array('h')
with open(path, 'rb') as f:
    data.frombytes(f.read())
left = data[0::2]
right = data[1::2]
n = len(left)
print(f"{n / RATE:.1f} s of audio")

DEC = 48  # 1 kHz envelope
WIN = 10 * RATE

def envelope(ch, start, count):
    out = []
    for i in range(start, start + count - DEC + 1, DEC):
        out.append(sum(abs(v) for v in ch[i:i + DEC]) / DEC)
    return out

def clicks(ch, start, count):
    diffs = [abs(ch[i + 1] - ch[i]) for i in range(start, start + count - 1)]
    if not diffs:
        return 0
    typical = sorted(diffs)[len(diffs) // 2] + 1
    limit = max(8000, typical * 40)
    return sum(1 for d in diffs if d > limit)

for w in range(0, n - WIN + 1, WIN):
    el = envelope(left, w, WIN)
    er = envelope(right, w, WIN)
    lvl_l = sum(el) / len(el)
    lvl_r = sum(er) / len(er)
    best_lag, best = 0, -1.0
    if lvl_l > 50 and lvl_r > 50:
        ml = lvl_l; mr = lvl_r
        a = [x - ml for x in el]; b = [x - mr for x in er]
        for lag in range(-1000, 1001, 5):  # ms
            s = 0.0
            for i in range(max(0, -lag), min(len(a), len(b) - lag)):
                s += a[i] * b[i + lag]
            if s > best:
                best, best_lag = s, lag
    print(f"t={w // RATE:4d}s  level L={lvl_l:7.0f} R={lvl_r:7.0f}  R lag={best_lag:+5d} ms  "
          f"clicks L={clicks(left, w, WIN):4d} R={clicks(right, w, WIN):4d}")
