# Builds the recompiler's inputs from the committed sources (both outputs are generated + git-ignored):
#   config/vendetta_functions.csv = config/vendetta_functions.ghidra.csv (Ghidra's export, untouched)
#                                   + config/missing_starts.csv (tools/find_missing_starts.py)
#   config/vendetta.toml          = config/vendetta.ghidra.toml (Ghidra's export) with this PC's paths and its `stubs`
#                                   = config/manual_stubs.txt (found by hand; wins) + Ghidra's + the analyzer's
#                                   (config/analyzer.toml), one per address, minus manual_stubs.txt's "-name@addr"
#                                   lines (left to the game's own code)
# usage: python tools/make_config.py
import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CFG = os.path.join(ROOT, 'config')


def stubs_of(text):
    body = re.search(r'^stubs = \[(.*?)^\]', text, re.S | re.M).group(1)
    return re.findall(r'"([^"@]+)@0x([0-9A-Fa-f]+)"', body)


def main():
    with open(os.path.join(CFG, 'vendetta_functions.csv'), 'w', newline='\n') as out:
        for name in ('vendetta_functions.ghidra.csv', 'missing_starts.csv'):
            with open(os.path.join(CFG, name)) as f:
                out.write(f.read())

    ghidra = open(os.path.join(CFG, 'vendetta.ghidra.toml')).read()
    analyzer = open(os.path.join(CFG, 'analyzer.toml')).read()
    entries = [l.strip() for l in open(os.path.join(CFG, 'manual_stubs.txt')) if l.strip() and not l.startswith('#')]
    manual = [tuple(e.split('@0x')) for e in entries if not e.startswith('-')]
    # "-name@0xADDR": never bind this address — the game's own (recompiled) code runs, even if Ghidra / the analyzer
    # recognised it as a library routine (the runtime's version doesn't keep the game's own state)
    unbound = {int(e[1:].split('@0x')[1], 16) for e in entries if e.startswith('-')}
    by_addr = {}
    for name, addr in manual + stubs_of(ghidra) + stubs_of(analyzer):
        if int(addr, 16) not in unbound:
            by_addr.setdefault(int(addr, 16), name)
    lines = ''.join(f'  "{n}@0x{a:08X}",\n' for a, n in sorted(by_addr.items()))
    toml = re.sub(r'^stubs = \[.*?^\]', lambda m: 'stubs = [\n' + lines + ']', ghidra, count=1, flags=re.S | re.M)
    root = ROOT.replace(os.sep, '/')
    paths = {'input': f'{root}/game/SLUS_206.39', 'output': f'{root}/generated',
             'ghidra_output': f'{root}/config/vendetta_functions.csv'}
    for key, value in paths.items():
        toml = re.sub(rf'^{key} = .*$', f'{key} = "{value}"', toml, count=1, flags=re.M)
    with open(os.path.join(CFG, 'vendetta.toml'), 'w', newline='\n') as f:
        f.write(toml)
    print(f'make_config: {len(manual)} manual, {len(unbound)} unbound + {len(stubs_of(ghidra))} ghidra + {len(stubs_of(analyzer))} analyzer '
          f'-> {len(by_addr)} stubs')


if __name__ == '__main__':
    main()
