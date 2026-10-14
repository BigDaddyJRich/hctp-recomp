@echo off
rem Diagnostic paraLLEl-GS build of the CURRENT source into out\diag (dependency sources copied from out\fast\_deps,
rem no network). Never touches out\fast (the fallback) or out\native.
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "%~dp0PS2Recomp"
cmake -S . -B out\diag -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=/O2 /Ob1 /DNDEBUG /arch:AVX2" "-DCMAKE_C_FLAGS_RELWITHDEBINFO=/O2 /DNDEBUG" "-DCMAKE_EXE_LINKER_FLAGS_RELWITHDEBINFO=/INCREMENTAL:NO /DEBUG /OPT:REF /OPT:ICF" -DPS2X_BUILD_RECOMP=OFF -DPS2X_BUILD_TEST=OFF -DPS2X_BUILD_STUDIO=OFF -DPS2X_BUILD_ANALYZER=OFF -DPS2X_ENABLE_AGRESSIVE_LOGS=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON "-DPS2X_PARALLEL_GS_DIR=%~dp0third_party/parallel-gs" || exit /b 1
cmake --build out\diag -j 2 || exit /b 1
