@echo off
rem Builds the recompiled HCTP runner WITHOUT link-time optimisation (out\fast): optimised per file, quick to relink,
rem so fix-and-retry rounds take minutes. build-runtime.bat = the full-speed (LTCG) build, ~20 min link.
rem /DEBUG gives the exe a PDB (function names for the VENDETTA_HOSTPROF=2 code-address profiler); /OPT:REF /OPT:ICF as without it.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0PS2Recomp"
cmake -S . -B out\fast -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG /arch:AVX2" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /DNDEBUG" "-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF" -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF "-DPS2X_PARALLEL_GS_DIR=%~dp0third_party/parallel-gs" || exit /b 1
cmake --build out\fast || exit /b 1
