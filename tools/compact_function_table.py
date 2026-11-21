# Rewrites the recompiler's register_functions.cpp (runner) so it compiles in little memory (0.3.7).
#
# PS2Recomp emits the guest function table as ONE constructor with an assignment per entry: for HCTP about 413,000
# statements, 40 MB of C++ in one function. MSVC builds that whole function in memory at /O2 - the single biggest
# compile of the game build, and a reason 8 GB PCs ran out of memory. This turns the same entries, in the same order,
# into a constant table of {slot, function} pairs (static data the linker fills in) and one loop that copies it in
# the same constructor: the table the game sees is identical (same slots, same functions, later entries for the same
# slot still win).
#
# Runs AFTER the other runner patches (patch_runner_handedits.py edits register_functions.cpp's assignment lines).
# Idempotent: an already compact file is left as it is. Any line it does not recognise = FAIL, nothing written.
# usage: python tools/compact_function_table.py <runner dir>
import os
import re
import sys

HEAD = '    GeneratedFunctionTableInitializer() {\n'
TAIL = '    }\n};\nstatic const GeneratedFunctionTableInitializer g_generatedFunctionTableInitializer;\n'
LINE = re.compile(r'^        g_ps2RecompiledFunctionTable\[(\d+)\] = ([A-Za-z_][A-Za-z0-9_]*);(?: // (.*))?$')
MARK = 'kGeneratedFunctionTableEntries'


def main():
    if len(sys.argv) != 2:
        sys.exit('usage: compact_function_table.py <runner dir>')
    path = os.path.join(sys.argv[1], 'register_functions.cpp')
    with open(path, 'rb') as f:
        raw = f.read()
    crlf = b'\r\n' in raw
    text = raw.decode('utf-8').replace('\r\n', '\n')
    if MARK in text:
        print('register_functions.cpp: already compact')
        return 0
    if text.count(HEAD) != 1 or text.count(TAIL) != 1:
        print('FAIL register_functions.cpp: the table constructor is not where it is expected')
        return 1
    start = text.index(HEAD) + len(HEAD)
    end = text.index(TAIL)
    if end < start:
        print('FAIL register_functions.cpp: constructor end before its start')
        return 1
    body = text[start:end].split('\n')
    if body and body[-1] == '':
        body.pop()
    rows = []
    for n, line in enumerate(body, 1):
        m = LINE.match(line)
        if not m:
            print(f'FAIL register_functions.cpp: constructor line {n} not a table entry: {line[:160]!r}')
            return 1
        slot, name, note = m.groups()
        rows.append(f'    {{{slot}u, {name}}},' + (f' // {note}' if note else ''))
    if not rows:
        print('FAIL register_functions.cpp: no table entries')
        return 1
    before = text[:text.index(HEAD)]
    # the anonymous namespace opens right before the struct; the entries go in it, before the struct
    struct_line = 'struct GeneratedFunctionTableInitializer {\n'
    if not before.endswith(struct_line):
        print('FAIL register_functions.cpp: unexpected text before the constructor')
        return 1
    before = before[:-len(struct_line)]
    out = (before +
           '// tools/compact_function_table.py: the entries as constant data + one loop (same order, same result)\n'
           'struct GeneratedFunctionTableEntry { uint32_t slot; PS2Runtime::RecompiledFunction function; };\n'
           f'const GeneratedFunctionTableEntry {MARK}[] = {{\n' + '\n'.join(rows) + '\n};\n' +
           struct_line + HEAD +
           f'        for (const GeneratedFunctionTableEntry &entry : {MARK})\n'
           '            g_ps2RecompiledFunctionTable[entry.slot] = entry.function;\n' +
           text[end:])
    if crlf:
        out = out.replace('\n', '\r\n')
    tmp = path + '.compact'
    with open(tmp, 'wb') as f:
        f.write(out.encode('utf-8'))
    os.replace(tmp, path)
    print(f'register_functions.cpp: {len(rows)} entries made a constant table')
    return 0


if __name__ == '__main__':
    sys.exit(main())
