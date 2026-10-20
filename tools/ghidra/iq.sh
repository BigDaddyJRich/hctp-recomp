#!/bin/bash
# HCTP: usage: tools/ghidra/hq.sh <outfile> <queries...>   (Git Bash)
# Runs AdjmkQuery.java read-only against the analysed HCTP Ghidra project (F:\hctp-project\ghidra_hctp\project, hctp,
# SLUS_207.87; Ghidra 12.1.4 on JDK 21) and keeps only the script output.
# Queries: info | x:<addr>[-<addr>] | f:<addr> | c:<addr> | d:<addr>
#   e.g.  tools/ghidra/hq.sh /tmp/o.txt c:0x146518 d:0x151460     then read /tmp/o.txt
out="$1"; shift
scripts="$(cygpath -w "$(cd "$(dirname "$0")" && pwd)")"      # this folder, as a Windows path
cat > "$out.cmd" <<EOF
@echo off
set "JAVA_HOME=C:\\Program Files\\Eclipse Adoptium\\jdk-21.0.12.101-hotspot"
call F:\\hctp-project\\ghidra_12.1.4\\ghidra_12.1.4_PUBLIC\\support\\analyzeHeadless.bat F:\\hctp-project\\ghidra_iop iop -process iop_ram.bin -noanalysis -scriptPath $scripts -postScript AdjmkQuery.java $*
EOF
cmd //c "$(cygpath -w "$out.cmd")" < /dev/null > "$out.raw" 2>&1
sed -n '/ADJMK-BEGIN/,/ADJMK-END/p' "$out.raw" | sed 's/^INFO  AdjmkQuery.java> //; s/ (GhidraScript) *$//' > "$out"
grep -E "^ERROR [^P]|error:" "$out.raw" | head -3
wc -l < "$out"
