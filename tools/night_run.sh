#!/bin/sh
# usage: night_run.sh NAME SECONDS [VAR=val ...]  -> runs autotest, copies logs\auto to logs\runs\NAME
name=$1; secs=$2; shift 2
extra=""
for kv in "$@"; do extra="$extra,\"$kv\""; done
extra=${extra#,}
if [ -n "$extra" ]; then
  powershell -ExecutionPolicy Bypass -Command "& F:\hctp-newbase\tools\autotest.ps1 -Seconds $secs -Every 10 -Pad '20:START,30:START,45:CROSS' -Extra @($extra)"
else
  powershell -ExecutionPolicy Bypass -Command "& F:\hctp-newbase\tools\autotest.ps1 -Seconds $secs -Every 10 -Pad '20:START,30:START,45:CROSS'"
fi
rm -rf /f/hctp-newbase/logs/runs/$name; cp -r /f/hctp-newbase/logs/auto /f/hctp-newbase/logs/runs/$name
cd /f/hctp-newbase && python -I tools/audio_check.py logs/runs/$name/audio.pcm > logs/runs/$name/audio_check.txt 2>&1
echo done $name
