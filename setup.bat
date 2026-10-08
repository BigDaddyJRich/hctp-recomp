@echo off
rem One-time setup (again after updating PS2Recomp): fetch the PS2Recomp submodule, apply our runtime patch
rem (patches\ps2recomp-local.diff), build the recompiler + analyzer. Then: python tools\prepare_disc.py <your disc>.
cd /d "%~dp0"
git submodule update --init || exit /b 1
git -C PS2Recomp apply --reverse --check ..\patches\ps2recomp-local.diff 2>nul
if errorlevel 1 (
  git -C PS2Recomp apply ..\patches\ps2recomp-local.diff || (echo our PS2Recomp patch no longer applies to this PS2Recomp commit & exit /b 1)
  echo applied patches\ps2recomp-local.diff
) else (
  echo patches\ps2recomp-local.diff already applied
)
call "%~dp0build-recomp.bat"
