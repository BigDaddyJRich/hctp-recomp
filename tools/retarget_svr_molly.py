"""Retarget SvR Molly Holly (4100 ring / 4103 B&P) onto HCTP Molly's own 137-bone skeleton.

Research this is based on (Nov 5 2026):
- YOBJ spec (F:\\YOBJ_FORMAT_SPEC_updated (1).md): bone record 80 B (name, local pos @0x10, local rot xyz radians
  @0x20, parent @0x30); FK is row-vector, local first: world_rot = R_local * R_parent, world_pos = parent_pos +
  pos * R_parent. Bone refs in influence groups are 1-based. Cross-file mesh import = remap refs BY NAME, fold to
  the nearest ancestor the target has (spec s6, same rule as the Bret tools and SvR2011 ps2_char).
- Vertices are model space; the game skins with world * invbind (CSkinModel::FastUpdate 0x151160 -> MulMatrixN2
  0x1510c0, PROGRESS_NOTES Oct 26), invbind from the model's own skeleton.
- HCTP body motions REPLACE the bind rotations (hctp_motion / finger_retarget notes, SvR2011 Mod Maker tools), so a
  bone whose bind rotation differs from HCTP's convention (SvR face bones are (-90,90,0), HCTP's are 0) gets twisted,
  and SvR's extra thigh twist bone momo_x between momo and sune breaks HCTP's leg chain / foot IK.
Method: target = HCTP Molly's skeleton (names, order, parents, ROTATIONS, extra fields kept). Each target bone that
SvR also has is moved so its world position = SvR's world position (local pos solved through HCTP's parent
rotations); other target bones keep their local offsets. SvR refs -> same name, alias, or nearest mapped ancestor.
Mesh/vertex/weight bytes are untouched except the 4 ref ints of every influence group.

usage: python retarget_svr_molly.py [--keep-extra] <svr pac in> <hctp skeleton donor pac> <out pac>
(each YOBJ part takes the donor's part with the same id, else donor part 2: 4100 <- HCTP Molly 4100,
 4103 B&P <- stock HCTP B&P 2703 whose pieces 20/21 carry their own body-subset skeletons)
"""
import math, struct, sys
from pathlib import Path

sys.path.insert(0, r"F:\hctp-project\.claude\worktrees\reverent-euclid-9d5ef2\tools")
from port_svr_molly import pac_read, pac_write, decode_pof0, encode_pof0, bone_records  # noqa: E402
sys.path.insert(0, r"F:\hctp-project\tools")
import hctp_yobj_lowlevel  # noqa: E402
YOBJ = hctp_yobj_lowlevel.load_namespace(r"F:\HCTP MODS\Mods\HCTP_YOBJ_Importer_Exporter.py")["YObjFull"]

# SvR bones HCTP lacks but with a clear HCTP counterpart (else: nearest mapped ancestor)
ALIAS = {
    'r_momo_x': 'r_momo', 'l_momo_x': 'l_momo',
    'l_mayu_00': 'l_mayu', 'l_mayu_01': 'l_mayu', 'l_mayu_02': 'l_mayu',
    'r_mayu_00': 'r_mayu', 'r_mayu_01': 'r_mayu', 'r_mayu_02': 'r_mayu',
    'l_u_kuchi': 'u_kuchi', 'r_u_kuchi': 'u_kuchi', 'u_kuchi_00': 'u_kuchi',
    'd_kuchi_00': 'd_kuchi', 'd_kuchi_01': 'd_kuchi',
}
for i in range(6):
    ALIAS[f'sita_{i:02d}'] = 'd_kuchi'


def rot(rx, ry, rz):
    """row-vector per-axis matrices (transpose of column form), X then Y then Z."""
    def mx(a):
        c, s = math.cos(a), math.sin(a); return [[1, 0, 0], [0, c, s], [0, -s, c]]
    def my(a):
        c, s = math.cos(a), math.sin(a); return [[c, 0, -s], [0, 1, 0], [s, 0, c]]
    def mz(a):
        c, s = math.cos(a), math.sin(a); return [[c, s, 0], [-s, c, 0], [0, 0, 1]]
    return mul(mul(mx(rx), my(ry)), mz(rz))


def mul(a, b):
    return [[sum(a[i][k] * b[k][j] for k in range(3)) for j in range(3)] for i in range(3)]


def vm(v, m):
    return [sum(v[k] * m[k][j] for k in range(3)) for j in range(3)]


def fk(recs):
    W, P = [], []
    for r in recs:
        pos = struct.unpack_from("<3f", r, 16); rx, ry, rz = struct.unpack_from("<3f", r, 32)
        par = struct.unpack_from("<i", r, 48)[0]; R = rot(rx, ry, rz)
        if par < 0: W.append(R); P.append(list(pos))
        else:
            W.append(mul(R, W[par])); P.append([P[par][j] + vm(pos, W[par])[j] for j in range(3)])
    return W, P


# --keep-extra: SvR face bones the runner ANY-BONE prelude drives (hctp_overrides.cpp abRule) are KEPT as their own
# bones (appended, parented under the mapped HCTP bone, world bind position = SvR's, HCTP zero-rotation convention)
# instead of folding into d_kuchi/u_kuchi/mayu. momo_x/kubi_x/kote_x2/kami stay folded (IK + CKami need HCTP joints).
def keep_extra_name(n):
    side = n[:2] in ('l_', 'r_')
    return (n.startswith('sita_0') or n.startswith('d_kuchi_0') or n.startswith('u_kuchi_0')
            or n in ('l_u_kuchi', 'r_u_kuchi', 'l_hoho', 'r_hoho', 'l_meziri', 'r_meziri', 'hana')
            or (side and n[2:].startswith('mayu_0')))


KEEP_EXTRA = False


def retarget_yobj(y, donor):
    y = bytearray(y)
    bt, srecs, snames = bone_records(y)
    _, trecs, tnames = bone_records(donor)
    sW, sP = fk(srecs)
    tpar = [struct.unpack_from("<i", r, 48)[0] for r in trecs]
    # 1) new target records: HCTP rotations, positions moved to SvR's where the name exists
    new = [bytearray(r) for r in trecs]
    newP = [None] * len(trecs); newW = [None] * len(trecs)
    moved = 0
    for i, r in enumerate(new):
        R = rot(*struct.unpack_from("<3f", r, 32)); p = tpar[i]
        if tnames[i] in snames:
            wp = sP[snames.index(tnames[i])]
            if p >= 0:
                d = [wp[j] - newP[p][j] for j in range(3)]
                inv = [list(x) for x in zip(*newW[p])]          # rotation inverse = transpose
                struct.pack_into("<3f", r, 16, *vm(d, inv))
            else:
                struct.pack_into("<3f", r, 16, *wp)
            moved += 1
        lp = struct.unpack_from("<3f", r, 16)
        if p < 0: newW[i] = R; newP[i] = list(lp)
        else: newW[i] = mul(R, newW[p]); newP[i] = [newP[p][j] + vm(lp, newW[p])[j] for j in range(3)]
    # check: moved bones land on SvR's world positions
    err = max(math.dist(newP[i], sP[snames.index(n)]) for i, n in enumerate(tnames) if n in snames)
    assert err < 1e-3, err
    # 2) SvR index -> target index
    spar = [struct.unpack_from("<i", r, 48)[0] for r in srecs]
    smap, folded = [], {}
    tnames = list(tnames)
    if KEEP_EXTRA:
        for i, n in enumerate(snames):
            if n in tnames or not keep_extra_name(n): continue
            k = spar[i]
            while k >= 0 and snames[k] not in tnames and snames[k] not in ALIAS: k = spar[k]
            p = tnames.index(snames[k]) if k >= 0 and snames[k] in tnames else (tnames.index(ALIAS[snames[k]]) if k >= 0 else 0)
            r = bytearray(srecs[i])
            d = [sP[i][j] - newP[p][j] for j in range(3)]
            inv = [list(x) for x in zip(*newW[p])]
            struct.pack_into("<3f", r, 16, *vm(d, inv)); struct.pack_into("<3f", r, 32, 0.0, 0.0, 0.0)
            struct.pack_into("<i", r, 48, p)
            new.append(r); tnames.append(n); newW.append(newW[p]); newP.append(list(sP[i]))
    for i, n in enumerate(snames):
        k = i
        while True:
            nm = snames[k]
            if nm in tnames: t = tnames.index(nm); break
            if nm in ALIAS: t = tnames.index(ALIAS[nm]); break
            if nm.startswith('kami') and any(x.startswith('kami') for x in tnames):
                # hair strand HCTP lacks -> the nearest HCTP hair bone (keeps it swinging with the hair)
                t = min((j for j, x in enumerate(tnames) if x.startswith('kami')),
                        key=lambda j: math.dist(newP[j], sP[k])); break
            k = spar[k]
            if k < 0: t = 0; break
        smap.append(t)
        if snames[i] != tnames[t]: folded[snames[i]] = tnames[t]
    # 3) rewrite bone refs in every mesh influence group (spec s4.1: mesh desc +0x08 count, +0x10 -> table, 32 B)
    import tempfile, os
    tf = tempfile.NamedTemporaryFile(delete=False, suffix=".yobj"); tf.write(bytes(y)); tf.close()
    try: parsed = YOBJ(tf.name)
    finally: os.unlink(tf.name)
    refs = 0
    for mesh in parsed.meshes:
        for g in mesh.groups:
            v = list(g.bone_refs)
            for j in range(4):
                if v[j] > 0: v[j] = smap[v[j] - 1] + 1; refs += 1
            struct.pack_into("<4i", y, g.addr + 16, *v)
    # 4) swap the bone table (shift pointers past it by the size change), rebuild POF0
    table_end = bt + 80 * len(srecs); D = 80 * len(new) - 80 * len(srecs)
    pof = struct.unpack_from("<I", y, 4)[0] + 8
    positions = decode_pof0(y, pof)
    for p in positions:
        if p == 0x24: continue
        raw = struct.unpack_from("<I", y, p)[0]
        if raw + 8 >= table_end: struct.pack_into("<I", y, p, raw + D)
    assert all(p < bt for p in positions), "pointer field inside/after bone table"
    body = y[:bt] + b"".join(new) + y[table_end:pof]
    struct.pack_into("<I", body, 0x1C, len(new))
    while len(body) % 0x10: body.append(0)
    struct.pack_into("<I", body, 0x04, len(body) - 8); struct.pack_into("<I", body, 0x0C, len(body) - 8)
    return bytes(body + encode_pof0(positions)), dict(moved=moved, refs=refs, folded=folded, src=len(srecs))


def main(src, donor_pac, out):
    order, r = pac_read(Path(src).read_bytes())
    dparts = pac_read(Path(donor_pac).read_bytes())[1]
    for ident in order:
        if r[ident][:4] != b"YOBJ": continue
        _, _, n = bone_records(r[ident])
        if 'koshi' not in n:
            print(f"part {ident}: {len(n)} bones (own skeleton) kept"); continue
        donor = dparts.get(ident, dparts[2]); _, _, dn = bone_records(donor)
        r[ident], rep = retarget_yobj(r[ident], donor)
        print(f"part {ident}: {rep['src']} -> {len(dn)} bones, moved {rep['moved']}, refs {rep['refs']}, "
              f"folded {len(rep['folded'])}")
        if ident == 2: print("  folds:", rep['folded'])
    Path(out).write_bytes(pac_write(order, r, Path(src).read_bytes()))


if __name__ == "__main__":
    a = [x for x in sys.argv[1:] if x != '--keep-extra']
    KEEP_EXTRA = len(a) != len(sys.argv) - 1
    main(*a[:3])
