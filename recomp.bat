@echo off
rem Recompiles game\SLUS_206.39 into generated\ (~2 min): builds config\vendetta.toml + vendetta_functions.csv
rem (tools\make_config.py), runs the recompiler and the VU1 recompiler (tools\vu1_recomp.py, the VU1 microprograms),
rem then copies only the changed files into PS2Recomp's runtime.
cd /d "%~dp0"
if not exist logs mkdir logs
python tools\make_config.py || exit /b 1
PS2Recomp\out\recomp\ps2xRecomp\ps2_recomp.exe config\vendetta.toml > logs\recomp.log 2>&1 || (echo recompile failed, see logs\recomp.log & exit /b 1)
python tools\vu1_recomp.py || exit /b 1
python tools\sync_generated.py
