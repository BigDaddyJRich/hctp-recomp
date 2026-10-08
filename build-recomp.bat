@echo off
rem Builds PS2Recomp's recompiler (ps2xRecomp) + analyzer (ps2xAnalyzer) in Release with the VS 2022 Build Tools.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0PS2Recomp"
cmake -S . -B out\recomp -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RUNTIME=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_ANALYZER=ON || exit /b 1
cmake --build out\recomp || exit /b 1
