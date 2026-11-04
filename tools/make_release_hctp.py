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
        'tools/vu1_recomp.py', 'tools/hctp_vu1_ranges.txt', 'tools/sync_generated.py')
# PS2Recomp: its top-level run/build bats and notes, backup copies, and targets the game build never configures
# (analyzer / studio / test / android / vita sit behind PS2X_BUILD_* options the setup turns off).
PS2RECOMP_SKIP = ('CLAUDE.md', 'CMakeSettings.json', '.github/', 'overrides_backup/', 'android/', 'vita/',
                  'ps2xAnalyzer/', 'ps2xStudio/', 'ps2xTest/')


def ps2recomp_skip(name):
    return name.startswith(PS2RECOMP_SKIP) or ('/' not in name and name.lower().endswith(('.bat', '.ps1', '.sh')))


README = r"""HCTP Recomp - WWE SmackDown! Here Comes the Pain on PC (work in progress)
=========================================================================

WWE SmackDown! Here Comes the Pain (PS2, USA, SLUS-20787) as a native Windows program, BUILT ON YOUR PC FROM YOUR
OWN COPY OF THE GAME. This download has no game files and none of the game's code: the game's code is translated to
C++ and compiled here, and it never leaves your PC.

You need: Windows 10/11 64-bit, a CPU with AVX2, a Direct3D 11 graphics card, ~16 GB of RAM, ~15 GB of free space,
internet for the first build, and your own HCTP disc (USA) as an .iso or .chd, or its files in a folder.

To build and play:
  1. Unzip this folder somewhere with a SHORT path, e.g. C:\Games\HCTP Recompiled (long paths can break the build).
  2. Open "HCTP Recompiled Settings.exe" (the launcher) and pick your disc image (.iso / .chd) or game folder.
     It builds the game on your PC. If Microsoft's free C++ build tools aren't installed, they are installed first
     (allow Windows' prompt once). The first build takes a while (roughly 30-90 minutes, depending on the PC).
     Close other programs (browser, games) while it builds: it needs the memory.
  3. When it's done, press PLAY. Settings (display, renderer, controllers, volume) and updates are in the same window.

If Windows says "Windows protected your PC": click "More info", then "Run anyway". Or right-click the zip >
Properties > tick "Unblock" > OK before unzipping.

After the build (inside files\):
  game\           your game's files plus "WWE SmackDown! Here Comes the Pain.exe" (the game) and its DLLs
  build\, logs\   the build's working files (build\ can be deleted once the game runs, to save space)

Add it to Steam: Steam > Games > Add a Non-Steam Game to My Library > Browse > files\game\
"WWE SmackDown! Here Comes the Pain.exe". It starts on its own (no launcher needed) with the settings you saved in
the launcher. Keep it in files\game: the game needs the files next to it.

Optional packs (shared on the project's Discord, not part of this download):
  Content Pack     the restored wrestlers (Hulk Hogan, Ultimate Warrior, Jeff Hardy, Regal, Kidman ...), models,
                   entrances, music, TitanTrons. Without it you get the stock roster.
  Commentary Pack  extra commentary and Finkel lines.
  Install: build/update the game first, close it, then unzip the pack INTO files\game\ (the folder with SLUS_207.87),
  merging folders and replacing files. A newer pack unzips over an older one the same way. Each pack has its own
  README.txt with the details. The launcher's Play page shows whether each pack is installed.

This is a work in progress: see the project page for what works and the known bugs.
  https://github.com/BigDaddyJRich/hctp-recomp

Free software under the GNU GPL version 3: files\source\LICENSE; the parts it uses and their licences:
files\source\THIRD-PARTY-NOTICES.txt. Built on Def Jam Vendetta Recompiled (jcsix694) and PS2Recomp (ran-j).
WWE SmackDown! Here Comes the Pain is (c) its owners. This download includes none of it.
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
            listed = base.git(['ls-files', '-co', '--exclude-standard', '-z'], REPO).decode().split('\0')
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
            counts['PS2Recomp'] = base.add_archive(z, os.path.join(REPO, 'PS2Recomp'), SRC + 'PS2Recomp/',
                                                   ps2recomp_skip)
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
