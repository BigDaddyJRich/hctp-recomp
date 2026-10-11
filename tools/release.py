# The mechanical steps of a release, one command each (the version and the notes are decided before):
#   python tools/release.py status                  this version, the last release, the commits since it
#   python tools/release.py set-version X.Y.Z       the version in its one place (the settings program's project)
#   python tools/release.py notes X.Y.Z             CHANGELOG.md's section for X.Y.Z -> dist/release-notes.md
#   python tools/release.py check-zip X.Y.Z         dist/<zip>: its list of files, its version, its size
#   python tools/release.py publish X.Y.Z [--repo owner/name] [--zip file] [--notes file]
#                                                   tags vX.Y.Z on this commit, pushes the tag, makes the GitHub
#                                                   release with the notes and the zip (nothing else is pushed here)
#   python tools/release.py verify X.Y.Z [--repo owner/name] [--zip file]
#                                                   asks GitHub what the settings program asks (the latest release)
#                                                   and checks the version, the zip, its checksum and the download link
# The zip keeps the same name in every release (make_release.py ZIP_NAME), so releases/latest/download/<name> always
# gets the newest; the settings program finds the newest at api.github.com/repos/<repo>/releases/latest.
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.request
import zipfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GITHUB_REPO = 'jcsix694/def-jam-vendetta-recomp'
CSPROJ = os.path.join(REPO, 'settings-app', 'VendettaSettings.csproj')
CHANGELOG = os.path.join(REPO, 'CHANGELOG.md')
DIST = os.path.join(REPO, 'dist')
ZIP_NAME = 'Def-Jam-Vendetta-Recompiled-Windows.zip'
TOP = 'Def Jam Vendetta Recompiled'
NOTES = os.path.join(DIST, 'release-notes.md')
# --zip / --notes (testing with a throwaway repository): another file uploaded under the zip's name, other notes
OPTIONS = {'zip': os.path.join(DIST, ZIP_NAME), 'notes': NOTES}


def run(*cmd, check=True):
    result = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True, encoding='utf-8', errors='replace')
    if check and result.returncode != 0:
        raise SystemExit(f'{" ".join(cmd)} failed:\n{result.stdout}{result.stderr}')
    return result.stdout.strip()


def valid(version):
    if not re.fullmatch(r'\d+\.\d+\.\d+', version):
        raise SystemExit(f'not a version: {version} (like 0.1.0)')
    return version


def current_version():
    text = open(CSPROJ, encoding='utf-8').read()
    return re.search(r'<Version>([^<]+)</Version>', text).group(1).strip()


def last_tag():
    tags = run('git', 'tag', '--list', 'v*', '--sort=-v:refname').splitlines()
    return tags[0] if tags else None


def status():
    tag = last_tag()
    print(f'this version (settings program): {current_version()}')
    print(f'last release tag: {tag or "none (this is the first release)"}')
    rng = f'{tag}..HEAD' if tag else 'HEAD'
    log = run('git', 'log', '--format=%h %s', rng)
    print(f'commits since {tag or "the start"}: {len(log.splitlines())}')
    print(log)


def set_version(version):
    text = open(CSPROJ, encoding='utf-8').read()
    new = re.sub(r'<Version>[^<]+</Version>', f'<Version>{version}</Version>', text, count=1)
    open(CSPROJ, 'w', encoding='utf-8', newline='').write(new)
    print(f'version {current_version()}')


def section(version):
    """The CHANGELOG section for this version: from its "## <version>" heading to the next "## " heading."""
    text = open(CHANGELOG, encoding='utf-8').read().replace('\r\n', '\n')
    m = re.search(rf'^## {re.escape(version)}\b.*$', text, re.M)
    if not m:
        raise SystemExit(f'CHANGELOG.md has no "## {version}" section')
    heading = m.group(0)
    if 'DRAFT' in heading.upper():
        raise SystemExit(f'the {version} section is still marked as a draft: {heading}')
    rest = text[m.end():]
    end = re.search(r'^## ', rest, re.M)
    body = (rest[:end.start()] if end else rest).strip()
    body = re.sub(r'\n-{3,}\s*$', '', body).strip()  # a section's closing rule
    if not body:
        raise SystemExit(f'the {version} section is empty')
    return heading, body


def notes(version):
    heading, body = section(valid(version))
    os.makedirs(DIST, exist_ok=True)
    open(NOTES, 'w', encoding='utf-8', newline='\n').write(body + '\n')
    print(f'{heading}\n-> {NOTES}\n')
    print(body)


def check_zip(version):
    path = os.path.join(DIST, ZIP_NAME)
    if not os.path.isfile(path):
        raise SystemExit(f'{path} is missing: run make-release.bat')
    with zipfile.ZipFile(path) as z:
        names = set(z.namelist())
        manifest = f'{TOP}/files/release-manifest.txt'
        if manifest not in names:
            raise SystemExit('the zip has no files/release-manifest.txt')
        lines = z.read(manifest).decode('utf-8').splitlines()
        if not lines or not lines[0].strip().endswith(version):
            raise SystemExit(f'the zip is for another version: {lines[0] if lines else "?"}')
        listed = [l for l in lines if l and not l.startswith('#')]
        missing = [l.split(' ', 2)[2] for l in listed if f'{TOP}/{l.split(" ", 2)[2]}' not in names]
        if missing:
            raise SystemExit(f'files listed but not in the zip: {missing[:5]}')
        for needed in (f'{TOP}/Def Jam Vendetta Recompiled Settings.exe', f'{TOP}/READ ME FIRST.txt'):
            if needed not in names:
                raise SystemExit(f'missing: {needed}')
    size = os.path.getsize(path)
    sha = hashlib.sha256(open(path, 'rb').read()).hexdigest()
    print(f'{ZIP_NAME}: {size / 1e6:.1f} MB, {len(listed)} files, version {version}, sha256 {sha}')


def publish(version, repo):
    tag = 'v' + valid(version)
    path, notes_file = OPTIONS['zip'], OPTIONS['notes']
    if not os.path.isfile(notes_file) or not os.path.isfile(path):
        raise SystemExit('run "notes" and make-release.bat first')
    if repo == GITHUB_REPO and (path != os.path.join(DIST, ZIP_NAME) or notes_file != NOTES):
        raise SystemExit('--zip / --notes are for a test repository only')
    if os.path.basename(path) != ZIP_NAME:
        raise SystemExit(f'the file must be called {ZIP_NAME} (the README\'s link and the settings program look for it)')
    cmd = ['gh', 'release', 'create', tag, path, '--repo', repo, '--title',
           f'Def Jam Vendetta Recompiled {version}', '--notes-file', notes_file, '--latest']
    if repo == GITHUB_REPO:
        # the real release: from a commit that is on GitHub, tagged here
        if run('git', 'status', '--porcelain'):
            raise SystemExit('uncommitted changes: a release comes from a commit')
        head = run('git', 'rev-parse', 'HEAD')
        run('git', 'fetch', '-q', 'origin')
        if run('git', 'rev-parse', 'origin/master') != head:
            raise SystemExit('this commit is not on GitHub yet: push first')
        if run('git', 'tag', '--list', tag):
            raise SystemExit(f'{tag} exists already')
        run('git', 'tag', '-a', tag, '-m', f'Def Jam Vendetta Recompiled {version}')
        run('git', 'push', 'origin', tag)
        cmd.append('--verify-tag')
    # (a test repository: GitHub makes the tag there, on its default branch)
    run(*cmd)
    print(f'published {tag} on {repo}')


def verify(version, repo):
    """What the settings program sees: the latest release, its zip, the checksum, the download."""
    data = json.loads(run('gh', 'api', f'repos/{repo}/releases/latest'))
    tag = data.get('tag_name')
    assets = [a for a in data.get('assets', []) if a.get('name') == ZIP_NAME]
    print(f'latest: {tag} ({data.get("html_url")})')
    if tag != 'v' + version:
        raise SystemExit(f'the latest release is {tag}, not v{version}')
    if not assets:
        raise SystemExit(f'no {ZIP_NAME} on the release')
    asset = assets[0]
    digest = asset.get('digest') or ''
    print(f'zip: {asset["size"] / 1e6:.1f} MB, digest {digest or "none"}')
    local = OPTIONS['zip']
    if os.path.isfile(local) and digest.startswith('sha256:'):
        same = hashlib.sha256(open(local, 'rb').read()).hexdigest() == digest[7:]
        print(f'the checksum matches {local}' if same else f'the checksum DIFFERS from {local}')
    visibility = json.loads(run('gh', 'repo', 'view', repo, '--json', 'visibility')).get('visibility')
    if visibility == 'PUBLIC':
        # the README's link and the settings program's request, without signing in
        link = f'https://github.com/{repo}/releases/latest/download/{ZIP_NAME}'
        request = urllib.request.Request(link, method='HEAD', headers={'User-Agent': 'release-check'})
        with urllib.request.urlopen(request) as response:
            print(f'download link: {response.status} ({link})')
        api = urllib.request.Request(f'https://api.github.com/repos/{repo}/releases/latest',
                                     headers={'User-Agent': 'release-check', 'Accept': 'application/vnd.github+json'})
        with urllib.request.urlopen(api) as response:
            print(f'without signing in, the latest is {json.load(response).get("tag_name")}')
    else:
        print(f'{repo} is {visibility}: players (not signed in) can\'t see this release until it is public')


def main():
    args = sys.argv[1:]
    repo = GITHUB_REPO
    if '--repo' in args:
        i = args.index('--repo')
        repo = args[i + 1]
        del args[i:i + 2]
    for name in ('zip', 'notes'):
        if f'--{name}' in args:
            i = args.index(f'--{name}')
            OPTIONS[name] = os.path.abspath(args[i + 1])
            del args[i:i + 2]
    if not args:
        raise SystemExit(__doc__ or 'see the top of this file')
    command, rest = args[0], args[1:]
    if command == 'status':
        status()
    elif command == 'set-version' and rest:
        set_version(valid(rest[0]))
    elif command == 'notes' and rest:
        notes(rest[0])
    elif command == 'check-zip' and rest:
        check_zip(valid(rest[0]))
    elif command == 'publish' and rest:
        publish(rest[0], repo)
    elif command == 'verify' and rest:
        verify(valid(rest[0]), repo)
    else:
        raise SystemExit('commands: status, set-version X.Y.Z, notes X.Y.Z, check-zip X.Y.Z, publish X.Y.Z, verify X.Y.Z')


if __name__ == '__main__':
    main()
