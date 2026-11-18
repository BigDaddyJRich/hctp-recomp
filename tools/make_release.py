# Helpers for tools/make_release_hctp.py (the build kit a player downloads): git, the name checks, adding folders and
# git archives to the zip, the launcher's single-file exe, the portable Python, and files/release-manifest.txt.
# Nothing from the disc or made from the game's code goes in: every name is checked.
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
TOP = FILES = SRC = ''  # tools/make_release_hctp.py sets these
PYTHON_VERSION = '3.14.6'
FORBIDDEN = ('disc/', 'game/', 'generated/', 'shots/', 'mc0/', 'mc1/', 'logs/', 'mods/', 'dist/')
FORBIDDEN_SUFFIX = ('.chd', '.iso', '.elf', '.p2s')

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


def add_archive(z, repo, prefix, skip=None, ref='HEAD'):
    data = git(['archive', '--format=tar', ref], repo)
    n = 0
    with tarfile.open(fileobj=io.BytesIO(data)) as tar:
        for m in tar:
            if m.isfile() and not (skip and skip(m.name)):
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
    return os.path.join(out, 'HCTP Recompiled Settings.exe')


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



def version():
    """The version, from HCTP Recompiled Settings' project (the one place it is set)."""
    with open(os.path.join(REPO, 'settings-app', 'HctpSettings.csproj'), encoding='utf-8') as f:
        text = f.read()
    start = text.index('<Version>') + len('<Version>')
    return text[start:text.index('</Version>', start)].strip()


def add_manifest(path):
    """files/release-manifest.txt: every file of the zip with its SHA-256 and size. The updater compares it with what is
    installed, so an update replaces only the files that changed (and keeps the build's work for the rest)."""
    import hashlib
    lines = [f'# {TOP} {version()}', f'# sha256 size path (below "{TOP}/")']
    with zipfile.ZipFile(path) as z:
        for info in z.infolist():
            if info.is_dir():
                continue
            digest = hashlib.sha256()
            with z.open(info) as f:
                for chunk in iter(lambda: f.read(1 << 20), b''):
                    digest.update(chunk)
            lines.append(f'{digest.hexdigest()} {info.file_size} {info.filename[len(TOP) + 1:]}')
    with zipfile.ZipFile(path, 'a', zipfile.ZIP_DEFLATED) as z:
        z.writestr(FILES + 'release-manifest.txt', '\n'.join(lines) + '\n')
    print(f'release-manifest.txt: {len(lines) - 2} files, version {version()}')

