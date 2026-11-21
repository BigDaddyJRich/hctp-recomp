"""Regen safety step 4 (Nov 12 2026): rebuild the generated runner from scratch, in the one proven order.

    python tools\\regen_runner.py <out_runner_dir> [--recomp EXE] [--toml TOML] [--vu1 FILE] [--work DIR] [--force] [--verify]

Pipeline (notes\\REGEN_SAFETY_20261112.md steps 1-3, F:\\hctp-project):
  1. ps2_recomp.exe on a temp copy of config_hctp\\hctp.toml whose [general] output = a temp folder
  2. copy the PINNED vu1_compiled.cpp (Oct 13 output, 20 programs, exact variant). Never rerun vu1_recomp.py: it adds the FAST variant.
  3. VU0 regexes of sync_generated.py (fix_vf0 / fix_vu0mem, same per-file conditions), taken from sync_generated.py itself
  4. patch_rosterx_cawcheck.py -> patch_rosterx_cawoff.py -> patch_rosterx_scale.py -> patch_prop_texwindow.py -> patch_runner_handedits.py
Result: <out_runner_dir> holds every generated runner file (.cpp + the 2 generated headers). Nothing else is written outside the
temp work folder (the patch scripts' overrides_backup copies stay in it). A new patch script (e.g. roster Step C2) MUST be added to
PATCH_SCRIPTS below and the manifest refreshed (verify_runner.py <scratch> --update).
"""
import argparse, os, re, shutil, subprocess, sys, tempfile, time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
DEF_RECOMP = r"F:\hctp-newbase-vulcan\out\recomp\ps2xRecomp\ps2_recomp.exe"
DEF_TOML = os.path.join(ROOT, "config_hctp", "hctp.toml")
DEF_VU1 = os.path.join(ROOT, "generated", "vu1_compiled.cpp")  # pinned copy (= live runner copy, Oct 13 07:52)
PATCH_SCRIPTS = [  # order matters (MakeChar 0x345700 declaration order = canonical order of this list)
    "patch_rosterx_cawcheck.py",
    "patch_rosterx_cawoff.py",
    "patch_rosterx_scale.py",
    "patch_prop_texwindow.py",
    "patch_runner_handedits.py",
    "patch_rosterx_code32.py",  # Nov 12 Step C2 (32-bit cast codes)
    "patch_svr_native_motion.py",  # Nov 12 SvR04/06 native motion (after handedits: rewrites its CCharIK twist hunks)
    "compact_function_table.py",  # 0.3.7: register_functions.cpp as constant data (LAST: handedits edits its lines)
]


def load_vu0_fixes():
    """fix_vf0 / fix_vu0mem / _VU0MEM from sync_generated.py itself (exec'd up to its copy loop, so nothing is copied)."""
    src_path = os.path.join(TOOLS, "sync_generated.py")
    code = open(src_path, encoding="utf-8").read()
    marker = "changed = added = 0"
    if marker not in code:
        sys.exit(f"regen_runner: '{marker}' not found in sync_generated.py - its layout changed, update load_vu0_fixes()")
    g = {"__file__": src_path, "__name__": "sync_generated_defs"}
    exec(compile(code[:code.index(marker)], src_path, "exec"), g)
    for k in ("fix_vf0", "fix_vu0mem", "_VU0MEM"):
        if k not in g:
            sys.exit(f"regen_runner: sync_generated.py no longer defines {k}")
    return g


def apply_vu0(runner):
    """Same per-file conditions as sync_generated.py's copy loop, in place."""
    g = load_vu0_fixes()
    n = 0
    for f in sorted(os.listdir(runner)):
        if not f.endswith(".cpp"):
            continue
        p = os.path.join(runner, f)
        data = open(p, "rb").read()
        has_vf0 = b"ctx->vu0_vf[0] = " in data
        has_mem = g["_VU0MEM"].search(data) is not None
        if not (has_vf0 or has_mem):
            continue
        if has_vf0:
            data = g["fix_vf0"](data)
        if has_mem:
            data = g["fix_vu0mem"](data)
        open(p, "wb").write(data)
        n += 1
    return n


def write_toml(src, dst, out_dir):
    text = open(src, encoding="utf-8").read()
    pat = re.compile(r'^output\s*=\s*".*"\s*$', re.M)
    if len(pat.findall(text)) != 1:
        sys.exit(f"regen_runner: expected exactly one 'output = \"...\"' line in {src}")
    text = pat.sub(lambda m: 'output = "' + out_dir.replace("\\", "/") + '"', text)
    open(dst, "w", encoding="utf-8", newline="\n").write(text)


def run(cmd, label):
    t = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    out = (p.stdout or "") + (p.stderr or "")
    tail = [l for l in out.splitlines() if l.strip()][-4:]
    print(f"[{label}] rc={p.returncode} {time.time() - t:.0f}s")
    for l in tail:
        print("    " + l[:200])
    if p.returncode != 0 or re.search(r"^FAIL\b", out, re.M):
        sys.exit(f"regen_runner: step '{label}' FAILED (rc {p.returncode})")
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("out_runner_dir")
    ap.add_argument("--recomp", default=DEF_RECOMP)
    ap.add_argument("--toml", default=DEF_TOML)
    ap.add_argument("--vu1", default=DEF_VU1, help="pinned vu1_compiled.cpp")
    ap.add_argument("--work", help="work folder (default: a new temp folder, deleted at the end)")
    ap.add_argument("--force", action="store_true", help="replace a non-empty out_runner_dir")
    ap.add_argument("--verify", action="store_true", help="run verify_runner.py on the result (non-zero exit on mismatch)")
    a = ap.parse_args()

    out = os.path.abspath(a.out_runner_dir)
    for p in (a.recomp, a.toml, a.vu1):
        if not os.path.isfile(p):
            sys.exit(f"regen_runner: missing {p}")
    if os.path.isdir(out) and os.listdir(out):
        if not a.force:
            sys.exit(f"regen_runner: {out} is not empty (use --force to replace it)")
        if os.path.isfile(os.path.join(out, "hctp_overrides.cpp")):
            sys.exit(f"regen_runner: {out} holds hctp_overrides.cpp = a live runner; refusing to replace it")
        shutil.rmtree(out)

    work = os.path.abspath(a.work) if a.work else tempfile.mkdtemp(prefix="hctp_regen_")
    # 3 dummy levels: the patch scripts back up to <runner>\..\..\..\overrides_backup -> stays inside work
    runner = os.path.join(work, "w", "a", "b", "runner")
    os.makedirs(runner, exist_ok=False)
    toml = os.path.join(work, "hctp_regen.toml")
    write_toml(a.toml, toml, runner)

    run([a.recomp, toml], "ps2_recomp")
    nfiles = len(os.listdir(runner))
    if nfiles < 10000:
        sys.exit(f"regen_runner: recompiler wrote only {nfiles} files")
    if os.path.exists(os.path.join(runner, "vu1_compiled.cpp")):
        sys.exit("regen_runner: the recompiler now writes vu1_compiled.cpp - re-check the VU1 pin before continuing")
    shutil.copyfile(a.vu1, os.path.join(runner, "vu1_compiled.cpp"))
    print(f"[vu1 pin] {a.vu1}")
    print(f"[vu0] {apply_vu0(runner)} files")
    for s in PATCH_SCRIPTS:
        run([sys.executable, os.path.join(TOOLS, s), runner], s)

    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    shutil.move(runner, out)
    if not a.work:
        shutil.rmtree(work, ignore_errors=True)
    print(f"regen_runner: {len(os.listdir(out))} files -> {out}")
    if a.verify:
        rc = subprocess.run([sys.executable, os.path.join(TOOLS, "verify_runner.py"), out]).returncode
        sys.exit(rc)


if __name__ == "__main__":
    main()
