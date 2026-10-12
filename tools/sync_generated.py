# Copies the recompiler's output (generated\) into PS2Recomp's runtime, touching only files whose content
# changed, so ninja recompiles just those. The two generated headers go to ps2xRuntime\include\ (as the
# runtime README says); .cpp files no longer generated are removed from the runner folder.
import filecmp, os, shutil, sys

root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
gen = os.path.join(root, 'generated')
runner = os.path.join(root, 'PS2Recomp', 'ps2xRuntime', 'src', 'runner')
include = os.path.join(root, 'PS2Recomp', 'ps2xRuntime', 'include')
HEADERS = {'ps2_recompiled_functions.h', 'ps2_recompiled_stubs.h'}
changed = added = 0
names = set(os.listdir(gen))
# reading ~25,000 files takes minutes: say how far it is (the setup turns these lines into progress)
total = len(names)
for i, n in enumerate(sorted(names), 1):
    if i % 1000 == 0 or i == total:
        print(f'Checking which files changed: {i:,} of {total:,}', flush=True)
    dst = os.path.join(include if n in HEADERS else runner, n)
    src = os.path.join(gen, n)
    if os.path.exists(dst):
        if filecmp.cmp(src, dst, shallow=False):
            continue
        changed += 1
    else:
        added += 1
    shutil.copyfile(src, dst)
# our own runtime code (overrides\), kept with the generated files
for n in os.listdir(os.path.join(root, 'overrides')):
    src, dst = os.path.join(root, 'overrides', n), os.path.join(runner, n)
    if not os.path.exists(dst) or not filecmp.cmp(src, dst, shallow=False):
        shutil.copyfile(src, dst); changed += 1
    names.add(n)
removed = 0
for n in os.listdir(runner):
    if n.endswith('.cpp') and n not in names:
        os.remove(os.path.join(runner, n)); removed += 1
print(f'sync: {changed} changed, {added} added, {removed} removed, {len(names) - changed - added} unchanged')
