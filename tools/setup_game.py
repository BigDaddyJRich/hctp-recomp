# A player's first-run setup, step by step (run by Vendetta Settings; also by hand):
#   python tools/setup_game.py --data <install folder> [--disc <disc.chd | disc.iso>]
# 1. the disc -> disc/ + game/ in the install folder, 2. Microsoft's C++ build tools (installed when missing: one
# Windows prompt), 3. PS2Recomp's recompiler, 4. the game's code -> C++, 5. the game, 6. "Def Jam Vendetta
# Recompiled.exe" in the install folder. The build folders are build/recomp and build/game there.
#
# What it prints for a program to follow (each on its own line, besides everything the tools print):
#   [step] <n> <of> <title>    [progress] <0-1 within the step>    [done]    [error] <what went wrong>
# The libraries PS2Recomp's CMake would clone with git come with a release (source/deps/<name>), so no git is needed.
import argparse
import ctypes
import os
import re
import shutil
import subprocess
import sys
import tempfile
import urllib.request

SRC = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PS2RECOMP = os.path.join(SRC, 'PS2Recomp')
GAME_EXE = 'Def Jam Vendetta Recompiled.exe'
VS_INSTALLER_URL = 'https://aka.ms/vs/17/release/vs_BuildTools.exe'
VS_COMPONENTS = ['Microsoft.VisualStudio.Workload.VCTools', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                 'Microsoft.VisualStudio.Component.VC.CMake.Project', 'Microsoft.VisualStudio.Component.Windows11SDK.22621']
# FetchContent name -> the folder a release brings it in (source/deps/<folder>)
DEPS = {'ELFIO': 'elfio', 'TOML11': 'toml11', 'FMT': 'fmt', 'LIBDWARF': 'libdwarf', 'RABBITIZER': 'rabbitizer',
        'RAYLIB': 'raylib', 'IMGUI': 'imgui', 'RLIMGUI': 'rlimgui'}
STEPS = ['Your disc', 'Build tools', 'The recompiler', 'Recompiling the game', 'Building the game', 'Finishing']


def say(text):
    print(text, flush=True)


def step(n, title):
    say(f'[step] {n} {len(STEPS)} {title}')


def progress(fraction):
    say(f'[progress] {max(0.0, min(1.0, fraction)):.3f}')


class SetupError(Exception):
    pass


def run(cmd, env=None, cwd=None, parse=None, log_name=None):
    """Runs a command, passing its lines on (and to parse(line) for progress)."""
    say('> ' + ' '.join(f'"{c}"' if ' ' in c else c for c in cmd))
    if env and not os.path.isabs(cmd[0]):
        # Windows looks a program up on this process's PATH, not env's: find cmake / ninja on the build tools' PATH
        found = shutil.which(cmd[0], path=env.get('PATH', env.get('Path', '')))
        if found:
            cmd = [found] + cmd[1:]
    proc = subprocess.Popen(cmd, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                            text=True, encoding='utf-8', errors='replace', bufsize=1)
    for line in proc.stdout:
        line = line.rstrip()
        print(line, flush=True)
        if parse:
            parse(line)
    if proc.wait() != 0:
        raise SetupError(f'{os.path.basename(cmd[0])} failed (exit code {proc.returncode})'
                         + (f'; see {log_name}' if log_name else ''))


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


# --- 2. the disc ----------------------------------------------------------------------------------------------------

def step_disc(data, disc):
    step(1, STEPS[0])
    elf = os.path.join(data, 'game', 'SLUS_206.39')
    iso = os.path.join(data, 'disc', 'SLUS-20639.iso')
    if not disc:
        if os.path.isfile(elf) and os.path.isfile(iso):
            say('the disc is already prepared')
            progress(1.0)
            return
        raise SetupError('no disc chosen')
    env = dict(os.environ, VENDETTA_DATA=data)

    def parse(line):
        m = re.match(r'(\d+)/(\d+) sectors', line)
        if m:
            progress(0.9 * int(m.group(1)) / max(1, int(m.group(2))))
    run([sys.executable, os.path.join(SRC, 'tools', 'prepare_disc.py'), disc], env=env, parse=parse)
    if not os.path.isfile(elf):
        raise SetupError('the disc was read but SLUS_206.39 is missing')
    progress(1.0)


# --- 3-5. building ----------------------------------------------------------------------------------------------------

def deps_args():
    args = []
    for name, folder in DEPS.items():
        path = os.path.join(SRC, 'deps', folder)
        if os.path.isdir(path):
            args.append(f'-DFETCHCONTENT_SOURCE_DIR_{name}={path}')
    return args


def build_jobs():
    """Parallel compiles: the cores, but about one per 3 GB of memory (the game's big files need ~2 GB each)."""
    class Status(ctypes.Structure):
        _fields_ = [('length', ctypes.c_ulong), ('load', ctypes.c_ulong), ('total', ctypes.c_ulonglong),
                    ('free', ctypes.c_ulonglong), ('pagetotal', ctypes.c_ulonglong), ('pagefree', ctypes.c_ulonglong),
                    ('virtualtotal', ctypes.c_ulonglong), ('virtualfree', ctypes.c_ulonglong), ('extended', ctypes.c_ulonglong)]
    status = Status()
    status.length = ctypes.sizeof(Status)
    gb = 8
    if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
        gb = status.total // (1 << 30)
    return max(1, min(os.cpu_count() or 2, int(gb // 3)))


def ninja_progress(start, end):
    def parse(line):
        m = re.match(r'\[(\d+)/(\d+)\]', line)
        if m:
            progress(start + (end - start) * int(m.group(1)) / max(1, int(m.group(2))))
    return parse


def step_recompiler(env, data):
    step(3, STEPS[2])
    build = os.path.join(data, 'build', 'recomp')
    run(['cmake', '-S', PS2RECOMP, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DPS2X_BUILD_RUNTIME=OFF',
         '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF', '-DPS2X_BUILD_ANALYZER=OFF'] + deps_args(), env=env)
    progress(0.1)
    run(['cmake', '--build', build, '--', f'-j{build_jobs()}'], env=env, parse=ninja_progress(0.1, 1.0))
    exe = os.path.join(build, 'ps2xRecomp', 'ps2_recomp.exe')
    if not os.path.isfile(exe):
        raise SetupError('the recompiler was not built')
    progress(1.0)
    return exe


def step_recompile(recompiler, data):
    step(4, STEPS[3])
    env = dict(os.environ, VENDETTA_DATA=data)
    logs = os.path.join(data, 'logs')
    os.makedirs(logs, exist_ok=True)
    run([sys.executable, os.path.join(SRC, 'tools', 'make_config.py')], env=env, cwd=SRC)
    progress(0.05)
    log = os.path.join(logs, 'recomp.log')
    say(f'> {recompiler} config/vendetta.toml (its output: {log})')
    with open(log, 'w', encoding='utf-8', errors='replace') as out:
        code = subprocess.run([recompiler, os.path.join(SRC, 'config', 'vendetta.toml')], cwd=SRC, env=env,
                              stdout=out, stderr=subprocess.STDOUT).returncode
    if code != 0:
        raise SetupError(f'the recompiler failed (exit code {code}); see {log}')
    progress(0.75)
    run([sys.executable, os.path.join(SRC, 'tools', 'vu1_recomp.py')], env=env, cwd=SRC)
    progress(0.85)
    run([sys.executable, os.path.join(SRC, 'tools', 'sync_generated.py')], env=env, cwd=SRC)
    progress(1.0)


def step_game(env, data):
    step(5, STEPS[4])
    build = os.path.join(data, 'build', 'game')
    # the same build as build-fast.bat (out\fast)
    run(['cmake', '-S', PS2RECOMP, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
         '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG /arch:AVX2', '-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /DNDEBUG',
         '-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF', '-DPS2X_BUILD_RECOMP=OFF',
         '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF', '-DPS2X_BUILD_ANALYZER=OFF', '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF',
         '-DPS2X_SHOW_WINDOWS_CONSOLE=OFF',  # a windowed program: no terminal behind the game (its output: logs\)
         f'-DPS2X_PARALLEL_GS_DIR={os.path.join(SRC, "third_party", "parallel-gs")}',
         # double-click the game: it finds game/SLUS_206.39 beside itself (main.cpp)
         '-DPS2X_DEFAULT_BOOT_ELF=game/SLUS_206.39'] + deps_args(), env=env)
    progress(0.03)
    run(['cmake', '--build', build, '--', f'-j{build_jobs()}'], env=env, parse=ninja_progress(0.03, 1.0))
    progress(1.0)
    return os.path.join(build, 'ps2xRuntime')


def step_finish(out_dir, data):
    step(6, STEPS[5])
    exe = os.path.join(out_dir, 'ps2EntryRunner.exe')
    if not os.path.isfile(exe):
        raise SetupError('the game was not built')
    shutil.copyfile(exe, os.path.join(data, GAME_EXE))
    for name in os.listdir(out_dir):  # the DLLs it needs (ffmpeg)
        if name.lower().endswith('.dll'):
            shutil.copyfile(os.path.join(out_dir, name), os.path.join(data, name))
    say(f'the game: {os.path.join(data, GAME_EXE)}')
    progress(1.0)


def main():
    parser = argparse.ArgumentParser(description='First-run setup: your disc -> Def Jam Vendetta Recompiled.exe')
    parser.add_argument('--data', required=True, help='the install folder (disc/, game/, the game program, build/)')
    parser.add_argument('--disc', help='your disc: .chd or .iso (USA, SLUS-20639)')
    args = parser.parse_args()
    data = os.path.abspath(args.data)
    try:
        step_disc(data, args.disc)
        env = step_tools()
        recompiler = step_recompiler(env, data)
        step_recompile(recompiler, data)
        out_dir = step_game(env, data)
        step_finish(out_dir, data)
    except SetupError as error:
        say(f'[error] {error}')
        return 1
    except Exception as error:  # anything unexpected: still a clear last line
        say(f'[error] {type(error).__name__}: {error}')
        return 1
    say('[done]')
    return 0


if __name__ == '__main__':
    sys.exit(main())
