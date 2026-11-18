"""Regen safety step 4 (Nov 12 2026): check a runner folder against tools\\runner_manifest.json.

    python tools\\verify_runner.py <runner_dir> [--include DIR] [--manifest FILE]
    python tools\\verify_runner.py <scratch_runner_dir> --update      (after a DELIBERATE pipeline change)

The manifest = sha256 of the CR-stripped text of every file regen_runner.py produces (generated .cpp, pinned vu1_compiled.cpp,
patched files, the 2 generated headers). Headers are looked up in <runner_dir> first, then --include (default <runner_dir>\\..\\..\\include,
= ps2xRuntime\\include in a live tree). Ignored (not generated): hctp_*.cpp/.inl/.h, svr04_*, svr06_* (native SvR module, Nov 14), sym_* (native SYM module, Nov 17 S21), sub-folders.
Exit code: 0 = every manifest file present and identical and no extra generated file; 1 = any missing/extra/mismatch.
--update rewrites the manifest from <runner_dir> (use regen_runner.py output, never a live runner) and prints what changed.
"""
import argparse, hashlib, json, os, re, sys, time

TOOLS = os.path.dirname(os.path.abspath(__file__))
DEF_MANIFEST = os.path.join(TOOLS, "runner_manifest.json")
HEADERS = ("ps2_recompiled_functions.h", "ps2_recompiled_stubs.h")
IGNORE = re.compile(r"^(hctp_.*\.(cpp|inl|h)|svr0[46]_.*|sym_.*)$", re.I)


def sha(path):
    return hashlib.sha256(open(path, "rb").read().replace(b"\r", b"")).hexdigest()


def header_path(runner, include, name):
    for d in (runner, include):
        if d and os.path.isfile(os.path.join(d, name)):
            return os.path.join(d, name)
    return None


def scan(runner, include):
    files = {}
    for n in os.listdir(runner):
        p = os.path.join(runner, n)
        if os.path.isfile(p) and not IGNORE.match(n) and n not in HEADERS:
            files[n] = sha(p)
    for h in HEADERS:
        p = header_path(runner, include, h)
        if p:
            files[h] = sha(p)
    return files


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runner_dir")
    ap.add_argument("--include")
    ap.add_argument("--manifest", default=DEF_MANIFEST)
    ap.add_argument("--update", action="store_true")
    ap.add_argument("--max-list", type=int, default=50, help="max names printed per category")
    a = ap.parse_args()
    runner = os.path.abspath(a.runner_dir)
    include = a.include or os.path.normpath(os.path.join(runner, "..", "..", "include"))
    if not os.path.isdir(runner):
        sys.exit(f"verify_runner: no folder {runner}")
    t = time.time()
    have = scan(runner, include)
    old = {}
    if os.path.isfile(a.manifest):
        old = json.load(open(a.manifest, encoding="utf-8"))["files"]

    missing = sorted(set(old) - set(have))
    extra = sorted(set(have) - set(old))
    bad = sorted(n for n in set(old) & set(have) if old[n] != have[n])

    def show(tag, names):
        for n in names[:a.max_list]:
            print(f"  {tag} {n}")
        if len(names) > a.max_list:
            print(f"  ... {len(names) - a.max_list} more {tag}")

    if a.update:
        if os.path.isfile(os.path.join(runner, "hctp_overrides.cpp")):
            sys.exit("verify_runner: --update on a live runner refused; run regen_runner.py into scratch and update from that")
        man = {
            "about": "sha256 of CR-stripped content of every file tools\\regen_runner.py produces. "
                     "Refresh only via: regen_runner.py <scratch> then verify_runner.py <scratch> --update.",
            "updated": time.strftime("%Y-%m-%d %H:%M"),
            "count": len(have),
            "files": dict(sorted(have.items())),
        }
        with open(a.manifest, "w", encoding="utf-8", newline="\n") as f:
            json.dump(man, f, indent=0, sort_keys=False)
            f.write("\n")
        print(f"verify_runner --update: {len(have)} files written to {a.manifest}")
        show("REMOVED", missing); show("ADDED", extra); show("CHANGED", bad)
        print(f"  {len(missing)} removed, {len(extra)} added, {len(bad)} changed")
        return 0

    if not old:
        sys.exit(f"verify_runner: no manifest {a.manifest} (create it with --update from a regen_runner.py output)")
    show("MISSING ", missing); show("EXTRA   ", extra); show("MISMATCH", bad)
    ok = not (missing or extra or bad)
    print(f"verify_runner: {'OK' if ok else 'FAILED'} - {len(old)} manifest files, {len(missing)} missing, "
          f"{len(extra)} extra, {len(bad)} mismatched ({time.time() - t:.0f}s, {runner})")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
