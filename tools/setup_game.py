# The setup's shared steps and helpers; tools/hctp_setup.py is the setup itself (the launcher runs it):
# Microsoft's C++ build tools (installed when missing: one Windows prompt), PS2Recomp's recompiler, the build with
# retries, and the lines it prints for a program to follow (each on its own line, besides what the tools print):
#   [step] <n> <of> <title>    [progress] <0-1 within the step>    [done]    [error] <what went wrong>
#   [detail] <a line of the tool's own error, shown under the [error] line; they come just before it>
# The libraries PS2Recomp's CMake would clone with git come with a release (source/deps/<name>), so no git is needed.
import argparse
import collections
import ctypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.request

SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PS2RECOMP = os.path.join(SRC, 'PS2Recomp')
VS_INSTALLER_URL = 'https://aka.ms/vs/17/release/vs_BuildTools.exe'
VS_COMPONENTS = ['Microsoft.VisualStudio.Workload.VCTools', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                 'Microsoft.VisualStudio.Component.VC.CMake.Project', 'Microsoft.VisualStudio.Component.Windows11SDK.22621']
# FetchContent name -> the folder a release brings it in (source/deps/<folder>)
DEPS = {'ELFIO': 'elfio', 'TOML11': 'toml11', 'FMT': 'fmt', 'LIBDWARF': 'libdwarf', 'RABBITIZER': 'rabbitizer',
        'RAYLIB': 'raylib', 'IMGUI': 'imgui', 'RLIMGUI': 'rlimgui'}
STEPS = ['Your disc', 'Build tools', 'The recompiler', 'Recompiling the game', 'Building the game', 'Finishing']


def say(text):
    print(text, flush=True)


current_step = 0


def step(n, title):
    global current_step
    current_step = n
    say(f'[step] {n} {len(STEPS)} {title}')


def progress(fraction):
    say(f'[progress] {max(0.0, min(1.0, fraction)):.3f}')


class SetupError(Exception):
    def __init__(self, message, details=()):
        super().__init__(message)
        self.details = list(details)  # the tool's own error lines


# the lines of a failed tool's output worth showing a player (the compiler's, the linker's, CMake's, Ninja's errors)
ERROR_LINE = re.compile(r'\berror\b|\bfatal\b|^FAILED:|CMake Error|cannot open|out of (heap|memory)|Permission denied',
                        re.IGNORECASE)
# what a few well known errors mean, in plain words
KNOWN_ERRORS = [
    (re.compile(r'C1060|C1002|C3859|out of (heap|memory)|not enough (memory|space)|insufficient memory', re.IGNORECASE),
     'Your PC ran out of memory while building. Close other programs (browsers, games) and try again.'),
    (re.compile(r'LNK1104|LNK1201|Permission denied|being used by another process', re.IGNORECASE),
     "Another program held one of the build's files, often an antivirus. Try again."),
]


def error_details(lines, keep=6, width=240):
    """The last few error lines of a tool's output (or its last lines, when none look like errors)."""
    picked = [line for line in lines if ERROR_LINE.search(line)] or list(lines)
    return [line.strip()[:width] for line in picked[-keep:] if line.strip()]


def run(cmd, env=None, cwd=None, parse=None, log_name=None, low_priority=False):
    """Runs a command, passing its lines on (and to parse(line) for progress). low_priority: below normal (the
    compilers it starts inherit it), so the PC stays usable during the long game build."""
    say('> ' + ' '.join(f'"{c}"' if ' ' in c else c for c in cmd))
    if env and not os.path.isabs(cmd[0]):
        # Windows looks a program up on this process's PATH, not env's: find cmake / ninja on the build tools' PATH
        found = shutil.which(cmd[0], path=env.get('PATH', env.get('Path', '')))
        if found:
            cmd = [found] + cmd[1:]
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding='utf-8', errors='replace', bufsize=1,
                            creationflags=0x4000 if low_priority and os.name == 'nt' else 0)  # BELOW_NORMAL_PRIORITY_CLASS
    last = collections.deque(maxlen=400)
    for line in proc.stdout:
        line = line.rstrip()
        print(line, flush=True)
        last.append(line)
        if parse:
            parse(line)
    if proc.wait() != 0:
        raise SetupError(f'{os.path.basename(cmd[0])} failed (exit code {proc.returncode})'
                         + (f'; see {log_name}' if log_name else ''), error_details(last))


# --- 1. the build tools ---------------------------------------------------------------------------------------------

def vswhere():
    path = os.path.join(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'),
                        'Microsoft Visual Studio', 'Installer', 'vswhere.exe')
    return path if os.path.isfile(path) else None


def find_vcvars():
    tool = vswhere()
    if not tool:
        return None
    out = subprocess.run([tool, '-latest', '-products', '*', '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                          '-property', 'installationPath'], capture_output=True, text=True).stdout.strip().splitlines()
    for install in out:
        vcvars = os.path.join(install.strip(), 'VC', 'Auxiliary', 'Build', 'vcvars64.bat')
        if os.path.isfile(vcvars):
            return vcvars
    return None


def build_environment(vcvars):
    """The environment vcvars64.bat sets (the compiler, the Windows SDK, and VS's CMake / Ninja on PATH)."""
    out = subprocess.run(f'cmd /s /c ""{vcvars}" >nul && set"', capture_output=True, text=True, shell=True).stdout
    env = {}
    for line in out.splitlines():
        if '=' in line:
            key, _, value = line.partition('=')
            env[key] = value
    if 'INCLUDE' not in env:
        raise SetupError('the build tools did not set up (vcvars64.bat)')
    return env


def has_tools(env):
    path = env.get('PATH', env.get('Path', ''))
    return all(shutil.which(tool, path=path) for tool in ('cl', 'cmake', 'ninja'))


def install_build_tools():
    say("Microsoft's C++ build tools are missing: downloading Microsoft's installer (free; a Windows prompt asks to allow it)")
    installer = os.path.join(tempfile.gettempdir(), 'vs_BuildTools.exe')
    with urllib.request.urlopen(VS_INSTALLER_URL) as response, open(installer, 'wb') as out:
        shutil.copyfileobj(response, out)
    cmd = [installer, '--passive', '--wait', '--norestart']
    for component in VS_COMPONENTS:
        cmd += ['--add', component]
    say('> ' + ' '.join(cmd))
    code = subprocess.run(cmd).returncode
    if code not in (0, 3010):  # 3010: done, a restart recommended
        raise SetupError(f"Microsoft's installer stopped (exit code {code})")


def step_tools():
    step(2, STEPS[1])
    vcvars = find_vcvars()
    env = build_environment(vcvars) if vcvars else None
    if not env or not has_tools(env):
        install_build_tools()
        progress(0.9)
        vcvars = find_vcvars()
        env = build_environment(vcvars) if vcvars else None
        if not env or not has_tools(env):
            raise SetupError('the build tools are still missing after the installer (C++, CMake and Ninja are needed)')
    say(f'build tools: {vcvars}')
    progress(1.0)
    return env


# --- 3-5. building ----------------------------------------------------------------------------------------------------

def deps_args():
    args = []
    for name, folder in DEPS.items():
        path = os.path.join(SRC, 'deps', folder)
        if os.path.isdir(path):
            args.append(f'-DFETCHCONTENT_SOURCE_DIR_{name}={path}')
    return args


def build_jobs():
    """Parallel compiles: the cores, but about one per 3 GB of memory and one per 2 GB of commit free right now (the
    game's big files need ~2 GB each, and a browser or a game left open leaves less than the PC has). Commit (memory +
    page file) is what runs out when the compiler stops with "out of heap space" (C1060); short of physical memory it
    only pages."""
    class Status(ctypes.Structure):
        _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                    ('free', ctypes.c_ulonglong), ('pagetotal', ctypes.c_ulonglong), ('pagefree', ctypes.c_ulonglong),
                    ('virtualtotal', ctypes.c_ulonglong), ('virtualfree', ctypes.c_ulonglong), ('extended', ctypes.c_ulonglong)]
    status = Status()
    status.length = ctypes.sizeof(Status)
    total_gb, avail_gb, free_gb = 8.0, 4.0, 8.0
    if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        total_gb = status.total / (1 << 30)
        avail_gb = status.free / (1 << 30)      # physical memory free right now (after Windows and open programs)
        free_gb = status.pagefree / (1 << 30)   # commit free (memory + page file)
    # 0.3.7: test switch HCTP_SETUP_MEMORY_GB="<free physical>,<commit free>" (a big PC acting as an 8 GB one)
    if os.environ.get('HCTP_SETUP_MEMORY_GB'):
        avail_gb, free_gb = (float(v) for v in os.environ['HCTP_SETUP_MEMORY_GB'].split(','))
    # 0.3.7 (8 GB PCs): count the memory FREE now, keep RESERVE_GB for Windows and what else runs, and give each
    # compile PER_JOB_GB (measured Nov 20: the biggest single compile after the 64 KB unity limit, with margin).
    # Total memory alone let an 8 GB PC with a browser open run 2 compiles of up to 3.1 GB: it paged until it crawled.
    jobs = max(1, min(os.cpu_count() or 2, int((avail_gb - RESERVE_GB) // PER_JOB_GB), int(free_gb // PER_JOB_GB)))
    say(f'{jobs} compiles at a time ({total_gb:.0f} GB of memory, {avail_gb:.1f} GB free now, {free_gb:.0f} GB of commit free)')
    return jobs


RESERVE_GB = 1.0  # left for Windows and the programs already open (they are already counted out of "free now")
PER_JOB_GB = 2.0  # the biggest single compile of the game build: 1.9 GB (measured Nov 20, whole build under a 2.5 GB cap)


def build_with_retry(build_dir, env, start, end):
    """cmake --build, with progress from start to end within the step. When it fails it is tried once more with one
    or two compiles at a time: Ninja carries on from where it stopped, and with little running at once a PC short on
    memory gets through, as does a file an antivirus held for a moment. A real error fails again quickly."""
    at = [start]

    def parse(line):
        m = re.match(r'\[(\d+)/(\d+)\]', line)
        if m:  # Ninja counts what is left: the second try continues the bar from where the first one stopped
            at[0] = max(at[0], first + (end - first) * int(m.group(1)) / max(1, int(m.group(2))))
            progress(at[0])

    jobs = build_jobs()
    first = start
    try:
        run(['cmake', '--build', build_dir, '--', f'-j{jobs}'], env=env, parse=parse, low_priority=True)
    except SetupError as error:
        retry_jobs = 1 if jobs <= 2 else 2
        say(f'[retry] The build stopped ({error}). Trying again with {retry_jobs} at a time')
        for line in error.details:
            say(f'  {line}')
        time.sleep(5)
        first = at[0]
        run(['cmake', '--build', build_dir, '--', f'-j{retry_jobs}'], env=env, parse=parse, low_priority=True)


def step_recompiler(env, data):
    step(3, STEPS[2])
    build = os.path.join(data, 'build', 'recomp')
    run(['cmake', '-S', PS2RECOMP, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DPS2X_BUILD_RUNTIME=OFF',
         '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF', '-DPS2X_BUILD_ANALYZER=OFF'] + deps_args(), env=env)
    progress(0.1)
    build_with_retry(build, env, 0.1, 1.0)
    exe = os.path.join(build, 'ps2xRecomp', 'ps2_recomp.exe')
    if not os.path.isfile(exe):
        raise SetupError('the recompiler was not built')
    progress(1.0)
    return exe


def sync_progress(start, end):
    """sync_generated.py's "Checking which files changed: N of T" lines, as progress within the step."""
    def parse(line):
        m = re.match(r'Checking which files changed: ([\d,]+) of ([\d,]+)', line)
        if m:
            done, total = (int(g.replace(',', '')) for g in m.groups())
            progress(start + (end - start) * done / max(1, total))
    return parse


