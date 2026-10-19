@echo off
rem Native renderer work build (branch native-renderer, out\native): the native Windows host (Win32 + Direct3D 11 GS
rem renderer + XInput + XAudio2, PS2X_NATIVE_HOST=ON) instead of raylib; no paraLLEl-GS. The fallback build (out\fast,
rem tag fallback-2026-10-14) is never overwritten. Run with run_hctp_native.bat (HCTP_RENDERER=cpu: CPU GS renderer).
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0PS2Recomp"
cmake -S . -B out\native -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG /arch:AVX2" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /DNDEBUG" "-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF" -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DPS2X_NATIVE_HOST=ON -DPS2X_DEFAULT_BOOT_ELF=SLUS_207.87 -DPS2X_ENABLE_DEBUG_UI=OFF "-DPS2X_PARALLEL_GS_DIR=" || exit /b 1
cmake --build out\native -j 8 || exit /b 1
