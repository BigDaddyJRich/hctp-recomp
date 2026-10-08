#!/bin/bash
# usage: tools/ghidra/gq.sh <outfile> <queries...>   (Git Bash)
# Runs AdjmkQuery.java read-only against the analysed Ghidra project and keeps only the script output.
# Needs Ghidra's headless launcher and adjmk's analysed project (C:\Tools\ghidra-headless.bat,
# C:\Tools\ghidra-projects\adjmk).
# Queries: info | x:<addr>[-<addr>] | f:<addr> | c:<addr> | d:<addr>
#   e.g.  tools/ghidra/gq.sh /tmp/o.txt c:0x146518 d:0x151460     then read /tmp/o.txt
out="$1"; shift
scripts="$(cygpath -w "$(cd "$(dirname "$0")" && pwd)")"      # this folder, as a Windows path
cmd //c "C:\Tools\ghidra-headless.bat C:\Tools\ghidra-projects adjmk -process SLUS_206.39 -noanalysis -readOnly -scriptPath $scripts -postScript AdjmkQuery.java $*" < /dev/null > "$out.raw" 2>&1
sed -n '/ADJMK-BEGIN/,/ADJMK-END/p' "$out.raw" | sed 's/^INFO  AdjmkQuery.java> //; s/ (GhidraScript) *$//' > "$out"
grep -E "^ERROR|error:" "$out.raw" | head -3
wc -l < "$out"
