@echo off
rem Builds the recompiled Vendetta (ps2EntryRunner = PS2Recomp's runtime + the generated C++ in ps2xRuntime\src\runner).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0PS2Recomp"
cmake -S . -B out\runtime -G Ninja -DCMAKE_BUILD_TYPE=Release -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF "-DPS2X_PARALLEL_GS_DIR=%~dp0third_party/parallel-gs" || exit /b 1
cmake --build out\runtime || exit /b 1
