@echo off
rem Makes the release zip a player downloads: dist\Def Jam Vendetta Recompiled.zip (nothing is published).
rem Needs git, the .NET 10 SDK and Python, and PS2Recomp built here once (its libraries come from PS2Recomp\out).
cd /d "%~dp0"
python tools\make_release.py || exit /b 1
