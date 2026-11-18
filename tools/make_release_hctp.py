# Makes the HCTP build kit a player downloads: dist\HCTP-Recomp-Windows.zip (nothing is published).
#   HCTP Recomp\
#     HCTP Recompiled Settings.exe   the launcher: asks for the disc, builds the game on this PC, PLAY, settings
#     READ ME FIRST.txt
#     files\python\           a portable Python (python.org's embeddable package)
#     files\source\           what the build needs from this repository (KEEP), PS2Recomp at its HEAD commit minus
#                             dev scripts / unused targets, and deps\ = the libraries PS2Recomp's CMake would clone
# Nothing from the disc and nothing made from the game's code goes in: the recompiled C++ (runner\, generated\) and
# the compiled exe are made on the player's PC. Every name is checked. Needs PS2Recomp built here once (out\recomp,
# out\native: their _deps). Run: python tools\make_release_hctp.py
import os
import sys
import tempfile
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_release as base  # noqa: E402

REPO = base.REPO
DIST = base.DIST
TOP = 'HCTP Recompiled'
ZIP_NAME = 'HCTP-Recompiled-Windows.zip'
EXE = 'HCTP Recompiled Settings.exe'  # the launcher (settings-app): first-run setup, PLAY, settings, updates
FILES = TOP + '/files/'
SRC = FILES + 'source/'
base.SRC = SRC  # base.check() strips this prefix
base.TOP, base.FILES = TOP, FILES  # base.add_manifest() writes files/release-manifest.txt for the updater
DEPS = {
    'raylib': ('out/native/_deps/raylib-src', ('examples/', 'projects/')),
    'imgui': ('out/native/_deps/imgui-src', ('examples/',)),
    'rlimgui': ('out/native/_deps/rlimgui-src', ('examples/', 'premake5')),
    'elfio': ('out/recomp/_deps/elfio-src', ('tests/', 'examples/')),
    'toml11': ('out/recomp/_deps/toml11-src', ('tests/', 'examples/')),
    'fmt': ('out/recomp/_deps/fmt-src', ('test/',)),
    'libdwarf': ('out/recomp/_deps/libdwarf-src', ('test/', 'fuzz/')),
    'rabbitizer': ('out/recomp/_deps/rabbitizer-src', ('tests/',)),
}
# Nov 4 (Jordan): players get only what they need to build and play - no test/run bats, dev tools, notes or docs, and
# no "Build HCTP.bat" (it set up <kit>\game while the launcher uses <kit>\files\game: two layouts, the bat's failed more).
# The launcher runs tools/hctp_setup.py (+ setup_game.py, hctp_prepare_disc.py -> prepare_disc.py -> disc/chdreader.py,
# vu1_recomp.py + hctp_vu1_ranges.txt, sync_generated.py -> overrides_hctp/), config_hctp/hctp.toml, and opens
# LICENSE / THIRD-PARTY-NOTICES.txt.
KEEP = ('LICENSE', 'THIRD-PARTY-NOTICES.txt', 'README.md', 'CHANGELOG.md', 'config_hctp/', 'overrides_hctp/',
        'tools/hctp_setup.py', 'tools/setup_game.py', 'tools/hctp_prepare_disc.py', 'tools/prepare_disc.py', 'tools/disc/',
        'tools/vu1_recomp.py', 'tools/hctp_vu1_ranges.txt', 'tools/sync_generated.py',
        # Nov 18: the runner patches the developers' build applies (tools/regen_runner.py PATCH_SCRIPTS); hctp_setup.py
        # runs them after sync_generated.py so a player's game = the tested one
        'tools/patch_rosterx_cawcheck.py', 'tools/patch_rosterx_cawoff.py', 'tools/patch_rosterx_scale.py',
        'tools/patch_prop_texwindow.py', 'tools/patch_runner_handedits.py', 'tools/patch_rosterx_code32.py',
        'tools/patch_svr_native_motion.py')
# Nov 18: the runtime = the active PS2Recomp worktree (F:\hctp-newbase-vulcan, branch codex/new-base-vulkan) at a PINNED
# commit (tested by Jordan; the worktree's uncommitted edits never go in). Move the pin only to a tested commit.
RUNTIME_REPO = r'F:\hctp-newbase-vulcan'
RUNTIME_REF = 'a71bdff'
# PS2Recomp: its top-level run/build bats and notes, backup copies, and targets the game build never configures
# (analyzer / studio / test / android / vita sit behind PS2X_BUILD_* options the setup turns off).
PS2RECOMP_SKIP = ('CLAUDE.md', 'CMakeSettings.json', '.github/', 'overrides_backup/', 'android/', 'vita/',
                  'ps2xAnalyzer/', 'ps2xStudio/', 'ps2xTest/', 'scripts/', 'ps2xRuntime/vita/',
                  'third_party/ps2-native-gs/')  # dev scripts, Vita port, the parked Vulkan renderer


def ps2recomp_skip(name):
    return name.startswith(PS2RECOMP_SKIP) or ('/' not in name and name.lower().endswith(('.bat', '.ps1', '.sh')))


README = r"""HCTP Recompiled - WWE SmackDown! Here Comes the Pain on PC
==========================================================

This turns YOUR OWN copy of the PS2 game into a Windows game. The download has no game files: the game is built on
your PC from your disc.

WHAT YOU NEED
  - The USA disc: WWE SmackDown! Here Comes the Pain, SLUS-20787, as an .iso or .chd made from the real disc.
    ONLY the unmodified USA retail disc works. Modded, patched, trainer or other-region ISOs break the game
    (for example a freeze on the Brock Lesnar screen). If you get freezes, make a fresh ISO from your disc first.
  - Windows 10 or 11 (64-bit), a CPU with AVX2, a DirectX 11 graphics card, 16 GB of RAM, 15 GB of free space,
    and internet for the first build.

INSTALL (step by step)
  1. Unzip this folder to a short path, for example C:\Games\HCTP Recompiled
     (not Desktop / Downloads / OneDrive: long paths can break the build).
  2. Open "HCTP Recompiled Settings.exe". This is the launcher.
     If Windows says "Windows protected your PC": click "More info", then "Run anyway".
  3. Pick your disc image (.iso or .chd). The launcher builds the game on your PC.
     If Microsoft's free C++ build tools are missing, it installs them first (allow the Windows prompt once).
     The first build takes 30-90 minutes. Close your browser and other games while it builds.
  4. When it is done, press PLAY.

PLAYING
  - Settings (screen, picture, sound, controllers) are in the launcher. Open it any time to change them.
  - Saves are kept in Documents\HCTP (back this folder up if you reinstall).
  - Steam: Games > Add a Non-Steam Game > Browse > files\game\"WWE SmackDown! Here Comes the Pain.exe".
    It uses the settings you saved in the launcher.

CONTROLLERS
  - Xbox controllers (and anything that works as an Xbox controller): plug in and play.
  - PlayStation 4 and 5 controllers: USB or Bluetooth, with vibration. No extra programs needed.
  - Keyboard: the keys are listed (and can be changed) on the launcher's Controls page. Controller buttons can be
    changed there too.
  - Up to 4 players (8 controllers can be connected).

SLOW OR CHOPPY?
  Open the launcher > Settings > lower "Scale" (Auto picks it from your screen: a 4K screen picks 6x,
  which is too much for most graphics cards). Try 3x, then 2x. Playing in a window or at 1080p also helps.

FOUND A BUG?
  Post it in the bugs channel of the HCTP Discord (launcher > About > Report a bug) or on GitHub:
  https://github.com/BigDaddyJRich/hctp-recomp/issues
  Please include: what you did, your CPU / graphics card / RAM, and the log files game.log and game.log.err
  from files\logs\ (files\game\logs\ if you start the game from Steam); setup.log if the build failed.

Free software under the GNU GPL version 3 (files\source\LICENSE); the parts it uses and their licences are in
files\source\THIRD-PARTY-NOTICES.txt. Built on PS2Recomp (ran-j). WWE SmackDown! Here Comes the Pain is (c) its
owners; this download includes none of it.
"""


def main():
    if base.git(['status', '--porcelain'], REPO).decode().strip():
        print('note: uncommitted changes go in too (a real release should come from a commit)')
    os.makedirs(DIST, exist_ok=True)
    out = os.path.join(DIST, ZIP_NAME)
    counts = {}
    with tempfile.TemporaryDirectory(prefix='hctp-release-') as work:
        base.settings_exe(work)
        exe = os.path.join(work, 'publish', EXE)
        pyzip = base.python_zip()
        with zipfile.ZipFile(out + '.part', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            z.write(exe, TOP + '/' + EXE)
            z.writestr(TOP + '/READ ME FIRST.txt', README.replace('\n', '\r\n'))
            with zipfile.ZipFile(pyzip) as py:
                for n in py.namelist():
                    z.writestr(FILES + 'python/' + n, py.read(n))
            # committed files only (Nov 18): untracked dev files (other-disc tomls, scratch tools) never go in
            listed = base.git(['ls-files', '-c', '-z'], REPO).decode().split('\0')
            n = 0
            for rel in listed:
                if not rel.startswith(KEEP) or rel.endswith(('.pid', '.log', '.pyc')):
                    continue
                path = os.path.join(REPO, rel)
                if os.path.isfile(path):
                    base.check(SRC + rel)
                    z.write(path, SRC + rel)
                    n += 1
            counts['ours'] = n
            # PS2Recomp as committed (its working copy holds the game's generated runner code: never taken from there)
            counts['PS2Recomp@' + RUNTIME_REF] = base.add_archive(z, RUNTIME_REPO, SRC + 'PS2Recomp/', ps2recomp_skip,
                                                                  RUNTIME_REF)
            for name, (where, skip) in DEPS.items():
                folder = os.path.join(REPO, 'PS2Recomp', *where.split('/'))
                if not os.path.isdir(folder):
                    raise SystemExit(f'{folder} is missing: build PS2Recomp here once (build-recomp.bat, build-native.bat)')
                counts['deps/' + name] = base.add_tree(z, folder, SRC + 'deps/' + name + '/', skip)
        base.add_manifest(out + '.part')
        os.replace(out + '.part', out)
    print(counts)

    with zipfile.ZipFile(out) as z:
        infos = z.infolist()
        runner = [i.filename for i in infos if '/src/runner/' in i.filename]
        for i in infos:
            base.check(i.filename)
            low = i.filename.lower()
            if low.endswith(('.exe', '.dll')) and not low.startswith((FILES + 'python/').lower()) and i.filename != TOP + '/' + EXE:
                raise SystemExit('a program in the kit: ' + i.filename)
            if '_0x' in os.path.basename(low) or os.path.basename(low) == 'vu1_compiled.cpp':
                raise SystemExit('game code in ' + i.filename)
            if i.file_size > 20 * 1024 * 1024 and i.filename != TOP + '/' + EXE:
                raise SystemExit('too big: ' + i.filename)
            if i.filename.endswith('runner/register_functions.cpp'):
                text = z.read(i).decode('utf-8', 'replace')
                if 'sub_00' in text or 'FUN_00' in text or 'entry_00' in text:
                    raise SystemExit('game code in ' + i.filename)
        longest = max(infos, key=lambda i: len(i.filename))
        print(f'runner files: {runner}')
        print(f'{len(infos)} files, {os.path.getsize(out) / 1e6:.1f} MB zipped, '
              f'{sum(i.file_size for i in infos) / 1e6:.1f} MB unzipped; longest name {len(longest.filename)}: '
              f'{longest.filename}')
    print('->', out)


if __name__ == '__main__':
    main()
