@echo off
rem Native renderer work build (branch native-renderer): same flags as build-fast.bat, separate folder out\native, so
rem the fallback build (out\fast, tag fallback-2026-10-14) is never overwritten. Run with run_hctp_native.bat.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0PS2Recomp"
cmake -S . -B out\native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG /arch:AVX2" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /DNDEBUG" "-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF" -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF "-DPS2X_PARALLEL_GS_DIR=%~dp0third_party/parallel-gs" || exit /b 1
cmake --build out\native || exit /b 1
