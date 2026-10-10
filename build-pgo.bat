@echo off
rem Profile-guided Release build. Usage:
rem   build-pgo.bat gen   instrumented build in runtime\Build\pgo-gen (run the game with it; profiles land in pgo\)
rem   build-pgo.bat use   optimised build in runtime\Build\x64-Clang-Release from pgo\merged.profdata
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "LLVM=%~dp0toolchain\clang+llvm-23.1.3-x86_64-pc-windows-msvc"
set "PATH=%LLVM%\bin;%PATH%"
set "LIB=%LLVM%\lib\clang\23\lib\windows;%LIB%"
set "PGO=%~dp0pgo"
set "PGO=%PGO:\=/%"
cd /d "%~dp0runtime"
if "%1"=="gen" (
  cmake --preset x64-Clang-Release -B Build\pgo-gen -DHAVE_ENDIAN_H=0 -DCMAKE_LINKER=lld-link "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS -fprofile-generate=%PGO%" "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc -fprofile-generate=%PGO%" || exit /b 1
  cmake --build Build\pgo-gen -- -j 3
) else (
  llvm-profdata merge -o "%PGO%/merged.profdata" "%PGO%" || exit /b 1
  cmake --preset x64-Clang-Release -DHAVE_ENDIAN_H=0 -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DCMAKE_LINKER=lld-link "-DCMAKE_C_FLAGS=/DWIN32 /D_WINDOWS -fprofile-use=%PGO%/merged.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" "-DCMAKE_CXX_FLAGS=/DWIN32 /D_WINDOWS /EHsc -fprofile-use=%PGO%/merged.profdata -Wno-profile-instr-unprofiled -Wno-profile-instr-out-of-date" || exit /b 1
  cmake --build Build\x64-Clang-Release -- -j 3
)
