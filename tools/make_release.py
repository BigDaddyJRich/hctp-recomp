# Makes the release zip a player downloads: dist\Def Jam Vendetta Recompiled.zip (nothing is published).
#   Def Jam Vendetta Recompiled\
#     Vendetta Settings.exe   the settings program with its first-run setup (.NET built in)
#     READ ME FIRST.txt
#     python\                 a portable Python (Python's own "embeddable" package; not installed on Windows)
#     source\                 the build kit: this repository's files (the notes and the game excluded), PS2Recomp at its
#                             pinned commit WITH our patch applied (so no git is needed), paraLLEl-GS + the parts of
#                             Granite it uses, and deps\ = the libraries PS2Recomp's CMake would clone with git
# Nothing from the disc or made from the game's code goes in: PS2Recomp comes from git (its working copy holds the
# game's generated code), and every name is checked at the end. Run: make-release.bat (needs git, the .NET SDK, and
# PS2Recomp built once here, for the libraries in PS2Recomp\out\*\_deps).
import io
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DIST = os.path.join(REPO, 'dist')
TOP = 'Def Jam Vendetta Recompiled'
SRC = TOP + '/source/'
PYTHON_VERSION = '3.14.6'
# the libraries FetchContent would clone (folder under source\deps\ <- where a build here has them), and what of
# them isn't needed (examples, tests: also the longest paths)
DEPS = {
    'raylib': ('out/fast/_deps/raylib-src', ('examples/', 'projects/')),
    'imgui': ('out/fast/_deps/imgui-src', ('examples/',)),
    'rlimgui': ('out/fast/_deps/rlimgui-src', ('examples/',)),
    'elfio': ('out/recomp/_deps/elfio-src', ('tests/', 'examples/')),
    'toml11': ('out/recomp/_deps/toml11-src', ('tests/', 'examples/')),
    'fmt': ('out/recomp/_deps/fmt-src', ('test/',)),
    'libdwarf': ('out/recomp/_deps/libdwarf-src', ()),
    'rabbitizer': ('out/recomp/_deps/rabbitizer-src', ('tests/',)),
}
FORBIDDEN = ('disc/', 'game/', 'generated/', 'shots/', 'mc0/', 'mc1/', 'logs/', 'mods/', 'dist/')
FORBIDDEN_SUFFIX = ('.chd', '.iso', '.elf', '.p2s')

README = r"""Def Jam Vendetta Recompiled
===========================

Def Jam Vendetta (PS2, USA, SLUS-20639) as a native PC program, made on your PC from your own disc.
No game files come with this download: the game is built here, from your copy, and never leaves your PC.

To play:
  1. Unzip this folder where you want the game to live (it needs about 8 GB once built).
  2. Open "Vendetta Settings.exe".
  3. Choose your disc: an .iso or .chd of the USA disc (SLUS-20639).
  4. Press "Next: build the game". If Microsoft's free C++ build tools aren't on this PC, they are installed first:
     allow Windows' prompt once. The first build takes about 20-30 minutes (it needs the internet once).
  5. Choose your settings, then PLAY. Shortcuts to the game and to Settings go on your desktop.

If Windows says "Windows protected your PC" when you open Vendetta Settings.exe:
  The program isn't signed with a paid certificate yet, so Windows doesn't recognise it. To open it anyway:
    1. Click "More info".
    2. Click "Run anyway".
  Or, before unzipping: right-click the downloaded zip > Properties > tick "Unblock" > OK. Then unzip it again;
  nothing inside will ask.

What's inside:
  Vendetta Settings.exe   settings, PLAY, and the first-run setup (.NET built in, nothing to install)
  python\                 a portable Python the setup uses (not installed on Windows)
  source\                 what the game is built from: this project's code, PS2Recomp, paraLLEl-GS and libraries
After the setup: the game program, game\ (your disc's files and your saves in game\mc0), disc\ (your disc as an
.iso), build\ (the build's working files: can be deleted once the game runs, to save space), logs\,
settings.toml.

Made by @jcsix694 (https://x.com/jcsix694). PS2Recomp is GPL; paraLLEl-GS is LGPL-3.0+; the libraries in source\deps
keep their own licences. Def Jam Vendetta is (c) its owners; this download includes none of it.
"""


def git(args, cwd):
    return subprocess.run(['git'] + args, cwd=cwd, check=True, capture_output=True).stdout


def check(name):
    low = name.lower()
    rel = low[len(SRC.lower()):] if low.startswith(SRC.lower()) else low
    base = os.path.basename(low)
    if rel.startswith(FORBIDDEN) or base.startswith('slus_') or low.endswith(FORBIDDEN_SUFFIX):
        raise SystemExit('refusing: ' + name)


def add_tree(z, folder, prefix, skip=()):
    n = 0
    for root, dirs, files in os.walk(folder):
        dirs[:] = [d for d in dirs if d != '.git']
        for f in files:
            path = os.path.join(root, f)
            rel = os.path.relpath(path, folder).replace(os.sep, '/')
            if rel.startswith(skip):
                continue
            name = prefix + rel
            check(name)
            z.write(path, name)
            n += 1
    return n


def add_archive(z, repo, prefix):
    data = git(['archive', '--format=tar', 'HEAD'], repo)
    n = 0
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for m in tar:
            if m.isfile():
                name = prefix + m.name
                check(name)
                z.writestr(name, tar.extractfile(m).read())
                n += 1
    return n


def settings_exe(work):
    out = os.path.join(work, 'publish')
    subprocess.run(['dotnet', 'publish', os.path.join(REPO, 'settings-app'), '-c', 'Release', '-r', 'win-x64',
                    '--self-contained', 'true', '-p:PublishSingleFile=true', '-p:IncludeNativeLibrariesForSelfExtract=true',
                    '-p:DebugType=none', '-o', out, '-nologo', '-v', 'q'], check=True)
    return os.path.join(out, 'Vendetta Settings.exe')


def python_zip():
    cache = os.path.join(DIST, 'cache', f'python-{PYTHON_VERSION}-embed-amd64.zip')
    if not os.path.isfile(cache):
        os.makedirs(os.path.dirname(cache), exist_ok=True)
        url = f'https://www.python.org/ftp/python/{PYTHON_VERSION}/python-{PYTHON_VERSION}-embed-amd64.zip'
        print('downloading', url)
        with urllib.request.urlopen(url) as r, open(cache + '.part', 'wb') as f:
            shutil.copyfileobj(r, f)
        os.replace(cache + '.part', cache)
    return cache


def patched_ps2recomp(work):
    """PS2Recomp at its pinned commit with our patch applied, in a scratch folder (a fresh clone + setup.bat)."""
    folder = os.path.join(work, 'PS2Recomp')
    os.makedirs(folder)
    data = git(['archive', '--format=tar', 'HEAD'], os.path.join(REPO, 'PS2Recomp'))
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        tar.extractall(folder, filter='data')
    subprocess.run(['git', 'apply', '--whitespace=nowarn', os.path.join(REPO, 'patches', 'ps2recomp-local.diff')],
                   cwd=folder, check=True)
    for patched in ('ps2xRuntime/src/lib/ps2_host_settings.cpp', 'ps2xRuntime/src/lib/ps2_pad_rumble.cpp'):
        if not os.path.isfile(os.path.join(folder, *patched.split('/'))):
            raise SystemExit(f'our patch did not apply: {patched} is missing')
    return folder


def main():
    dirty = git(['status', '--porcelain'], REPO).decode().strip()
    if dirty:
        print('note: uncommitted changes go in too (a real release should come from a commit)')
    os.makedirs(DIST, exist_ok=True)
    out = os.path.join(DIST, TOP + '.zip')
    counts = {}
    # outside the repository: git apply run inside a repository's subfolder skips the patch's paths without a word
    with tempfile.TemporaryDirectory(prefix='vdj-release-') as work:
        exe = settings_exe(work)
        pyzip = python_zip()
        ps2recomp = patched_ps2recomp(work)
        with zipfile.ZipFile(out + '.part', 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as z:
            z.write(exe, TOP + '/Vendetta Settings.exe')
            z.writestr(TOP + '/READ ME FIRST.txt', README.replace('\n', '\r\n'))
            with zipfile.ZipFile(pyzip) as py:
                for n in py.namelist():
                    z.writestr(TOP + '/python/' + n, py.read(n))
            # this repository: what git would commit (the notes are excluded locally, the game by .gitignore)
            listed = git(['ls-files', '-co', '--exclude-standard', '-z'], REPO).decode().split('\0')
            n = 0
            for rel in listed:
                if not rel or rel in ('PS2Recomp', 'third_party/parallel-gs') or rel.startswith(
                        ('settings-app/bin/', 'settings-app/obj/', 'dist/')):
                    continue
                path = os.path.join(REPO, rel)
                if os.path.isfile(path):
                    check(SRC + rel)
                    z.write(path, SRC + rel)
                    n += 1
            counts['ours'] = n
            counts['PS2Recomp'] = add_tree(z, ps2recomp, SRC + 'PS2Recomp/')
            pgs = os.path.join(REPO, 'third_party', 'parallel-gs')
            counts['parallel-gs'] = add_archive(z, pgs, SRC + 'third_party/parallel-gs/')
            granite = os.path.join(pgs, 'Granite')
            counts['Granite'] = add_archive(z, granite, SRC + 'third_party/parallel-gs/Granite/')
            for sub in ('third_party/volk', 'third_party/khronos/vulkan-headers'):
                counts[sub] = add_archive(z, os.path.join(granite, *sub.split('/')),
                                          SRC + 'third_party/parallel-gs/Granite/' + sub + '/')
            for name, (where, skip) in DEPS.items():
                folder = os.path.join(REPO, 'PS2Recomp', *where.split('/'))
                if not os.path.isdir(folder):
                    raise SystemExit(f'{folder} is missing: build PS2Recomp here once (setup.bat, build-fast.bat)')
                counts['deps/' + name] = add_tree(z, folder, SRC + 'deps/' + name + '/', skip)
        os.replace(out + '.part', out)
    print(counts)

    with zipfile.ZipFile(out) as z:
        infos = z.infolist()
        for i in infos:
            check(i.filename)
            if i.file_size > 20 * 1024 * 1024 and not i.filename.endswith('Vendetta Settings.exe'):
                raise SystemExit('too big: ' + i.filename)
            if i.filename.endswith('runner/register_functions.cpp'):
                text = z.read(i).decode('utf-8', 'replace')
                if 'FUN_00' in text or 'entry_00' in text:
                    raise SystemExit('game code in ' + i.filename)
        longest = max(infos, key=lambda i: len(i.filename))
        print(f'{len(infos)} files, {os.path.getsize(out) / 1e6:.1f} MB zipped, '
              f'{sum(i.file_size for i in infos) / 1e6:.1f} MB unzipped; longest name {len(longest.filename)}: '
              f'{longest.filename}')
    print('->', out)


if __name__ == '__main__':
    main()
