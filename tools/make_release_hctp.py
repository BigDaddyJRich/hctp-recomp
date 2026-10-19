# Makes the HCTP build kit a player downloads: dist\HCTP-Recomp-Windows.zip (nothing is published).
#   HCTP Recomp\
#     Build HCTP.bat          drop your disc (.iso/.chd) or game folder on it: builds the game on this PC
#     READ ME FIRST.txt
#     files\python\           a portable Python (python.org's embeddable package)
#     files\source\           this repository (as git would commit it), PS2Recomp at its HEAD commit, and deps\ =
#                             the libraries PS2Recomp's CMake would clone with git
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
TOP = 'HCTP Recomp'
ZIP_NAME = 'HCTP-Recomp-Windows.zip'
FILES = TOP + '/files/'
SRC = FILES + 'source/'
base.SRC = SRC  # base.check() strips this prefix
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
SKIP = ('settings-app/', 'dist/', 'images/', 'logs/', 'shots/', 'third_party/')

BUILD_BAT = r'''@echo off
rem Builds WWE SmackDown! Here Comes the Pain for this PC from YOUR copy of the game (USA, SLUS-20787).
rem Drop your disc image (.iso / .chd) or your game folder (the one with SLUS_207.87) on this file, or run it and
rem paste the path when asked.
cd /d "%~dp0"
set "DISC=%~1"
if "%DISC%"=="" set /p "DISC=Path to your HCTP .iso / .chd or game folder (Enter = already set up): "
if "%DISC%"=="" (
  "files\python\python.exe" "files\source\tools\hctp_setup.py" --data "%~dp0."
) else (
  "files\python\python.exe" "files\source\tools\hctp_setup.py" --data "%~dp0." --disc "%DISC%"
)
if errorlevel 1 (echo. & echo The build stopped - the [error] line above says why. Logs: "%~dp0logs") else (echo. & echo Done. Start the game with "Play HCTP.bat".)
pause
'''

README = r"""HCTP Recomp - WWE SmackDown! Here Comes the Pain on PC (work in progress)
=========================================================================

WWE SmackDown! Here Comes the Pain (PS2, USA, SLUS-20787) as a native Windows program, BUILT ON YOUR PC FROM YOUR
OWN COPY OF THE GAME. This download has no game files and none of the game's code: the game's code is translated to
C++ and compiled here, and it never leaves your PC.

You need: Windows 10/11 64-bit, a CPU with AVX2, a Direct3D 11 graphics card, ~16 GB of RAM, ~15 GB of free space,
internet for the first build, and your own HCTP disc (USA) as an .iso or .chd, or its files in a folder.

To build and play:
  1. Unzip this folder somewhere with a SHORT path, e.g. C:\Games\HCTP Recomp (long paths can break the build).
  2. Drag your disc image (.iso / .chd) or your game folder onto "Build HCTP.bat".
     If Microsoft's free C++ build tools aren't installed, they are installed first (allow Windows' prompt once).
     The first build takes a while (roughly 30-90 minutes, depending on the PC).
  3. When it says Done, start the game with "Play HCTP.bat".

If Windows says "Windows protected your PC": click "More info", then "Run anyway". Or right-click the zip >
Properties > tick "Unblock" > OK before unzipping.

After the build:
  game\       your game's files plus ps2EntryRunner.exe (the game) and its DLLs; memory cards are in game\mc0, mc1
  build\, logs\   the build's working files (build\ can be deleted once the game runs, to save space)

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
    with tempfile.TemporaryDirectory(prefix='hctp-release-'):
        pyzip = base.python_zip()
        with zipfile.ZipFile(out + '.part', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            z.writestr(TOP + '/READ ME FIRST.txt', README.replace('\n', '\r\n'))
            z.writestr(TOP + '/Build HCTP.bat', BUILD_BAT.replace('\r\n', '\n').replace('\n', '\r\n'))
            with zipfile.ZipFile(pyzip) as py:
                for n in py.namelist():
                    z.writestr(FILES + 'python/' + n, py.read(n))
            listed = base.git(['ls-files', '-co', '--exclude-standard', '-z'], REPO).decode().split('\0')
            n = 0
            for rel in listed:
                if not rel or rel == 'PS2Recomp' or rel.startswith(SKIP) or rel.endswith(('.pid', '.log')):
                    continue
                path = os.path.join(REPO, rel)
                if os.path.isfile(path):
                    base.check(SRC + rel)
                    z.write(path, SRC + rel)
                    n += 1
            counts['ours'] = n
            # PS2Recomp as committed (its working copy holds the game's generated runner code: never taken from there)
            counts['PS2Recomp'] = base.add_archive(z, os.path.join(REPO, 'PS2Recomp'), SRC + 'PS2Recomp/')
            for name, (where, skip) in DEPS.items():
                folder = os.path.join(REPO, 'PS2Recomp', *where.split('/'))
                if not os.path.isdir(folder):
                    raise SystemExit(f'{folder} is missing: build PS2Recomp here once (build-recomp.bat, build-native.bat)')
                counts['deps/' + name] = base.add_tree(z, folder, SRC + 'deps/' + name + '/', skip)
        os.replace(out + '.part', out)
    print(counts)

    with zipfile.ZipFile(out) as z:
        infos = z.infolist()
        runner = [i.filename for i in infos if '/src/runner/' in i.filename]
        for i in infos:
            base.check(i.filename)
            low = i.filename.lower()
            if low.endswith(('.exe', '.dll')) and not low.startswith((FILES + 'python/').lower()):
                raise SystemExit('a program in the kit: ' + i.filename)
            if '_0x' in os.path.basename(low) or os.path.basename(low) == 'vu1_compiled.cpp':
                raise SystemExit('game code in ' + i.filename)
            if i.file_size > 20 * 1024 * 1024:
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
