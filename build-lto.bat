@echo off
rem Release build with link-time optimisation, plus line tables in a PDB so that crash and hang
rem reports from play sessions name the code involved. Usage: build-lto.bat [-- -j N]
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
set "PATH=%~dp0toolchain\clang+llvm-23.1.3-x86_64-pc-windows-msvc\bin;%PATH%"
cd /d "%~dp0runtime"
cmake --preset x64-Clang-Release -DHAVE_ENDIAN_H=0 -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON -DCMAKE_LINKER=lld-link -DCMAKE_C_FLAGS="/DWIN32 /D_WINDOWS -gline-tables-only -gcodeview" -DCMAKE_CXX_FLAGS="/DWIN32 /D_WINDOWS /EHsc -gline-tables-only -gcodeview" -DCMAKE_EXE_LINKER_FLAGS="/machine:x64 /DEBUG /OPT:REF /OPT:ICF" || exit /b 1
cmake --build Build\x64-Clang-Release %*
