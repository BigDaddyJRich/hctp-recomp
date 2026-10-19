# HCTP first-run setup (the build kit's "Build HCTP.bat" runs it; also by hand):
#   python tools/hctp_setup.py --data <install folder> [--disc <disc.iso | disc.chd | folder with SLUS_207.87>]
# 1. your game -> <data>/game/, 2. Microsoft's C++ build tools (installed when missing), 3. PS2Recomp's recompiler,
# 4. the game's code -> C++ (on this PC only), 5. the native Windows build (D3D11 host, as build-native.bat),
# 6. ps2EntryRunner.exe + DLLs into <data>/game/ (the game runs from its own folder) and "Play HCTP.bat" beside it.
# Same [step]/[progress]/[error] lines as setup_game.py (whose helpers it uses).
import argparse
import os
import re
import shutil
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import setup_game as base  # noqa: E402
from setup_game import SetupError, progress, run, say, step  # noqa: E402

SRC = base.SRC
PS2RECOMP = base.PS2RECOMP
ELF = 'SLUS_207.87'
base.STEPS[:] = ['Your game', 'Build tools', 'The recompiler', 'Recompiling the game', 'Building the game', 'Finishing']


def step_disc(data, disc):
    step(1, base.STEPS[0])
    elf = os.path.join(data, 'game', ELF)
    if not disc:
        if os.path.isfile(elf):
            say('the game files are already in place')
            progress(1.0)
            return
        raise SetupError('no game chosen (an .iso / .chd of the USA disc, or a folder with SLUS_207.87)')

    def parse(line):
        m = re.match(r'(\d+)/(\d+) files', line)
        if m:
            progress(0.95 * int(m.group(1)) / max(1, int(m.group(2))))
    run([sys.executable, os.path.join(SRC, 'tools', 'hctp_prepare_disc.py'), disc],
        env=dict(os.environ, HCTP_DATA=data), parse=parse)
    if not os.path.isfile(elf):
        raise SetupError(f'the game was read but {ELF} is missing')
    progress(1.0)


def write_config(data):
    """config_hctp/hctp.toml with this PC's paths (the committed one has the developer's)."""
    with open(os.path.join(SRC, 'config_hctp', 'hctp.toml'), encoding='utf-8') as f:
        text = f.read()
    elf = os.path.join(data, 'game', ELF).replace('\\', '/')
    out = os.path.join(SRC, 'generated').replace('\\', '/')
    text = re.sub(r'(?m)^input = ".*"$', f'input = "{elf}"', text, count=1)
    text = re.sub(r'(?m)^output = ".*"$', f'output = "{out}"', text, count=1)
    os.makedirs(os.path.join(data, 'build'), exist_ok=True)
    path = os.path.join(data, 'build', 'hctp.toml')
    with open(path, 'w', encoding='utf-8') as f:
        f.write(text)
    os.makedirs(out, exist_ok=True)
    return path


def step_recompile(recompiler, data):
    step(4, base.STEPS[3])
    logs = os.path.join(data, 'logs')
    os.makedirs(logs, exist_ok=True)
    config = write_config(data)
    say("Turning the game's code into C++")
    log = os.path.join(logs, 'recomp.log')
    say(f'> {recompiler} {config} (its output: {log})')
    with open(log, 'w', encoding='utf-8', errors='replace') as out:
        code = subprocess.run([recompiler, config], cwd=SRC, stdout=out, stderr=subprocess.STDOUT).returncode
    if code != 0:
        raise SetupError(f'the recompiler failed (exit code {code}); see {log}')
    progress(0.15)
    run([sys.executable, os.path.join(SRC, 'tools', 'vu1_recomp.py'), os.path.join(data, 'game', ELF),
         os.path.join(SRC, 'generated', 'vu1_compiled.cpp')], cwd=SRC)
    progress(0.18)
    run([sys.executable, os.path.join(SRC, 'tools', 'sync_generated.py')], cwd=SRC,
        parse=base.sync_progress(0.18, 1.0))
    progress(1.0)


def step_game(env, data):
    step(5, base.STEPS[4])
    build = os.path.join(data, 'build', 'game')
    # the same build as build-native.bat (Win32 + Direct3D 11 + XInput + XAudio2 host, no paraLLEl-GS)
    run(['cmake', '-S', PS2RECOMP, '-B', build, '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=RelWithDebInfo',
         '-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG /arch:AVX2', '-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /DNDEBUG',
         '-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF', '-DPS2X_BUILD_RECOMP=OFF',
         '-DPS2X_BUILD_TEST=OFF', '-DPS2X_BUILD_STUDIO=OFF', '-DPS2X_BUILD_ANALYZER=OFF',
         '-DPS2X_ENABLE_AGRESSIVE_LOGS=OFF', '-DPS2X_NATIVE_HOST=ON', '-DPS2X_ENABLE_DEBUG_UI=OFF',
         f'-DPS2X_DEFAULT_BOOT_ELF={ELF}', '-DPS2X_PARALLEL_GS_DIR='] + base.deps_args(), env=env)
    progress(0.03)
    base.build_with_retry(build, env, 0.03, 1.0)
    progress(1.0)
    return os.path.join(build, 'ps2xRuntime')


PLAY_BAT = '@echo off\r\ncd /d "%~dp0game"\r\nstart "" ps2EntryRunner.exe\r\n'


def step_finish(out_dir, data):
    step(6, base.STEPS[5])
    exe = os.path.join(out_dir, 'ps2EntryRunner.exe')
    if not os.path.isfile(exe):
        raise SetupError('the game was not built')
    game = os.path.join(data, 'game')
    files = [exe] + [os.path.join(out_dir, n) for n in os.listdir(out_dir) if n.lower().endswith('.dll')]
    for source in files:
        shutil.copyfile(source, os.path.join(game, os.path.basename(source) + '.new'))
    for source in files:
        name = os.path.basename(source)
        os.replace(os.path.join(game, name + '.new'), os.path.join(game, name))
    with open(os.path.join(data, 'Play HCTP.bat'), 'w', newline='') as f:
        f.write(PLAY_BAT)
    say(f'the game: {os.path.join(game, "ps2EntryRunner.exe")} (start it with "Play HCTP.bat")')
    progress(1.0)


def main():
    parser = argparse.ArgumentParser(description='First-run setup: your HCTP disc -> a native PC build')
    parser.add_argument('--data', required=True, help='the install folder (game/, build/, logs/)')
    parser.add_argument('--disc', help='your game: .iso / .chd of the USA disc, or a folder with SLUS_207.87')
    args = parser.parse_args()
    data = os.path.abspath(args.data)
    try:
        step_disc(data, args.disc)
        env = base.step_tools()
        recompiler = base.step_recompiler(env, data)
        step_recompile(recompiler, data)
        out_dir = step_game(env, data)
        step_finish(out_dir, data)
    except SetupError as error:
        where = f'{base.STEPS[base.current_step - 1]}: ' if 1 <= base.current_step <= len(base.STEPS) else ''
        message = where + str(error)
        for pattern, meaning in base.KNOWN_ERRORS:
            if any(pattern.search(line) for line in error.details):
                message += '. ' + meaning
                break
        for line in error.details:
            say(f'[detail] {line}')
        say(f'[error] {message}')
        return 1
    except Exception as error:
        say(f'[error] {type(error).__name__}: {error}')
        return 1
    say('[done]')
    return 0


if __name__ == '__main__':
    sys.exit(main())
